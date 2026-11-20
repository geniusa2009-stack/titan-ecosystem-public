//! TITAN Code document system.
//!
//! A [`Document`] couples a [`titan_text::TextDocument`] with the file it came from:
//!
//! * **Loading** rejects files above a size limit, binary files (NUL bytes), UTF-16 files
//!   (BOM) and invalid UTF-8 instead of silently corrupting them. A UTF-8 BOM is preserved.
//!   The size check and the read use the same open handle and the read is capped; a file
//!   that changes while it is read is read again.
//! * **Line endings** are never normalised. The text is kept byte-for-byte, so CRLF, LF and
//!   mixed files round-trip unchanged; [`LineEnding`] only reports what was found.
//! * **The disk version** a buffer is based on is recorded as length + BLAKE3
//!   [`ContentHash`] + `stat` stamp ([`DiskState`]). External modification is decided by
//!   content, not timestamps.
//! * **Saving** never silently overwrites a newer file: see [`save`](crate::save) for the
//!   validate → write temp → re-validate → atomic swap → verify sequence and its limits.
//!   On a conflict the buffer is untouched and the error says what happened.
//! * **Reconciliation** with the file system: [`read_disk`] takes a snapshot (run it off
//!   the latency-sensitive thread), [`Document::disk_matches`] compares it,
//!   [`Document::reload_from_disk`] replaces a buffer with it as one undoable transaction,
//!   and [`Document::adopt_disk_state`] keeps the buffer while acknowledging the new disk
//!   version (the next save then overwrites that exact version, and only that version).
//!
//! Symlinks are not followed here; callers pass the resolved real path (see
//! `titan-workspace`).

mod hash;
pub mod save;

use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use titan_text::{ByteOffset, Change, TextDocument, TextEdit, TextError};

pub use hash::{ContentHash, ContentHasher, hash_reader, stable_hash};
pub use save::{
    ConflictKind, DisplacedFile, Expected, FileStamp, ReplaceError, Replaced, SavePhase,
    SaveStrategy, replace_file,
};

const UTF8_BOM: &[u8] = &[0xEF, 0xBB, 0xBF];
const READ_ATTEMPTS: usize = 3;

/// Limits applied when loading files.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub max_file_bytes: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_file_bytes: 64 * 1024 * 1024,
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum DocError {
    #[error("I/O error on {path:?}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("{path:?} is {len} bytes, above the {limit}-byte limit")]
    TooLarge { path: PathBuf, len: u64, limit: u64 },
    #[error("{path:?} looks binary (contains NUL bytes)")]
    Binary { path: PathBuf },
    #[error("{path:?} uses an unsupported encoding ({encoding})")]
    UnsupportedEncoding {
        path: PathBuf,
        encoding: &'static str,
    },
    #[error("{path:?} is not valid UTF-8 (first invalid byte at offset {valid_up_to})")]
    NotUtf8 { path: PathBuf, valid_up_to: usize },
    #[error("{path:?} changed on disk since it was last loaded or saved ({kind})")]
    ExternalModification {
        path: PathBuf,
        kind: ConflictKind,
        /// Where displaced content was preserved, when a save could not put it back.
        preserved_copy: Option<PathBuf>,
    },
    #[error(transparent)]
    Text(#[from] TextError),
}

fn io_err(path: &Path) -> impl FnOnce(io::Error) -> DocError + '_ {
    move |source| DocError::Io {
        path: path.to_path_buf(),
        source,
    }
}

/// Line-ending style found in a file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LineEnding {
    Lf,
    Crlf,
    Cr,
    Mixed,
    /// The text contains no line break.
    Unknown,
}

