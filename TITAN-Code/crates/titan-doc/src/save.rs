//! Conflict-safe replacement of a file's content.
//!
//! ## Strategy
//! 1. **Validate** the file on disk against the version the buffer was based on: open it,
//!    `fstat` it, stream its content through BLAKE3, `fstat` again (a change while reading
//!    is a conflict). Missing or different ⇒ conflict, nothing written.
//! 2. **Write** the new content to a temporary file in the same directory, `fsync` it,
//!    copy the old permissions.
//! 3. **Re-validate** right before replacing: `stat` the path and compare its identity
//!    stamp (device, inode, size, mtime, ctime) with the one validated in step 1.
//! 4. **Replace**:
//!    * Linux (`renameat2(RENAME_EXCHANGE)`) and macOS (`renameatx_np(RENAME_SWAP)`):
//!      atomically **swap** the temporary file and the target. Nothing is unlinked by the
//!      swap: the previous file is now at the temporary path. Its content is hashed again;
//!      if it is not exactly the validated version (someone changed or replaced the file
//!      between step 3 and the swap), the two are swapped back and the save reports a
//!      conflict. If even the swap back cannot be confirmed, the displaced file is kept as
//!      a `*.titan-conflict-<ms>` copy next to the target and its path is reported. The
//!      other writer's bytes are therefore never deleted.
//!    * Elsewhere, or on file systems that reject the swap (`EINVAL`, `ENOTSUP`, `ENOSYS`):
//!      plain `rename`. The window between step 3 and the rename (microseconds) remains;
//!      a change made exactly then is overwritten. This is reported as
//!      [`SaveStrategy::Rename`] so callers know which guarantee they got.
//!    * When the document expects the file to be absent (it was deleted and the user chose
//!      to keep the buffer): a no-clobber rename (`link`/`RENAME_NOREPLACE`), so a file
//!      created by someone else in the meantime is never overwritten.
//! 5. `fsync` the directory.
//!
//! ## Writers holding the old file open
//! A process that opened the file before the save and keeps writing to it afterwards
//! writes into the displaced inode, not into the new file. The displaced file is unlinked
//! from the directory but **kept open** ([`DisplacedFile`]): the caller checks it for a
//! grace period (`fstat` is cheap) and, if it changed, reads the late bytes through the
//! descriptor and preserves them. Writes after the grace period ends are not seen; that is
//! the guarantee boundary (documented in ADR 0010).
//!
//! ## Limits (operating-system level)
//! * Hard links: the swap replaces this directory entry only; other links keep the old
//!   content (same as every atomic-rename editor).
//! * The comparison is by content: a file rewritten with identical bytes is not a conflict.

use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::hash::{ContentHash, hash_reader};

/// Identity and timestamps of a file as reported by `stat`. Used for cheap "did anything
/// touch this file" checks; equality is necessary but not sufficient for "unchanged"
/// (timestamps can be coarse), so decisions also compare content hashes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileStamp {
    pub len: u64,
    pub dev: u64,
    pub ino: u64,
    pub modified_ns: Option<i128>,
    pub changed_ns: Option<i128>,
}

impl FileStamp {
    #[cfg(unix)]
    pub fn from_metadata(meta: &fs::Metadata) -> Self {
        use std::os::unix::fs::MetadataExt;
        Self {
            len: meta.len(),
            dev: meta.dev(),
            ino: meta.ino(),
            modified_ns: Some(
                i128::from(meta.mtime()) * 1_000_000_000 + i128::from(meta.mtime_nsec()),
            ),
            changed_ns: Some(
                i128::from(meta.ctime()) * 1_000_000_000 + i128::from(meta.ctime_nsec()),
            ),
        }
    }

    #[cfg(not(unix))]
    pub fn from_metadata(meta: &fs::Metadata) -> Self {
        let modified_ns = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_nanos() as i128);
        Self {
            len: meta.len(),
            dev: 0,
            ino: 0,
            modified_ns,
            changed_ns: None,
        }
    }

    pub fn of_path(path: &Path) -> io::Result<Self> {
        fs::metadata(path).map(|m| Self::from_metadata(&m))
    }
}

