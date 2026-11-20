//! TITAN Code crash-recovery journal.
//!
//! Atomic saves protect files on disk; this protects **unsaved** edits. While a document is
//! dirty, every committed change is appended to a per-document journal in a state
//! directory outside the workspace. After a crash or kill, the journal is inspected and the
//! unsaved text can be restored into an editor buffer. The user's file is never written by
//! recovery itself.
//!
//! ## File format
//! ```text
//! "TITANJ1\n"                                   8-byte magic
//! repeated: u32 LE length | u32 LE CRC-32 | JSON payload
//! ```
//! Payloads: `base` (document path, disk length + content hash, BOM, starting revision),
//! `checkpoint` (full text at a revision), `edits` (one committed change, revision `from` to
//! `to`). A journal starts with `base`, normally followed by a `checkpoint` of the loaded text
//! so recovery does not depend on the file staying unchanged. Compaction rewrites the file as
//! `base` + `checkpoint` (temp file + fsync + rename).
//!
//! Format version 2 records the disk content as `blake3:<hex>` (a collision-resistant
//! [`ContentHash`]); version 1 journals (FNV-1a, 16 hex digits) are still read.
//!
//! ## Durability
//! Two different guarantees:
//! * **Process crash / kill** (`SIGKILL`, panic, OOM kill): nothing acknowledged is lost.
//!   Each record is handed to the kernel with one `write` call before the edit is
//!   acknowledged, and the kernel keeps it even if the process dies the next instant.
//! * **OS crash / power loss**: records reach stable storage only at `fsync`, which is
//!   batched ([`JournalWriter::sync`], every 200 ms in `titand`). The most recent, not yet
//!   synced records can be lost; the journal stays readable (a torn tail is detected).
//!
//! If a write fails (for example the disk is full), the partial record is truncated away
//! so the journal stays valid up to the last complete record, and the error is returned;
//! the caller decides whether to retry later with a fresh journal.
//!
//! ## Reading
//! [`inspect`] never panics and never trusts the file: a short final record (interrupted
//! write) is reported as a truncated tail; a checksum or JSON error, a revision gap or an
//! edit that does not apply stops reconstruction at the last valid record and is reported as
//! corruption. The reconstructed text is compared with the file on disk to decide whether
//! restoring would be safe (`recoverable`) or would discard newer external content
//! (`conflict`).

use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use titan_doc::{ContentHash, Document, Limits, stable_hash};
use titan_text::{ByteOffset, Change, ChangeKind, TextBuffer, TextEdit};

pub const MAGIC: &[u8; 8] = b"TITANJ1\n";
pub const FORMAT_VERSION: u32 = 2;
/// Oldest format version that can still be read.
pub const MIN_FORMAT_VERSION: u32 = 1;
pub const JOURNAL_EXTENSION: &str = "tjournal";
const RECORD_HEADER: usize = 8;

#[derive(Clone, Debug)]
pub struct JournalConfig {
    pub dir: PathBuf,
    /// Documents up to this size get a checkpoint of their loaded text when the journal is
    /// created. Larger ones rely on the file on disk being unchanged until compaction.
    pub initial_checkpoint_max_bytes: u64,
    /// Compact once the journal grows past this many bytes.
    pub compact_after_bytes: u64,
    /// Journals larger than this are not read.
    pub max_journal_bytes: u64,
    /// Inactive journals older than this are deleted by [`JournalStore::cleanup`].
    pub retention: Duration,
    /// Total size budget for inactive journals; the oldest are deleted beyond it.
    pub max_total_bytes: u64,
}

impl JournalConfig {
    pub fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            initial_checkpoint_max_bytes: 4 * 1024 * 1024,
            compact_after_bytes: 8 * 1024 * 1024,
            max_journal_bytes: 512 * 1024 * 1024,
            retention: Duration::from_secs(14 * 24 * 3600),
            max_total_bytes: 1024 * 1024 * 1024,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
struct JournalEdit {
    start: usize,
    end: usize,
    text: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum Record {
    Base {
        format_version: u32,
        path: String,
        disk_len: u64,
        disk_hash: String,
        has_bom: bool,
        base_revision: u64,
        created_unix_ms: u64,
        instance: String,
    },
    Checkpoint {
        revision: u64,
        text: String,
    },
    Edits {
        from_revision: u64,
        to_revision: u64,
        kind: String,
        edits: Vec<JournalEdit>,
    },
}

fn encode(record: &Record) -> io::Result<Vec<u8>> {
    let payload = serde_json::to_vec(record).map_err(io::Error::other)?;
    let len = u32::try_from(payload.len())
        .map_err(|_| io::Error::other("journal record larger than 4 GiB"))?;
    let mut out = Vec::with_capacity(RECORD_HEADER + payload.len());
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(&crc32fast::hash(&payload).to_le_bytes());
    out.extend_from_slice(&payload);
    Ok(out)
}

fn unix_ms(time: SystemTime) -> u64 {
    time.duration_since(UNIX_EPOCH)
        .map(|d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
        .unwrap_or(0)
}

fn kind_name(kind: ChangeKind) -> &'static str {
    match kind {
        ChangeKind::Edit => "edit",
        ChangeKind::Undo => "undo",
        ChangeKind::Redo => "redo",
    }
}

#[cfg(unix)]
fn sync_dir(dir: &Path) -> io::Result<()> {
    File::open(dir)?.sync_all()
}

#[cfg(not(unix))]
fn sync_dir(_dir: &Path) -> io::Result<()> {
    Ok(())
}

/// Identity of the document a journal belongs to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BaseInfo {
    pub path: PathBuf,
    pub disk_len: u64,
    pub disk_hash: ContentHash,
    pub has_bom: bool,
    pub base_revision: u64,
}

impl BaseInfo {
    pub fn from_document(doc: &Document) -> Self {
        let disk = doc.disk_state();
        Self {
            path: doc.path().to_path_buf(),
            disk_len: disk.len,
            disk_hash: disk.hash,
            has_bom: doc.has_bom(),
            base_revision: doc.text().revision(),
        }
    }