impl LineEnding {
    pub fn detect(text: &str) -> Self {
        let bytes = text.as_bytes();
        let (mut lf, mut crlf, mut cr) = (0usize, 0usize, 0usize);
        let mut i = 0;
        while i < bytes.len() {
            match bytes[i] {
                b'\r' if bytes.get(i + 1) == Some(&b'\n') => {
                    crlf += 1;
                    i += 1;
                }
                b'\r' => cr += 1,
                b'\n' => lf += 1,
                _ => {}
            }
            i += 1;
        }
        match (lf > 0, crlf > 0, cr > 0) {
            (false, false, false) => Self::Unknown,
            (true, false, false) => Self::Lf,
            (false, true, false) => Self::Crlf,
            (false, false, true) => Self::Cr,
            _ => Self::Mixed,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Lf => "lf",
            Self::Crlf => "crlf",
            Self::Cr => "cr",
            Self::Mixed => "mixed",
            Self::Unknown => "unknown",
        }
    }
}

/// The disk version a document is based on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiskState {
    /// False when the document is known to have no file on disk (deleted, buffer kept).
    pub exists: bool,
    pub len: u64,
    pub hash: ContentHash,
    pub stamp: Option<FileStamp>,
}

impl DiskState {
    /// The state of a path with no file.
    pub fn missing() -> Self {
        Self {
            exists: false,
            len: 0,
            hash: ContentHash::of(b""),
            stamp: None,
        }
    }

    /// Same content (length and hash), or both absent.
    pub fn same_content(&self, other: &DiskState) -> bool {
        self.exists == other.exists
            && (!self.exists || (self.len == other.len && self.hash == other.hash))
    }
}

/// A file's bytes and state, read at one moment.
#[derive(Clone, Debug)]
pub struct DiskSnapshot {
    pub state: DiskState,
    /// Empty when the file does not exist.
    pub bytes: Vec<u8>,
}

/// Read at most `limit` bytes of `path`. The size check uses the open handle and the read
/// is capped, so a growing file cannot exceed the limit. If `fstat` before and after the
/// read differ, the file changed while it was read and the read is repeated.
fn read_limited(path: &Path, limit: u64) -> Result<(Vec<u8>, DiskState), DocError> {
    for _ in 0..READ_ATTEMPTS {
        let file = fs::File::open(path).map_err(io_err(path))?;
        let meta = file.metadata().map_err(io_err(path))?;
        if meta.len() > limit {
            return Err(DocError::TooLarge {
                path: path.to_path_buf(),
                len: meta.len(),
                limit,
            });
        }
        let before = FileStamp::from_metadata(&meta);
        let mut bytes = Vec::with_capacity(usize::try_from(meta.len()).unwrap_or(0));
        let mut handle = file;
        (&mut handle)
            .take(limit.saturating_add(1))
            .read_to_end(&mut bytes)
            .map_err(io_err(path))?;
        let len = bytes.len() as u64;
        if len > limit {
            return Err(DocError::TooLarge {
                path: path.to_path_buf(),
                len,
                limit,
            });
        }
        let after = handle
            .metadata()
            .map(|m| FileStamp::from_metadata(&m))
            .map_err(io_err(path))?;
        if after != before || after.len != len {
            continue;
        }
        let state = DiskState {
            exists: true,
            len,
            hash: ContentHash::of(&bytes),
            stamp: Some(after),
        };
        return Ok((bytes, state));
    }
    Err(DocError::Io {
        path: path.to_path_buf(),
        source: io::Error::other("the file kept changing while it was being read"),
    })
}

/// Snapshot the file at `path` (missing files give a `missing` state). Intended for worker
/// threads: it reads and hashes the whole file.
pub fn read_disk(path: &Path, limits: &Limits) -> Result<DiskSnapshot, DocError> {
    match read_limited(path, limits.max_file_bytes) {
        Ok((bytes, state)) => Ok(DiskSnapshot { state, bytes }),
        Err(DocError::Io { source, .. }) if source.kind() == io::ErrorKind::NotFound => {
            Ok(DiskSnapshot {
                state: DiskState::missing(),
                bytes: Vec::new(),
            })
        }
        Err(e) => Err(e),
    }
}

/// Result of a successful save.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SaveOutcome {
    pub revision: u64,
    pub bytes_written: u64,
    pub strategy: SaveStrategy,
}