/// How the new content replaced the old file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SaveStrategy {
    /// Atomic swap followed by verification of the displaced file; no lost updates before
    /// the swap.
    Exchange,
    /// Atomic rename after a final check; a change in the last microseconds can be lost.
    Rename,
    /// The target did not exist; created without overwriting anything.
    CreateNew,
}

impl SaveStrategy {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Exchange => "exchange",
            Self::Rename => "rename",
            Self::CreateNew => "createNew",
        }
    }
}

/// Why a save was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConflictKind {
    /// The file's content differs from the version the buffer was based on.
    Modified,
    /// The file was deleted (or replaced by something that is not a regular file).
    Deleted,
    /// The file appeared although the document expected it to be absent.
    Created,
    /// The file changed while the save was in progress (after the first validation).
    ModifiedDuringSave,
}

impl ConflictKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Modified => "modified",
            Self::Deleted => "deleted",
            Self::Created => "created",
            Self::ModifiedDuringSave => "modifiedDuringSave",
        }
    }
}

impl std::fmt::Display for ConflictKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Points in a save where tests inject concurrent modifications.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SavePhase {
    /// The disk content was validated; the temporary file is not written yet.
    Validated,
    /// The temporary file is written and synced; the final check passed; the next step
    /// replaces the target.
    BeforeReplace,
}

/// What the caller expects to find at the path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Expected {
    /// A file with this content.
    Content { hash: ContentHash, len: u64 },
    /// No file.
    Absent,
    /// Anything (forced save).
    Anything,
}

/// Why [`replace_file`] did not replace the file.
#[derive(Debug)]
pub enum ReplaceError {
    Conflict {
        kind: ConflictKind,
        preserved_copy: Option<PathBuf>,
    },
    Io(io::Error),
}

impl From<io::Error> for ReplaceError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

fn conflict(kind: ConflictKind) -> ReplaceError {
    ReplaceError::Conflict {
        kind,
        preserved_copy: None,
    }
}

/// Validate `path` against `expected`; return the stamp that was validated (None: absent).
fn validate(path: &Path, expected: Expected) -> Result<Option<FileStamp>, ReplaceError> {
    let mut file = match File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            return match expected {
                Expected::Content { .. } => Err(conflict(ConflictKind::Deleted)),
                Expected::Absent | Expected::Anything => Ok(None),
            };
        }
        Err(e) => return Err(e.into()),
    };
    let before = file.metadata()?;
    if !before.is_file() {
        return Err(conflict(ConflictKind::Deleted));
    }
    let stamp = FileStamp::from_metadata(&before);
    match expected {
        Expected::Anything => Ok(Some(stamp)),
        Expected::Absent => Err(conflict(ConflictKind::Created)),
        Expected::Content { hash, len } => {
            if stamp.len != len {
                return Err(conflict(ConflictKind::Modified));
            }
            let (actual, read) = hash_reader(&mut file)?;
            let after = FileStamp::from_metadata(&file.metadata()?);
            if after != stamp || read != len {
                return Err(conflict(ConflictKind::ModifiedDuringSave));
            }
            if actual != hash {
                return Err(conflict(ConflictKind::Modified));
            }
            Ok(Some(stamp))
        }
    }
}

fn hash_file(path: &Path) -> io::Result<(ContentHash, u64)> {
    let mut file = File::open(path)?;
    hash_reader(&mut file)
}

fn unix_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0)
}

fn conflict_copy_path(target: &Path) -> PathBuf {
    let name = target
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "file".to_string());
    target.with_file_name(format!("{name}.titan-conflict-{}", unix_ms()))
}

/// Outcome of a successful replacement.
#[derive(Debug)]
pub struct Replaced {
    pub strategy: SaveStrategy,
    pub stamp: Option<FileStamp>,
    /// The previous version, unlinked but still open (see [`DisplacedFile`]). `None` when
    /// there was no previous file.
    pub displaced: Option<DisplacedFile>,
}