    fn record(&self, instance: &str) -> Record {
        Record::Base {
            format_version: FORMAT_VERSION,
            path: self.path.to_string_lossy().into_owned(),
            disk_len: self.disk_len,
            disk_hash: format!("blake3:{}", self.disk_hash.to_hex()),
            has_bom: self.has_bom,
            base_revision: self.base_revision,
            created_unix_ms: unix_ms(SystemTime::now()),
            instance: instance.to_string(),
        }
    }
}

/// Appends changes for one open document.
#[derive(Debug)]
pub struct JournalWriter {
    file: File,
    path: PathBuf,
    dir: PathBuf,
    base: BaseInfo,
    instance: String,
    bytes: u64,
    records: u64,
    last_revision: u64,
    needs_sync: bool,
    compact_after_bytes: u64,
}

impl JournalWriter {
    /// Create a new journal for `base`. `text` is the buffer at `base.base_revision`; it is
    /// checkpointed when not larger than the configured limit.
    pub fn create(config: &JournalConfig, base: BaseInfo, text: &TextBuffer) -> io::Result<Self> {
        Self::create_inner(config, base, text, false)
    }

    /// Like [`JournalWriter::create`], but always checkpoints `text`, whatever its size. Use
    /// it when `text` is not the disk content (the buffer already had unsaved changes), so
    /// recovery never depends on replaying edits onto the wrong base.
    pub fn create_checkpointed(
        config: &JournalConfig,
        base: BaseInfo,
        text: &TextBuffer,
    ) -> io::Result<Self> {
        Self::create_inner(config, base, text, true)
    }

    fn create_inner(
        config: &JournalConfig,
        base: BaseInfo,
        text: &TextBuffer,
        force_checkpoint: bool,
    ) -> io::Result<Self> {
        fs::create_dir_all(&config.dir)?;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let instance = format!("{}-{now:x}", std::process::id());
        let name = format!(
            "{:016x}-{instance}.{JOURNAL_EXTENSION}",
            stable_hash(base.path.to_string_lossy().as_bytes())
        );
        let path = config.dir.join(name);
        let mut bytes = MAGIC.to_vec();
        bytes.extend(encode(&base.record(&instance))?);
        let mut records = 1;
        if force_checkpoint || text.len_bytes() as u64 <= config.initial_checkpoint_max_bytes {
            bytes.extend(encode(&Record::Checkpoint {
                revision: base.base_revision,
                text: text.text(),
            })?);
            records += 1;
        }
        let mut file = OpenOptions::new()
            .create_new(true)
            .append(true)
            .open(&path)?;
        let written = file
            .write_all(&bytes)
            .and_then(|()| file.sync_data())
            .and_then(|()| sync_dir(&config.dir));
        if let Err(e) = written {
            // Never leave a headerless or torn journal behind (for example on a full disk).
            drop(file);
            let _ = fs::remove_file(&path);
            return Err(e);
        }
        Ok(Self {
            file,
            path,
            dir: config.dir.clone(),
            last_revision: base.base_revision,
            base,
            instance,
            bytes: bytes.len() as u64,
            records,
            needs_sync: false,
            compact_after_bytes: config.compact_after_bytes,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The journal's id (its file name), as used by [`JournalStore`].
    pub fn id(&self) -> String {
        self.path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    pub fn len_bytes(&self) -> u64 {
        self.bytes
    }

    pub fn last_revision(&self) -> u64 {
        self.last_revision
    }

    /// Append one committed change. It must directly follow the last journaled revision.
    pub fn append(&mut self, change: &Change) -> io::Result<()> {
        if change.previous_revision != self.last_revision {
            return Err(io::Error::other(format!(
                "journal gap: last revision {}, change from {}",
                self.last_revision, change.previous_revision
            )));
        }
        let record = Record::Edits {
            from_revision: change.previous_revision,
            to_revision: change.revision,
            kind: kind_name(change.kind).to_string(),
            edits: change
                .edits
                .iter()
                .map(|e| JournalEdit {
                    start: e.start.0,
                    end: e.end.0,
                    text: e.text.clone(),
                })
                .collect(),
        };
        let bytes = encode(&record)?;
        // One write per record: either the whole record reaches the OS or the reader sees a
        // short tail.
        if let Err(e) = self.file.write_all(&bytes) {
            // Remove a partial record so the journal stays valid up to the last complete
            // one (best effort: if even this fails, readers detect the torn tail).
            let _ = self.file.set_len(self.bytes);
            return Err(e);
        }
        self.bytes += bytes.len() as u64;
        self.records += 1;
        self.last_revision = change.revision;
        self.needs_sync = true;
        Ok(())
    }

    pub fn needs_compaction(&self) -> bool {
        self.bytes > self.compact_after_bytes
    }

    /// Replace the journal with `base` + a checkpoint of `text` at `revision`, atomically.
    pub fn compact(&mut self, revision: u64, text: &TextBuffer) -> io::Result<()> {
        let mut bytes = MAGIC.to_vec();
        bytes.extend(encode(&self.base.record(&self.instance))?);
        bytes.extend(encode(&Record::Checkpoint {
            revision,
            text: text.text(),
        })?);
        let mut tmp = tempfile::Builder::new()
            .prefix(".compact-")
            .tempfile_in(&self.dir)?;
        tmp.write_all(&bytes)?;
        tmp.as_file().sync_all()?;
        tmp.persist(&self.path).map_err(|e| e.error)?;
        sync_dir(&self.dir)?;
        self.file = OpenOptions::new().append(true).open(&self.path)?;
        self.bytes = bytes.len() as u64;
        self.records = 2;
        self.last_revision = revision;
        self.needs_sync = false;
        Ok(())
    }

    /// `fsync` records written since the last sync.
    pub fn sync(&mut self) -> io::Result<()> {
        if self.needs_sync {
            self.file.sync_data()?;
            self.needs_sync = false;
        }
        Ok(())
    }

    /// Delete the journal (after a save or an explicit discard).
    pub fn remove(self) -> io::Result<()> {
        let path = self.path.clone();
        drop(self);
        match fs::remove_file(&path) {
            Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
            _ => Ok(()),
        }
    }
}

/// How the file on disk compares with the journal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiskComparison {
    /// Disk content equals what the journal started from.
    MatchesBase,
    /// Disk already contains the recovered text.
    MatchesRecovered,
    /// Disk changed since the journal started (by someone else).
    ChangedSinceBase,
    Missing,
    Unreadable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryState {
    /// Unsaved text can be restored; the disk still holds the content it was based on.
    Recoverable,
    /// Unsaved text exists, but the file changed or vanished since; restoring needs an
    /// explicit decision.
    Conflict,
    /// Nothing to restore: no edits, or the disk already has this text.
    NoChanges,
    /// The journal cannot be reconstructed.
    Unrecoverable,
}

impl RecoveryState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Recoverable => "recoverable",
            Self::Conflict => "conflict",
            Self::NoChanges => "noChanges",
            Self::Unrecoverable => "unrecoverable",
        }
    }
}

impl DiskComparison {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::MatchesBase => "matchesBase",
            Self::MatchesRecovered => "matchesRecovered",
            Self::ChangedSinceBase => "changedSinceBase",
            Self::Missing => "missing",
            Self::Unreadable => "unreadable",
        }
    }
}