/// A text file opened for editing.
#[derive(Debug)]
pub struct Document {
    path: PathBuf,
    text: TextDocument,
    has_bom: bool,
    line_ending: LineEnding,
    disk: DiskState,
    /// Content id of the buffer state that equals `disk`; `None` when no buffer state is
    /// known to match the disk (then the document is dirty).
    saved_content_id: Option<u64>,
    /// The version the last save replaced, kept open to catch late writers.
    displaced: Option<DisplacedFile>,
}

impl Document {
    /// Load `path`. The file must be UTF-8 text no larger than `limits.max_file_bytes`.
    pub fn open(path: &Path, limits: &Limits) -> Result<Self, DocError> {
        let (bytes, disk) = read_limited(path, limits.max_file_bytes)?;
        let text = decode(path, &bytes)?;
        let has_bom = bytes.starts_with(UTF8_BOM);
        let line_ending = LineEnding::detect(text);
        let doc = TextDocument::new(text);
        Ok(Self {
            path: path.to_path_buf(),
            saved_content_id: Some(doc.content_id()),
            text: doc,
            has_bom,
            line_ending,
            disk,
            displaced: None,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn text(&self) -> &TextDocument {
        &self.text
    }

    pub fn text_mut(&mut self) -> &mut TextDocument {
        &mut self.text
    }

    pub fn has_bom(&self) -> bool {
        self.has_bom
    }

    /// Line-ending style detected when the file was loaded.
    pub fn line_ending(&self) -> LineEnding {
        self.line_ending
    }

    pub fn disk_state(&self) -> DiskState {
        self.disk
    }

    /// True if the in-memory content differs from the disk version it is based on.
    pub fn is_dirty(&self) -> bool {
        self.saved_content_id != Some(self.text.content_id())
    }

    /// True if the file on disk no longer matches the recorded disk version (changed,
    /// replaced, deleted, or created when the document expected none). Streams the file.
    pub fn changed_on_disk(&self) -> Result<bool, DocError> {
        let mut file = match fs::File::open(&self.path) {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(self.disk.exists),
            Err(e) => return Err(io_err(&self.path)(e)),
        };
        if !self.disk.exists {
            return Ok(true);
        }
        let meta = file.metadata().map_err(io_err(&self.path))?;
        if meta.len() != self.disk.len {
            return Ok(true);
        }
        let (hash, _) = hash_reader(&mut file).map_err(io_err(&self.path))?;
        Ok(hash != self.disk.hash)
    }

    /// The file version the last save replaced (unlinked, still open), for late-write checks.
    pub fn take_displaced(&mut self) -> Option<DisplacedFile> {
        self.displaced.take()
    }

    /// True if `snapshot` describes the disk version this document is based on.
    pub fn disk_matches(&self, state: &DiskState) -> bool {
        self.disk.same_content(state)
    }

    /// The bytes that saving would write: optional BOM followed by the text, unmodified.
    pub fn encoded_bytes(&self) -> Vec<u8> {
        let buffer = self.text.buffer();
        let mut out = Vec::with_capacity(buffer.len_bytes() + UTF8_BOM.len());
        if self.has_bom {
            out.extend_from_slice(UTF8_BOM);
        }
        for chunk in buffer.chunks() {
            out.extend_from_slice(chunk.as_bytes());
        }
        out
    }

    /// Replace the buffer with the disk content in `snapshot` as one transaction (undo
    /// returns to the previous buffer). Afterwards the document is clean and based on
    /// `snapshot`. `None` when the text was already identical (only the disk state is
    /// updated). A missing file is refused: there is no content to load.
    pub fn reload_from_disk(
        &mut self,
        expected_revision: u64,
        snapshot: &DiskSnapshot,
    ) -> Result<Option<Change>, DocError> {
        check_revision(&self.text, expected_revision)?;
        if !snapshot.state.exists {
            return Err(DocError::Io {
                path: self.path.clone(),
                source: io::Error::from(io::ErrorKind::NotFound),
            });
        }
        let text = decode(&self.path, &snapshot.bytes)?;
        let change = if self.text.buffer().text() == text {
            None
        } else {
            let end = ByteOffset(self.text.buffer().len_bytes());
            Some(self.text.apply(
                expected_revision,
                vec![TextEdit::replace(ByteOffset(0), end, text)],
            )?)
        };
        self.has_bom = snapshot.bytes.starts_with(UTF8_BOM);
        self.line_ending = LineEnding::detect(text);
        self.disk = snapshot.state;
        self.saved_content_id = Some(self.text.content_id());
        Ok(change)
    }

    /// Keep the buffer but record `state` as the disk version it is based on, so the next
    /// save replaces exactly that version (and refuses any other). The document is clean
    /// only if its encoded bytes equal that version.
    pub fn adopt_disk_state(&mut self, state: DiskState) {
        self.disk = state;
        let bytes = self.encoded_bytes();
        let matches = state.exists
            && bytes.len() as u64 == state.len
            && ContentHash::of(&bytes) == state.hash;
        self.saved_content_id = matches.then(|| self.text.content_id());
    }

    /// Save conflict-safely (see [`save`](crate::save)).
    ///
    /// `expected_revision` must match the current revision. Unless `force` is set, the save
    /// is refused with [`DocError::ExternalModification`] if the file differs from the
    /// recorded disk version; the buffer is left untouched.
    pub fn save(&mut self, expected_revision: u64, force: bool) -> Result<SaveOutcome, DocError> {
        self.save_with_hook(expected_revision, force, &mut |_| {})
    }

    /// [`Document::save`] with a callback at fixed points of the sequence. Tests use it to
    /// modify the file concurrently at the worst possible moments.
    pub fn save_with_hook(
        &mut self,
        expected_revision: u64,
        force: bool,
        hook: &mut dyn FnMut(SavePhase),
    ) -> Result<SaveOutcome, DocError> {
        check_revision(&self.text, expected_revision)?;
        let expected = if force {
            Expected::Anything
        } else if self.disk.exists {
            Expected::Content {
                hash: self.disk.hash,
                len: self.disk.len,
            }
        } else {
            Expected::Absent
        };
        let bytes = self.encoded_bytes();
        let replaced =
            save::replace_file(&self.path, &bytes, expected, hook).map_err(|e| match e {
                ReplaceError::Conflict {
                    kind,
                    preserved_copy,
                } => DocError::ExternalModification {
                    path: self.path.clone(),
                    kind,
                    preserved_copy,
                },
                ReplaceError::Io(source) => DocError::Io {
                    path: self.path.clone(),
                    source,
                },
            })?;
        self.disk = DiskState {
            exists: true,
            len: bytes.len() as u64,
            hash: ContentHash::of(&bytes),
            stamp: replaced.stamp,
        };
        self.saved_content_id = Some(self.text.content_id());
        self.displaced = replaced.displaced;
        Ok(SaveOutcome {
            revision: expected_revision,
            bytes_written: bytes.len() as u64,
            strategy: replaced.strategy,
        })
    }
}

fn check_revision(text: &TextDocument, expected: u64) -> Result<(), TextError> {
    let current = text.revision();
    if expected != current {
        return Err(TextError::StaleRevision { expected, current });
    }
    Ok(())
}

fn decode<'a>(path: &Path, bytes: &'a [u8]) -> Result<&'a str, DocError> {
    if bytes.starts_with(&[0xFF, 0xFE]) || bytes.starts_with(&[0xFE, 0xFF]) {
        return Err(DocError::UnsupportedEncoding {
            path: path.to_path_buf(),
            encoding: "UTF-16",
        });
    }
    if bytes.contains(&0) {
        return Err(DocError::Binary {
            path: path.to_path_buf(),
        });
    }
    let body = bytes.strip_prefix(UTF8_BOM).unwrap_or(bytes);
    std::str::from_utf8(body).map_err(|e| DocError::NotUtf8 {
        path: path.to_path_buf(),
        valid_up_to: e.valid_up_to(),
    })
}

#[cfg(test)]
mod tests;