/// The file version a save replaced: already removed from the directory, but kept open so
/// writes that other programs make through descriptors they opened before the save are
/// not silently lost.
#[derive(Debug)]
pub struct DisplacedFile {
    file: File,
    /// `fstat` right after the replacement (link count changes already applied).
    stamp: FileStamp,
    /// Content hash at replacement time.
    hash: ContentHash,
    since: std::time::Instant,
}

impl DisplacedFile {
    fn new(file: File, hash: ContentHash) -> io::Result<Self> {
        let stamp = FileStamp::from_metadata(&file.metadata()?);
        Ok(Self {
            file,
            stamp,
            hash,
            since: std::time::Instant::now(),
        })
    }

    /// Time since the replacement.
    pub fn age(&self) -> std::time::Duration {
        self.since.elapsed()
    }

    /// Cheap check (`fstat` on the open descriptor): size or timestamps changed since the
    /// replacement, so someone probably wrote to the old version.
    pub fn touched(&self) -> io::Result<bool> {
        let now = FileStamp::from_metadata(&self.file.metadata()?);
        Ok(now.len != self.stamp.len
            || now.modified_ns != self.stamp.modified_ns
            || now.changed_ns != self.stamp.changed_ns)
    }

    /// Full check: read the old version through the descriptor; returns its bytes when they
    /// differ from what was last seen (the replaced version, or the last reported late
    /// write), `None` when unchanged. Each late write is reported once.
    pub fn late_write(&mut self, limit: u64) -> io::Result<Option<Vec<u8>>> {
        use std::io::{Read, Seek, SeekFrom};
        self.file.seek(SeekFrom::Start(0))?;
        let mut bytes = Vec::new();
        (&mut self.file).take(limit).read_to_end(&mut bytes)?;
        let hash = ContentHash::of(&bytes);
        self.stamp = FileStamp::from_metadata(&self.file.metadata()?);
        if hash == self.hash {
            return Ok(None);
        }
        self.hash = hash;
        Ok(Some(bytes))
    }
}

/// Replace the content of `path` with `bytes` under the strategy in the module docs. Used by
/// [`crate::Document::save`] and by multi-file change application (`titan-changes`).
pub fn replace_file(
    path: &Path,
    bytes: &[u8],
    expected: Expected,
    hook: &mut dyn FnMut(SavePhase),
) -> Result<Replaced, ReplaceError> {
    let observed = validate(path, expected)?;
    hook(SavePhase::Validated);

    let dir = match path.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent,
        _ => Path::new("."),
    };
    let mut tmp = tempfile::Builder::new()
        .prefix(".titan-save-")
        .tempfile_in(dir)?;
    tmp.write_all(bytes)?;
    tmp.flush()?;
    if let Ok(meta) = fs::metadata(path) {
        fs::set_permissions(tmp.path(), meta.permissions())?;
    }
    tmp.as_file().sync_all()?;
    let tmp_path = tmp.into_temp_path();

    // Final cheap check right before replacing.
    let now = match FileStamp::of_path(path) {
        Ok(stamp) => Some(stamp),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.into()),
    };
    if !matches!(expected, Expected::Anything) && now != observed {
        return Err(conflict(ConflictKind::ModifiedDuringSave));
    }
    hook(SavePhase::BeforeReplace);

    let mut displaced = None;
    let strategy = match (expected, observed) {
        (Expected::Absent, _) | (Expected::Anything, None) => {
            tmp_path.persist_noclobber(path).map_err(|e| {
                if e.error.kind() == io::ErrorKind::AlreadyExists {
                    conflict(ConflictKind::Created)
                } else {
                    ReplaceError::Io(e.error)
                }
            })?;
            SaveStrategy::CreateNew
        }
        (Expected::Content { hash, len }, _) => match exchange(&tmp_path, path) {
            Ok(()) => {
                // The previous file now sits at `tmp_path`. Verify it is the validated one,
                // through a descriptor we keep.
                let opened = File::open(&tmp_path).and_then(|mut f| {
                    let (h, n) = hash_reader(&mut f)?;
                    Ok((f, h, n))
                });
                let (file, h, n) = match opened {
                    Ok(v) => v,
                    Err(_) => return Err(swap_back(tmp_path, path, bytes)),
                };
                if h != hash || n != len {
                    drop(file);
                    return Err(swap_back(tmp_path, path, bytes));
                }
                drop(tmp_path); // unlinks the verified old version; `file` keeps it readable
                displaced = DisplacedFile::new(file, hash).ok();
                SaveStrategy::Exchange
            }
            Err(e) if exchange_unsupported(&e) => {
                // Keep a descriptor on the version the rename will displace.
                let old = File::open(path).ok();
                tmp_path
                    .persist(path)
                    .map_err(|e| ReplaceError::Io(e.error))?;
                displaced = old.and_then(|f| DisplacedFile::new(f, hash).ok());
                SaveStrategy::Rename
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                return Err(conflict(ConflictKind::Deleted));
            }
            Err(e) => return Err(e.into()),
        },
        (Expected::Anything, Some(_)) => match exchange(&tmp_path, path) {
            Ok(()) => {
                drop(tmp_path);
                SaveStrategy::Exchange
            }
            Err(e) if exchange_unsupported(&e) || e.kind() == io::ErrorKind::NotFound => {
                tmp_path
                    .persist(path)
                    .map_err(|e| ReplaceError::Io(e.error))?;
                SaveStrategy::Rename
            }
            Err(e) => return Err(e.into()),
        },
    };
    sync_dir(dir)?;
    Ok(Replaced {
        strategy,
        stamp: FileStamp::of_path(path).ok(),
        displaced,
    })
}