#[derive(Clone, Debug)]
pub struct JournalReport {
    pub journal_id: String,
    pub journal_path: PathBuf,
    pub document_path: Option<PathBuf>,
    pub instance: Option<String>,
    pub created_unix_ms: Option<u64>,
    pub base_revision: Option<u64>,
    pub recovered_revision: Option<u64>,
    pub valid_records: usize,
    pub edit_records: usize,
    /// The last record was cut short (interrupted write). Earlier records are intact.
    pub truncated_tail: bool,
    /// Why reading stopped early (checksum, JSON, revision gap, edit out of range).
    pub corruption: Option<String>,
    pub disk: DiskComparison,
    pub state: RecoveryState,
    pub reason: Option<String>,
    pub recovered_text: Option<String>,
    pub has_bom: bool,
}

impl JournalReport {
    fn unrecoverable(id: String, path: &Path, reason: String) -> Self {
        Self {
            journal_id: id,
            journal_path: path.to_path_buf(),
            document_path: None,
            instance: None,
            created_unix_ms: None,
            base_revision: None,
            recovered_revision: None,
            valid_records: 0,
            edit_records: 0,
            truncated_tail: false,
            corruption: None,
            disk: DiskComparison::Unreadable,
            state: RecoveryState::Unrecoverable,
            reason: Some(reason),
            recovered_text: None,
            has_bom: false,
        }
    }
}

struct ParsedJournal {
    records: Vec<Record>,
    truncated_tail: bool,
    corruption: Option<String>,
}

fn parse_records(bytes: &[u8]) -> ParsedJournal {
    let mut out = ParsedJournal {
        records: Vec::new(),
        truncated_tail: false,
        corruption: None,
    };
    let mut pos = MAGIC.len();
    while pos < bytes.len() {
        let rest = &bytes[pos..];
        if rest.len() < RECORD_HEADER {
            out.truncated_tail = true;
            break;
        }
        let len = u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]) as usize;
        let crc = u32::from_le_bytes([rest[4], rest[5], rest[6], rest[7]]);
        if rest.len() - RECORD_HEADER < len {
            out.truncated_tail = true;
            break;
        }
        let payload = &rest[RECORD_HEADER..RECORD_HEADER + len];
        if crc32fast::hash(payload) != crc {
            out.corruption = Some(format!("checksum mismatch in record at byte {pos}"));
            break;
        }
        match serde_json::from_slice::<Record>(payload) {
            Ok(record) => out.records.push(record),
            Err(e) => {
                out.corruption = Some(format!("unreadable record at byte {pos}: {e}"));
                break;
            }
        }
        pos += RECORD_HEADER + len;
    }
    out
}

/// The disk fingerprint in a base record.
#[derive(Clone, Copy, Debug)]
enum BaseHash {
    Blake3(ContentHash),
    /// Format version 1.
    Fnv1a(u64),
}

impl BaseHash {
    fn parse(text: &str) -> Option<Self> {
        match text.strip_prefix("blake3:") {
            Some(hex) => ContentHash::from_hex(hex).map(Self::Blake3),
            None => u64::from_str_radix(text, 16).ok().map(Self::Fnv1a),
        }
    }

    fn matches(self, doc: &Document) -> bool {
        match self {
            Self::Blake3(hash) => doc.disk_state().hash == hash,
            Self::Fnv1a(hash) => stable_hash(&doc.encoded_bytes()) == hash,
        }
    }
}

