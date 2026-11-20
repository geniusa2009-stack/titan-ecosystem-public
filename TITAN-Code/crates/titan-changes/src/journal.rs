//! Transaction journal: one file per transaction, CRC-framed JSON lines.
//!
//! ```text
//! TITANCHG1\n                       magic line
//! <crc32 as 8 hex digits> <json>\n  one record per line, fsynced before the next step
//! ```
//! A final line without `\n` is a torn write and is ignored. A line whose CRC or JSON is
//! wrong stops reading; the journal is then reported as damaged and recovery leaves the
//! transaction alone (it reports it; the staged copies stay in the transaction directory).

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

pub(crate) const MAGIC: &str = "TITANCHG1\n";
pub(crate) const FORMAT_VERSION: u32 = 1;

/// Content identity of one side of a planned operation.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct Version {
    pub hash: String,
    pub len: u64,
}

/// What a planned operation does to one path.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum OpKind {
    /// Replace the content of an existing file.
    Write,
    /// Create a file that does not exist.
    Create,
    /// Remove a file.
    Delete,
}

/// One planned file operation. `before`/`after` = `None` means "no file".
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PlanOp {
    pub index: usize,
    pub change_index: usize,
    pub kind: OpKind,
    pub rel: String,
    pub path: PathBuf,
    pub before: Option<Version>,
    pub after: Option<Version>,
    /// Permission bits for a file this operation (or its undo) creates.
    pub mode: Option<u32>,
    /// Directories this operation creates (outermost first); removed again on undo if
    /// empty.
    #[serde(default)]
    pub created_dirs: Vec<PathBuf>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
pub(crate) enum Record {
    #[serde(rename_all = "camelCase")]
    Prepared {
        format_version: u32,
        txn_id: String,
        proposal_id: String,
        patch_id: String,
        workspace_root: PathBuf,
        created_unix_ms: u64,
        ops: Vec<PlanOp>,
    },
    #[serde(rename_all = "camelCase")]
    Begin {
        index: usize,
    },
    #[serde(rename_all = "camelCase")]
    Applied {
        index: usize,
    },
    #[serde(rename_all = "camelCase")]
    NotApplied {
        index: usize,
        reason: String,
    },
    Committed,
    #[serde(rename_all = "camelCase")]
    RollbackStarted {
        reason: String,
    },
    #[serde(rename_all = "camelCase")]
    Undone {
        index: usize,
        result: String,
    },
    RolledBack,
    #[serde(rename_all = "camelCase")]
    RollbackIncomplete {
        conflicts: Vec<String>,
    },
    RestoreStarted,
    Restored,
    #[serde(rename_all = "camelCase")]
    RestoreIncomplete {
        conflicts: Vec<String>,
    },
}

impl Record {
    /// A record after which nothing more happens to the transaction.
    pub fn is_terminal(&self) -> bool {
        matches!(
            self,
            Self::Committed
                | Self::RolledBack
                | Self::RollbackIncomplete { .. }
                | Self::Restored
                | Self::RestoreIncomplete { .. }
        )
    }
}

fn encode(record: &Record) -> io::Result<Vec<u8>> {
    let json = serde_json::to_string(record).map_err(io::Error::other)?;
    Ok(format!("{:08x} {json}\n", crc32fast::hash(json.as_bytes())).into_bytes())
}

/// Append-only writer; every record is written with one call and `fsync`ed.
pub(crate) struct JournalWriter {
    file: File,
}

impl JournalWriter {
    /// Create the journal (it must not exist) with its first record, `fsync` it and its
    /// directory.
    pub fn create(path: &Path, first: &Record) -> io::Result<Self> {
        let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
        let mut bytes = MAGIC.as_bytes().to_vec();
        bytes.extend(encode(first)?);
        file.write_all(&bytes)?;
        file.sync_all()?;
        if let Some(dir) = path.parent() {
            crate::util::sync_dir(dir)?;
        }
        Ok(Self { file })
    }

    /// Open an existing journal for appending.
    pub fn open(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new().append(true).open(path)?;
        Ok(Self { file })
    }

    pub fn append(&mut self, record: &Record) -> io::Result<()> {
        self.file.write_all(&encode(record)?)?;
        self.file.sync_data()
    }
}

/// What could be read from a journal.
#[derive(Debug, Default)]
pub(crate) struct ParsedJournal {
    pub records: Vec<Record>,
    /// Why reading stopped early (bad magic, CRC or JSON error).
    pub damaged: Option<String>,
    /// A final record without its newline was ignored.
    pub torn_tail: bool,
}

impl ParsedJournal {
    pub fn prepared(&self) -> Option<(&str, &str, &[PlanOp])> {
        match self.records.first() {
            Some(Record::Prepared {
                txn_id,
                proposal_id,
                ops,
                ..
            }) => Some((txn_id, proposal_id, ops)),
            _ => None,
        }
    }

    pub fn last(&self) -> Option<&Record> {
        self.records.last()
    }

    pub fn is_finished(&self) -> bool {
        self.damaged.is_none() && self.last().is_some_and(Record::is_terminal)
    }
}

/// Read a journal without trusting it.
pub(crate) fn read(path: &Path) -> io::Result<ParsedJournal> {
    let bytes = fs::read(path)?;
    let mut parsed = ParsedJournal::default();
    let Some(body) = bytes.strip_prefix(MAGIC.as_bytes()) else {
        parsed.damaged = Some("missing or wrong magic line".to_string());
        return Ok(parsed);
    };
    let mut rest = body;
    let mut line_no = 1usize;
    while !rest.is_empty() {
        line_no += 1;
        let Some(end) = rest.iter().position(|&b| b == b'\n') else {
            parsed.torn_tail = true;
            break;
        };
        let line = &rest[..end];
        rest = &rest[end + 1..];
        match decode_line(line) {
            Ok(record) => parsed.records.push(record),
            Err(reason) => {
                parsed.damaged = Some(format!("line {line_no}: {reason}"));
                break;
            }
        }
    }
    Ok(parsed)
}

fn decode_line(line: &[u8]) -> Result<Record, String> {
    let text = std::str::from_utf8(line).map_err(|_| "not UTF-8".to_string())?;
    let (crc_hex, json) = text
        .split_once(' ')
        .ok_or_else(|| "no checksum".to_string())?;
    let crc = u32::from_str_radix(crc_hex, 16).map_err(|_| "bad checksum field".to_string())?;
    if crc_hex.len() != 8 || crc32fast::hash(json.as_bytes()) != crc {
        return Err("checksum mismatch".to_string());
    }
    serde_json::from_str(json).map_err(|e| format!("bad record: {e}"))
}