/// The swap displaced a file that was not the validated version: put it back. Never
/// deletes anything that is not verifiably our own content.
fn swap_back(tmp_path: tempfile::TempPath, target: &Path, ours: &[u8]) -> ReplaceError {
    let ours_hash = ContentHash::of(ours);
    if exchange(&tmp_path, target).is_ok() {
        // `tmp_path` should now hold our content again.
        if matches!(hash_file(&tmp_path), Ok((h, n)) if h == ours_hash && n == ours.len() as u64) {
            drop(tmp_path);
            return conflict(ConflictKind::ModifiedDuringSave);
        }
    }
    // Could not confirm: keep whatever is at the temporary path as a visible copy.
    let preserved = conflict_copy_path(target);
    match tmp_path.keep() {
        Ok(kept) => {
            let preserved_copy = if fs::rename(&kept, &preserved).is_ok() {
                Some(preserved)
            } else {
                Some(kept)
            };
            ReplaceError::Conflict {
                kind: ConflictKind::ModifiedDuringSave,
                preserved_copy,
            }
        }
        Err(e) => ReplaceError::Io(e.error),
    }
}

#[cfg(any(target_os = "linux", target_os = "android", target_vendor = "apple"))]
fn exchange(a: &Path, b: &Path) -> io::Result<()> {
    use rustix::fs::{CWD, RenameFlags, renameat_with};
    renameat_with(CWD, a, CWD, b, RenameFlags::EXCHANGE).map_err(io::Error::from)
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_vendor = "apple")))]
fn exchange(_a: &Path, _b: &Path) -> io::Result<()> {
    Err(io::Error::from(io::ErrorKind::Unsupported))
}

fn exchange_unsupported(e: &io::Error) -> bool {
    if e.kind() == io::ErrorKind::Unsupported {
        return true;
    }
    #[cfg(any(target_os = "linux", target_os = "android", target_vendor = "apple"))]
    {
        use rustix::io::Errno;
        if let Some(code) = e.raw_os_error() {
            let errno = Errno::from_raw_os_error(code);
            return errno == Errno::INVAL
                || errno == Errno::NOSYS
                || errno == Errno::NOTSUP
                || errno == Errno::OPNOTSUPP
                || errno == Errno::XDEV;
        }
    }
    false
}

#[cfg(unix)]
fn sync_dir(dir: &Path) -> io::Result<()> {
    File::open(dir)?.sync_all()
}

#[cfg(not(unix))]
fn sync_dir(_dir: &Path) -> io::Result<()> {
    Ok(())
}