/// Read, verify and reconstruct one journal. Never panics; never modifies anything.
pub fn inspect(journal_path: &Path, max_bytes: u64) -> JournalReport {
    let id = journal_path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let meta = match fs::metadata(journal_path) {
        Ok(meta) => meta,
        Err(e) => return JournalReport::unrecoverable(id, journal_path, e.to_string()),
    };
    if meta.len() > max_bytes {
        return JournalReport::unrecoverable(
            id,
            journal_path,
            format!(
                "journal is {} bytes, above the {max_bytes}-byte limit",
                meta.len()
            ),
        );
    }
    let bytes = match fs::read(journal_path) {
        Ok(bytes) => bytes,
        Err(e) => return JournalReport::unrecoverable(id, journal_path, e.to_string()),
    };
    if !bytes.starts_with(MAGIC) {
        return JournalReport::unrecoverable(id, journal_path, "bad journal header".to_string());
    }
    let parsed = parse_records(&bytes);
    let mut records = parsed.records.into_iter();
    let Some(Record::Base {
        format_version,
        path,
        disk_len,
        disk_hash,
        has_bom,
        base_revision,
        created_unix_ms,
        instance,
    }) = records.next()
    else {
        return JournalReport::unrecoverable(
            id,
            journal_path,
            "journal does not start with a base record".to_string(),
        );
    };
    let mut report = JournalReport::unrecoverable(id, journal_path, String::new());
    report.document_path = Some(PathBuf::from(&path));
    report.instance = Some(instance);
    report.created_unix_ms = Some(created_unix_ms);
    report.base_revision = Some(base_revision);
    report.truncated_tail = parsed.truncated_tail;
    report.corruption = parsed.corruption;
    report.has_bom = has_bom;
    report.valid_records = 1;
    if !(MIN_FORMAT_VERSION..=FORMAT_VERSION).contains(&format_version) {
        report.reason = Some(format!("unsupported journal format {format_version}"));
        return report;
    }
    let base_hash = BaseHash::parse(&disk_hash);

    // Current disk state, loaded once.
    let doc_path = PathBuf::from(&path);
    let limits = Limits {
        max_file_bytes: max_bytes,
    };
    let disk_doc = Document::open(&doc_path, &limits);
    let disk_text = disk_doc.as_ref().ok().map(|d| d.text().buffer().text());
    let disk_matches_base = disk_doc
        .as_ref()
        .ok()
        .is_some_and(|d| d.disk_state().len == disk_len && base_hash.is_some_and(|h| h.matches(d)));

    let mut buffer: Option<TextBuffer> = None;
    let mut revision = base_revision;
    let mut edit_records = 0;
    for record in records {
        match record {
            Record::Base { .. } => {
                report.corruption = Some("second base record".to_string());
                break;
            }
            Record::Checkpoint {
                revision: rev,
                text,
            } => {
                buffer = Some(TextBuffer::new(&text));
                revision = rev;
            }
            Record::Edits {
                from_revision,
                to_revision,
                edits,
                ..
            } => {
                if buffer.is_none() {
                    // No checkpoint: the edits apply to the base text, which must still be
                    // on disk unchanged.
                    match (&disk_text, disk_matches_base) {
                        (Some(text), true) => buffer = Some(TextBuffer::new(text)),
                        _ => {
                            report.reason = Some(
                                "the file changed on disk and the journal has no checkpoint \
                                 of the original text"
                                    .to_string(),
                            );
                            return report;
                        }
                    }
                }
                if from_revision != revision {
                    report.corruption = Some(format!(
                        "revision gap: expected a change from {revision}, found {from_revision}"
                    ));
                    break;
                }
                let edits: Vec<TextEdit> = edits
                    .into_iter()
                    .map(|e| TextEdit::replace(ByteOffset(e.start), ByteOffset(e.end), e.text))
                    .collect();
                let Some(buf) = buffer.as_mut() else { break };
                if let Err(e) = buf.apply(edits) {
                    report.corruption = Some(format!(
                        "change {from_revision}->{to_revision} does not apply: {e}"
                    ));
                    break;
                }
                revision = to_revision;
                edit_records += 1;
            }
        }
        report.valid_records += 1;
    }
    report.edit_records = edit_records;
    report.recovered_revision = Some(revision);

    let recovered = buffer.map(|b| b.text());
    report.disk = match (&disk_doc, &disk_text) {
        (Err(titan_doc::DocError::Io { source, .. }), _)
            if source.kind() == io::ErrorKind::NotFound =>
        {
            DiskComparison::Missing
        }
        (Err(_), _) => DiskComparison::Unreadable,
        (Ok(_), Some(text)) if recovered.as_deref() == Some(text.as_str()) => {
            DiskComparison::MatchesRecovered
        }
        (Ok(_), _) if disk_matches_base => DiskComparison::MatchesBase,
        _ => DiskComparison::ChangedSinceBase,
    };
    report.state = match (&recovered, report.disk) {
        (None, _) => RecoveryState::NoChanges,
        (Some(_), DiskComparison::MatchesRecovered) => RecoveryState::NoChanges,
        (Some(_), DiskComparison::MatchesBase) if edit_records == 0 => RecoveryState::NoChanges,
        (Some(_), DiskComparison::MatchesBase) => RecoveryState::Recoverable,
        (Some(_), _) => RecoveryState::Conflict,
    };
    report.reason = match report.state {
        RecoveryState::Conflict => Some(format!(
            "the file is {} since the unsaved edits were made",
            match report.disk {
                DiskComparison::Missing => "missing",
                DiskComparison::Unreadable => "unreadable",
                _ => "changed",
            }
        )),
        _ => None,
    };
    report.recovered_text = recovered;
    report
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CleanupReport {
    pub removed_expired: usize,
    pub removed_over_budget: usize,
    pub kept: usize,
    pub kept_bytes: u64,
}

/// The set of journals in a state directory.
#[derive(Clone, Debug)]
pub struct JournalStore {
    config: JournalConfig,
}

impl JournalStore {
    pub fn new(config: JournalConfig) -> Self {
        Self { config }
    }

    pub fn config(&self) -> &JournalConfig {
        &self.config
    }

    /// Map a journal id to its path. Ids are bare file names; anything containing a path
    /// separator, `..` or unexpected characters is rejected.
    pub fn path_for_id(&self, id: &str) -> Option<PathBuf> {
        let valid = id.ends_with(&format!(".{JOURNAL_EXTENSION}"))
            && id
                .chars()
                .all(|c| c.is_ascii_hexdigit() || c == '-' || c == '.' || c.is_ascii_lowercase())
            && !id.contains("..")
            && !id.starts_with('.');
        valid.then(|| self.config.dir.join(id))
    }

    fn journal_files(&self) -> io::Result<Vec<(PathBuf, fs::Metadata)>> {
        let entries = match fs::read_dir(&self.config.dir) {
            Ok(entries) => entries,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(e) => return Err(e),
        };
        let mut out = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.is_file() && path.extension().is_some_and(|ext| ext == JOURNAL_EXTENSION) {
                out.push((path, meta));
            }
        }
        out.sort_by(|a, b| a.0.cmp(&b.0));
        Ok(out)
    }

    /// Ids of journals (not in `active`) whose file name marks them as belonging to `path`.
    /// Cheap: reads the directory listing only. Callers must still verify the base record.
    pub fn ids_for_document(&self, path: &Path, active: &HashSet<PathBuf>) -> Vec<String> {
        let prefix = format!("{:016x}-", stable_hash(path.to_string_lossy().as_bytes()));
        self.journal_files()
            .unwrap_or_default()
            .into_iter()
            .filter(|(p, _)| !active.contains(p))
            .filter_map(|(p, _)| p.file_name().map(|n| n.to_string_lossy().into_owned()))
            .filter(|name| name.starts_with(&prefix))
            .collect()
    }

    /// Inspect every journal except the ones this process is writing.
    pub fn list(&self, active: &HashSet<PathBuf>) -> io::Result<Vec<JournalReport>> {
        Ok(self
            .journal_files()?
            .into_iter()
            .filter(|(path, _)| !active.contains(path))
            .map(|(path, _)| inspect(&path, self.config.max_journal_bytes))
            .collect())
    }

    /// Inspect one journal by id.
    pub fn inspect_id(&self, id: &str) -> Option<JournalReport> {
        let path = self.path_for_id(id)?;
        path.is_file()
            .then(|| inspect(&path, self.config.max_journal_bytes))
    }

    /// Delete one journal by id. Returns false if it did not exist.
    pub fn discard(&self, id: &str) -> io::Result<bool> {
        let Some(path) = self.path_for_id(id) else {
            return Ok(false);
        };
        match fs::remove_file(path) {
            Ok(()) => Ok(true),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(false),
            Err(e) => Err(e),
        }
    }

    /// Enforce retention: delete inactive journals older than the retention period, then the
    /// oldest inactive ones until the total fits the byte budget.
    pub fn cleanup(&self, active: &HashSet<PathBuf>, now: SystemTime) -> io::Result<CleanupReport> {
        let mut report = CleanupReport::default();
        let mut keep: Vec<(PathBuf, SystemTime, u64)> = Vec::new();
        for (path, meta) in self.journal_files()? {
            if active.contains(&path) {
                continue;
            }
            let modified = meta.modified().unwrap_or(now);
            let age = now.duration_since(modified).unwrap_or_default();
            if age > self.config.retention {
                if fs::remove_file(&path).is_ok() {
                    report.removed_expired += 1;
                }
                continue;
            }
            keep.push((path, modified, meta.len()));
        }
        keep.sort_by_key(|(_, modified, _)| *modified);
        let mut total: u64 = keep.iter().map(|(_, _, len)| len).sum();
        let mut remaining = Vec::new();
        for (path, modified, len) in keep {
            if total > self.config.max_total_bytes && fs::remove_file(&path).is_ok() {
                total -= len;
                report.removed_over_budget += 1;
            } else {
                remaining.push((path, modified, len));
            }
        }
        report.kept = remaining.len();
        report.kept_bytes = total;
        Ok(report)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use titan_text::TextDocument;

    struct Fixture {
        _dirs: (tempfile::TempDir, tempfile::TempDir),
        config: JournalConfig,
        file: PathBuf,
    }

    fn fixture(content: &str) -> Fixture {
        let state = tempfile::tempdir().unwrap();
        let work = tempfile::tempdir().unwrap();
        let file = work.path().join("main.rs");
        fs::write(&file, content).unwrap();
        let config = JournalConfig::new(state.path().join("recovery"));
        Fixture {
            _dirs: (state, work),
            config,
            file,
        }
    }

    fn open(fx: &Fixture) -> (Document, JournalWriter) {
        let doc = Document::open(&fx.file, &Limits::default()).unwrap();
        let writer = JournalWriter::create(
            &fx.config,
            BaseInfo::from_document(&doc),
            doc.text().buffer(),
        )
        .unwrap();
        (doc, writer)
    }

    fn edit(doc: &mut Document, writer: &mut JournalWriter, at: usize, text: &str) {
        let rev = doc.text().revision();
        let change = doc
            .text_mut()
            .apply(rev, vec![TextEdit::insert(ByteOffset(at), text)])
            .unwrap();
        writer.append(&change).unwrap();
    }

    #[test]
    fn unsaved_edits_survive_and_disk_is_untouched() {
        let fx = fixture("fn main() {}\n");
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 11, " println!(\"مرحبا\"); ");
        edit(&mut doc, &mut writer, 0, "// 👋\n");
        let rev = doc.text().revision();
        let undo = doc.text_mut().undo(rev).unwrap().unwrap();
        writer.append(&undo).unwrap();
        let expected = doc.text().buffer().text();
        let path = writer.path().to_path_buf();
        drop(writer); // no sync, no cleanup: like a crash

        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Recoverable, "{report:?}");
        assert_eq!(report.disk, DiskComparison::MatchesBase);
        assert_eq!(report.recovered_revision, Some(3));
        assert_eq!(report.edit_records, 3);
        assert_eq!(report.recovered_text.as_deref(), Some(expected.as_str()));
        assert!(!report.truncated_tail && report.corruption.is_none());
        assert_eq!(fs::read_to_string(&fx.file).unwrap(), "fn main() {}\n");
    }

    #[test]
    fn truncated_tail_keeps_the_valid_prefix() {
        let fx = fixture("abc");
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 3, "1");
        let after_first = doc.text().buffer().text();
        edit(&mut doc, &mut writer, 4, "2");
        let path = writer.path().to_path_buf();
        drop(writer);
        // Cut the last record in half, as an interrupted write would.
        let bytes = fs::read(&path).unwrap();
        let last_len = encode(&Record::Edits {
            from_revision: 1,
            to_revision: 2,
            kind: "edit".to_string(),
            edits: vec![JournalEdit {
                start: 4,
                end: 4,
                text: "2".to_string(),
            }],
        })
        .unwrap()
        .len();
        fs::write(&path, &bytes[..bytes.len() - last_len / 2]).unwrap();

        let report = inspect(&path, u64::MAX);
        assert!(report.truncated_tail);
        assert!(report.corruption.is_none());
        assert_eq!(report.recovered_revision, Some(1));
        assert_eq!(report.recovered_text.as_deref(), Some(after_first.as_str()));
        assert_eq!(report.state, RecoveryState::Recoverable);
    }

    #[test]
    fn corrupted_record_stops_reconstruction() {
        let fx = fixture("abc");
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 3, "1");
        let good = doc.text().buffer().text();
        edit(&mut doc, &mut writer, 4, "WXYZ");
        edit(&mut doc, &mut writer, 0, ">");
        let path = writer.path().to_path_buf();
        drop(writer);
        let mut bytes = fs::read(&path).unwrap();
        let at = bytes
            .windows(4)
            .position(|w| w == b"WXYZ")
            .expect("payload present");
        bytes[at] = b'Q';
        fs::write(&path, &bytes).unwrap();

        let report = inspect(&path, u64::MAX);
        assert!(
            report
                .corruption
                .as_deref()
                .is_some_and(|c| c.contains("checksum")),
            "{report:?}"
        );
        assert_eq!(report.recovered_revision, Some(1));
        assert_eq!(report.recovered_text.as_deref(), Some(good.as_str()));
    }

    #[test]
    fn external_change_is_a_conflict_but_text_is_kept() {
        let fx = fixture("original\n");
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 0, "mine: ");
        let path = writer.path().to_path_buf();
        drop(writer);
        fs::write(&fx.file, "someone else's newer content\n").unwrap();

        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Conflict);
        assert_eq!(report.disk, DiskComparison::ChangedSinceBase);
        assert_eq!(report.recovered_text.as_deref(), Some("mine: original\n"));
        assert_eq!(
            fs::read_to_string(&fx.file).unwrap(),
            "someone else's newer content\n"
        );

        fs::remove_file(&fx.file).unwrap();
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Conflict);
        assert_eq!(report.disk, DiskComparison::Missing);
    }

    #[test]
    fn without_checkpoint_an_external_change_is_unrecoverable() {
        let mut fx = fixture("original\n");
        fx.config.initial_checkpoint_max_bytes = 0;
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 0, "mine: ");
        let path = writer.path().to_path_buf();
        drop(writer);

        // Disk unchanged: edits replay onto the disk text.
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Recoverable);
        assert_eq!(report.recovered_text.as_deref(), Some("mine: original\n"));

        fs::write(&fx.file, "changed\n").unwrap();
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Unrecoverable);
        assert!(report.recovered_text.is_none());
    }

    #[test]
    fn compaction_bounds_size_and_preserves_text() {
        let mut fx = fixture("");
        fx.config.compact_after_bytes = 4_096;
        let (mut doc, mut writer) = open(&fx);
        let mut compactions = 0;
        for i in 0..2_000 {
            let at = doc.text().buffer().len_bytes();
            edit(
                &mut doc,
                &mut writer,
                at,
                if i % 50 == 49 { "\n" } else { "x" },
            );
            if writer.needs_compaction() {
                let rev = doc.text().revision();
                writer.compact(rev, doc.text().buffer()).unwrap();
                compactions += 1;
            }
            assert!(writer.len_bytes() < 4_096 + 8_192);
        }
        assert!(compactions > 0);
        writer.sync().unwrap();
        let path = writer.path().to_path_buf();
        drop(writer);
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.recovered_revision, Some(2_000));
        assert_eq!(
            report.recovered_text.as_deref(),
            Some(doc.text().buffer().text().as_str())
        );
    }

    #[test]
    fn revision_gaps_are_refused_by_the_writer() {
        let fx = fixture("abc");
        let (_doc, mut writer) = open(&fx);
        let mut other = TextDocument::new("abc");
        other
            .apply(0, vec![TextEdit::insert(ByteOffset(0), "x")])
            .unwrap();
        let skipped = other
            .apply(1, vec![TextEdit::insert(ByteOffset(0), "y")])
            .unwrap();
        assert!(writer.append(&skipped).is_err());
    }

    #[test]
    fn edits_undone_back_to_disk_mean_no_changes() {
        let fx = fixture("same");
        let (mut doc, mut writer) = open(&fx);
        edit(&mut doc, &mut writer, 4, "!");
        let undo = doc.text_mut().undo(1).unwrap().unwrap();
        writer.append(&undo).unwrap();
        let path = writer.path().to_path_buf();
        drop(writer);
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::NoChanges);
        assert_eq!(report.disk, DiskComparison::MatchesRecovered);
    }

    #[test]
    fn garbage_and_foreign_files_are_unrecoverable() {
        let fx = fixture("x");
        fs::create_dir_all(&fx.config.dir).unwrap();
        let bad = fx.config.dir.join("0000000000000000-1-1.tjournal");
        fs::write(&bad, b"not a journal").unwrap();
        assert_eq!(inspect(&bad, u64::MAX).state, RecoveryState::Unrecoverable);
        let mut header_only = MAGIC.to_vec();
        header_only.extend(
            encode(&Record::Checkpoint {
                revision: 0,
                text: String::new(),
            })
            .unwrap(),
        );
        fs::write(&bad, header_only).unwrap();
        assert_eq!(inspect(&bad, u64::MAX).state, RecoveryState::Unrecoverable);
    }

    #[test]
    fn store_ids_are_validated_and_cleanup_enforces_retention() {
        let fx = fixture("x");
        let store = JournalStore::new(fx.config.clone());
        assert!(store.path_for_id("../etc/passwd").is_none());
        assert!(store.path_for_id("a/b.tjournal").is_none());
        assert!(store.path_for_id(".hidden.tjournal").is_none());
        assert!(store.path_for_id("deadbeef-12-ab.txt").is_none());

        let (doc, writer) = open(&fx);
        let id = writer.id();
        assert!(store.path_for_id(&id).is_some());
        let active: HashSet<PathBuf> = [writer.path().to_path_buf()].into_iter().collect();
        assert!(store.list(&active).unwrap().is_empty());
        assert_eq!(store.list(&HashSet::new()).unwrap().len(), 1);

        // An old inactive journal is removed; the active one is kept.
        let (_doc2, old) = open(&fx);
        let old_path = old.path().to_path_buf();
        drop(old);
        let file = OpenOptions::new().write(true).open(&old_path).unwrap();
        file.set_modified(SystemTime::now() - Duration::from_secs(30 * 24 * 3600))
            .unwrap();
        let report = store.cleanup(&active, SystemTime::now()).unwrap();
        assert_eq!(report.removed_expired, 1);
        assert!(!old_path.exists());
        assert!(writer.path().exists());

        assert!(store.discard(&id).unwrap());
        assert!(!store.discard(&id).unwrap());
        drop((doc, writer));
    }
    /// A journal of several transactions plus the model text after each revision.
    fn sample_journal(fx: &Fixture) -> (PathBuf, Vec<String>) {
        let (mut doc, mut writer) = open(fx);
        let mut texts = vec![doc.text().buffer().text()];
        for (at, text) in [(0, "α"), (2, "β\n"), (0, "🚀 "), (5, "edit-four"), (1, "z")] {
            let at = at.min(doc.text().buffer().len_bytes());
            let rev = doc.text().revision();
            let change = doc
                .text_mut()
                .apply(rev, vec![TextEdit::insert(ByteOffset(at), text)]);
            let change = match change {
                Ok(c) => c,
                // Not on a char boundary: insert at the end instead.
                Err(_) => {
                    let end = doc.text().buffer().len_bytes();
                    doc.text_mut()
                        .apply(rev, vec![TextEdit::insert(ByteOffset(end), text)])
                        .unwrap()
                }
            };
            writer.append(&change).unwrap();
            texts.push(doc.text().buffer().text());
        }
        let path = writer.path().to_path_buf();
        drop(writer);
        (path, texts)
    }

    /// Interrupted writes at every possible byte: reading never panics and never yields a
    /// text other than the exact text of some earlier revision.
    #[test]
    fn truncation_at_every_byte_recovers_an_exact_prefix() {
        let fx = fixture("base\n");
        let (path, texts) = sample_journal(&fx);
        let full = fs::read(&path).unwrap();
        // Record boundaries: a cut exactly there is a clean (shorter) journal.
        let mut boundaries = vec![MAGIC.len()];
        let mut pos = MAGIC.len();
        while pos < full.len() {
            let len = u32::from_le_bytes([full[pos], full[pos + 1], full[pos + 2], full[pos + 3]]);
            pos += RECORD_HEADER + len as usize;
            boundaries.push(pos);
        }
        let mut last_revision = 0;
        for cut in MAGIC.len()..=full.len() {
            fs::write(&path, &full[..cut]).unwrap();
            let report = inspect(&path, u64::MAX);
            assert!(report.corruption.is_none(), "cut {cut}: {report:?}");
            if let (Some(rev), Some(text)) = (report.recovered_revision, &report.recovered_text) {
                assert_eq!(text, &texts[rev as usize], "cut {cut}");
                assert!(rev >= last_revision, "recovered revision went backwards");
                last_revision = rev;
                assert_eq!(
                    report.truncated_tail,
                    !boundaries.contains(&cut),
                    "cut {cut}"
                );
            }
        }
        assert_eq!(last_revision, 5);
    }

    /// Single-byte corruption anywhere: never a wrong text, never a panic.
    #[test]
    fn corruption_at_every_byte_never_yields_wrong_text() {
        let fx = fixture("base\n");
        let (path, texts) = sample_journal(&fx);
        let full = fs::read(&path).unwrap();
        let mut detected = 0;
        for at in 0..full.len() {
            let mut bytes = full.clone();
            bytes[at] ^= 0x41;
            fs::write(&path, &bytes).unwrap();
            let report = inspect(&path, u64::MAX);
            if let (Some(rev), Some(text)) = (report.recovered_revision, &report.recovered_text) {
                assert_eq!(text, &texts[rev as usize], "flip at {at}: {report:?}");
            }
            if report.corruption.is_some()
                || report.truncated_tail
                || report.state == RecoveryState::Unrecoverable
            {
                detected += 1;
            }
        }
        // Every flip is either detected or harmless (none is silently accepted as different
        // content, checked above); in practice CRC-32 flags every single flip.
        assert_eq!(detected, full.len());
    }

    #[test]
    fn version_1_journals_are_still_read() {
        let fx = fixture("old format\n");
        fs::create_dir_all(&fx.config.dir).unwrap();
        let path = fx.config.dir.join("00000000000000aa-1-1.tjournal");
        let mut bytes = MAGIC.to_vec();
        bytes.extend(
            encode(&Record::Base {
                format_version: 1,
                path: fx.file.to_string_lossy().into_owned(),
                disk_len: 11,
                disk_hash: format!("{:016x}", stable_hash(b"old format\n")),
                has_bom: false,
                base_revision: 0,
                created_unix_ms: 0,
                instance: "1-1".to_string(),
            })
            .unwrap(),
        );
        bytes.extend(
            encode(&Record::Edits {
                from_revision: 0,
                to_revision: 1,
                kind: "edit".to_string(),
                edits: vec![JournalEdit {
                    start: 0,
                    end: 0,
                    text: "v1: ".to_string(),
                }],
            })
            .unwrap(),
        );
        fs::write(&path, bytes).unwrap();
        let report = inspect(&path, u64::MAX);
        assert_eq!(report.state, RecoveryState::Recoverable, "{report:?}");
        assert_eq!(report.recovered_text.as_deref(), Some("v1: old format\n"));
        fs::write(&fx.file, "changed\n").unwrap();
        assert_eq!(inspect(&path, u64::MAX).state, RecoveryState::Unrecoverable);
    }

    #[test]
    fn repeated_crashes_accumulate_no_lost_edits() {
        // Crash, recover into a fresh document, edit, crash again — three times.
        let fx = fixture("v0\n");
        let mut text = fs::read_to_string(&fx.file).unwrap();
        for round in 0..3 {
            let mut doc = Document::open(&fx.file, &Limits::default()).unwrap();
            // Restore the previous round's text as one transaction, as titand does.
            if doc.text().buffer().text() != text {
                let end = doc.text().buffer().len_bytes();
                doc.text_mut()
                    .apply(
                        0,
                        vec![TextEdit::replace(
                            ByteOffset(0),
                            ByteOffset(end),
                            text.clone(),
                        )],
                    )
                    .unwrap();
            }
            let base = BaseInfo::from_document(&doc);
            let mut writer = JournalWriter::create(&fx.config, base, doc.text().buffer()).unwrap();
            edit(&mut doc, &mut writer, 0, &format!("round {round}\n"));
            let expected = doc.text().buffer().text();
            let path = writer.path().to_path_buf();
            drop(writer); // crash
            let store = JournalStore::new(fx.config.clone());
            let reports = store.list(&HashSet::new()).unwrap();
            assert_eq!(reports.len(), 1, "previous journals were discarded");
            let report = &reports[0];
            assert_eq!(report.state, RecoveryState::Recoverable, "{report:?}");
            assert_eq!(report.recovered_text.as_deref(), Some(expected.as_str()));
            text = report.recovered_text.clone().unwrap();
            fs::remove_file(path).unwrap(); // restored: titand discards the journal
        }
        assert_eq!(text, "round 2\nround 1\nround 0\nv0\n");
        assert_eq!(fs::read_to_string(&fx.file).unwrap(), "v0\n");
    }

    #[test]
    fn cleanup_enforces_the_byte_budget_oldest_first() {
        let mut fx = fixture("x");
        fx.config.max_total_bytes = 1;
        let mut paths = Vec::new();
        for age in [3u64, 2, 1] {
            let (_doc, writer) = open(&fx);
            let path = writer.path().to_path_buf();
            drop(writer);
            let file = OpenOptions::new().write(true).open(&path).unwrap();
            file.set_modified(SystemTime::now() - Duration::from_secs(age * 3600))
                .unwrap();
            paths.push(path);
        }
        let store = JournalStore::new(fx.config.clone());
        let report = store.cleanup(&HashSet::new(), SystemTime::now()).unwrap();
        assert_eq!(report.removed_over_budget, 3);
        assert!(paths.iter().all(|p| !p.exists()));
        // An active journal is never removed, whatever the budget.
        let (_doc, writer) = open(&fx);
        let active: HashSet<PathBuf> = [writer.path().to_path_buf()].into_iter().collect();
        store.cleanup(&active, SystemTime::now()).unwrap();
        assert!(writer.path().exists());
    }

    /// Creating a journal on a full file system fails cleanly and leaves no file behind.
    #[test]
    #[ignore = "needs TITAN_TINY_FS pointing at a small (e.g. 256 KiB) file system; CI provides it"]
    fn creation_on_a_full_disk_leaves_no_file() {
        use std::io::Write as _;
        let tiny = std::env::var_os("TITAN_TINY_FS")
            .expect("set TITAN_TINY_FS to a directory on a small file system");
        let state = tempfile::tempdir_in(&tiny).unwrap();
        let config = JournalConfig::new(state.path().join("recovery"));
        fs::create_dir_all(&config.dir).unwrap();
        let filler = state.path().join("filler");
        let mut f = File::create(&filler).unwrap();
        while f.write_all(&[b'f'; 4096]).is_ok() {}
        drop(f);
        let work = tempfile::tempdir().unwrap();
        let file = work.path().join("a.rs");
        fs::write(&file, "x".repeat(64 * 1024)).unwrap();
        let doc = Document::open(&file, &Limits::default()).unwrap();
        let result =
            JournalWriter::create(&config, BaseInfo::from_document(&doc), doc.text().buffer());
        assert!(result.is_err(), "the disk is full");
        let left: Vec<_> = fs::read_dir(&config.dir).unwrap().collect();
        assert!(left.is_empty(), "left behind: {left:?}");
        fs::remove_file(filler).unwrap();
    }

    /// Real storage exhaustion on a tiny tmpfs (CI mounts one and sets `TITAN_TINY_FS`).
    #[test]
    #[ignore = "needs TITAN_TINY_FS pointing at a small (e.g. 256 KiB) file system; CI provides it"]
    fn storage_exhaustion_keeps_the_journal_valid() {
        let tiny = std::env::var_os("TITAN_TINY_FS")
            .expect("set TITAN_TINY_FS to a directory on a small file system");
        let state = tempfile::tempdir_in(&tiny).unwrap();
        let work = tempfile::tempdir().unwrap();
        let file = work.path().join("big.rs");
        fs::write(&file, "fn main() {}\n").unwrap();
        let mut config = JournalConfig::new(state.path().join("recovery"));
        config.compact_after_bytes = u64::MAX;
        let mut doc = Document::open(&file, &Limits::default()).unwrap();
        let mut writer =
            JournalWriter::create(&config, BaseInfo::from_document(&doc), doc.text().buffer())
                .unwrap();
        let chunk = "x".repeat(16 * 1024);
        let mut failed_at = None;
        let mut journaled_text = doc.text().buffer().text();
        for i in 0..1_000 {
            let rev = doc.text().revision();
            let end = doc.text().buffer().len_bytes();
            let change = doc
                .text_mut()
                .apply(rev, vec![TextEdit::insert(ByteOffset(end), chunk.as_str())])
                .unwrap();
            match writer.append(&change) {
                Ok(()) => journaled_text = doc.text().buffer().text(),
                Err(e) => {
                    failed_at = Some((i, e.to_string()));
                    break;
                }
            }
        }
        let (i, error) = failed_at.expect("a 256 KiB file system must fill up");
        eprintln!("journal write failed after {i} records: {error}");
        let path = writer.path().to_path_buf();
        drop(writer);
        let report = inspect(&path, u64::MAX);
        assert!(
            !report.truncated_tail,
            "the partial record was removed: {report:?}"
        );
        assert!(report.corruption.is_none());
        assert_eq!(
            report.recovered_text.as_deref(),
            Some(journaled_text.as_str())
        );
        assert_eq!(report.recovered_revision, Some(i as u64));
    }
}
