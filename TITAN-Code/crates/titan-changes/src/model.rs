//! The change model: proposals, file changes, expected bases, statuses.
//!
//! Everything here is plain data, serialisable as camelCase JSON, so the same model can be
//! produced by a developer command, an LSP refactoring (for example a rust-analyzer rename
//! `WorkspaceEdit`) or an agent, and can be stored and inspected later.

use std::fmt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use serde::{Deserialize, Serialize};
use titan_doc::ContentHash;

use crate::util::unix_ms;
use crate::validate::Problem;

/// Maximum length of an identifier ([`ProposalId`], transaction ids).
pub const MAX_ID_LEN: usize = 64;

static ID_COUNTER: AtomicU64 = AtomicU64::new(0);

/// A fresh opaque identifier: the first 32 hex digits of BLAKE3 over a domain tag, the
/// current time in nanoseconds, the process id and a process-wide counter.
pub(crate) fn fresh_id(domain: &str) -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let counter = ID_COUNTER.fetch_add(1, Ordering::Relaxed);
    let input = format!(
        "titan-changes:{domain}:{nanos}:{}:{counter}",
        std::process::id()
    );
    let mut hex = ContentHash::of(input.as_bytes()).to_hex();
    hex.truncate(32);
    hex
}

/// True if `id` is usable as an identifier and as a file-name component: 1 to
/// [`MAX_ID_LEN`] ASCII letters, digits, `-` or `_`.
pub fn is_valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= MAX_ID_LEN
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

/// Unique, opaque identifier of a proposal. Validated on construction and on
/// deserialisation (it is used in file names, so it can never contain a path separator).
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(try_from = "String", into = "String")]
pub struct ProposalId(String);

impl ProposalId {
    /// A new identifier (32 lowercase hex digits).
    pub fn generate() -> Self {
        Self(fresh_id("proposal"))
    }

    /// Parse an identifier; `None` unless [`is_valid_id`] holds.
    pub fn parse(text: &str) -> Option<Self> {
        is_valid_id(text).then(|| Self(text.to_string()))
    }

    /// The identifier text.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl TryFrom<String> for ProposalId {
    type Error = String;

    fn try_from(value: String) -> Result<Self, Self::Error> {
        if is_valid_id(&value) {
            Ok(Self(value))
        } else {
            Err(format!("invalid proposal id {value:?}"))
        }
    }
}

impl From<ProposalId> for String {
    fn from(id: ProposalId) -> Self {
        id.0
    }
}

impl fmt::Display for ProposalId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// Who or what produced a proposal.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProposalSource {
    /// `"developer"`, `"lsp.rename"`, `"agent"`, ... (free-form, recorded as given).
    pub kind: String,
    /// Source-specific detail (for example the LSP request), recorded as given.
    #[serde(default)]
    pub detail: serde_json::Value,
}

/// The workspace state a proposal was computed against.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BaseSnapshot {
    /// Workspace root the relative paths refer to.
    pub workspace_root: PathBuf,
    /// `git rev-parse HEAD` at proposal time, when known. Recorded, not checked.
    #[serde(default)]
    pub git_head: Option<String>,
}

/// What a change expects to find at its path before it is applied.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum ExpectedBase {
    /// An open editor buffer at this revision whose text has this BLAKE3 hash (hex of the
    /// buffer text's UTF-8 bytes). Such changes are returned to the caller as
    /// [`crate::BufferEdits`]; this crate never writes them to disk.
    #[serde(rename_all = "camelCase")]
    Buffer { revision: u64, hash: String },
    /// A file on disk with this BLAKE3 hash (hex) and length in bytes.
    #[serde(rename_all = "camelCase")]
    Disk { hash: String, len: u64 },
    /// No file.
    Absent,
}

impl ExpectedBase {
    /// The expectation for exactly these disk bytes.
    pub fn disk(bytes: &[u8]) -> Self {
        Self::Disk {
            hash: ContentHash::of(bytes).to_hex(),
            len: bytes.len() as u64,
        }
    }

    /// The expectation for an open buffer with this text at this revision.
    pub fn buffer(revision: u64, text: &str) -> Self {
        Self::Buffer {
            revision,
            hash: ContentHash::of(text.as_bytes()).to_hex(),
        }
    }
}

/// One replacement of the byte range `start..end` of the expected base content by `text`.
///
/// Offsets are UTF-8 byte offsets: into the file's bytes for a disk base (a UTF-8 byte-order
/// mark, if present, is part of the bytes), into the buffer text for a buffer base. Edits
/// of one change must be sorted by `start`, must not overlap, and must fall on character
/// boundaries. Several insertions (`start == end`) at the same offset are allowed and are
/// applied in the order given.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ByteEdit {
    pub start: usize,
    pub end: usize,
    pub text: String,
    /// Id of a [`ChangeAnnotation`] of the proposal.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub annotation: Option<String>,
}

impl ByteEdit {
    /// Replace `start..end` by `text`, without annotation.
    pub fn new(start: usize, end: usize, text: impl Into<String>) -> Self {
        Self {
            start,
            end,
            text: text.into(),
            annotation: None,
        }
    }
}

/// One file-level change. Paths are relative to the workspace root, `/`-separated.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum FileChange {
    /// Edit an existing file or open buffer.
    #[serde(rename_all = "camelCase")]
    Edit {
        path: String,
        expected: ExpectedBase,
        edits: Vec<ByteEdit>,
    },
    /// Create a new file. Never overwrites an existing file.
    #[serde(rename_all = "camelCase")]
    Create {
        path: String,
        content: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        annotation: Option<String>,
    },
    /// Delete a file whose content is exactly `expected`.
    #[serde(rename_all = "camelCase")]
    Delete {
        path: String,
        expected: ExpectedBase,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        annotation: Option<String>,
    },
    /// Move a file whose content is exactly `expected` to `to`, which must not exist.
    #[serde(rename_all = "camelCase")]
    Rename {
        from: String,
        to: String,
        expected: ExpectedBase,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        annotation: Option<String>,
    },
}

impl FileChange {
    /// Short name of the change kind (`edit`, `create`, `delete`, `rename`).
    pub fn kind_name(&self) -> &'static str {
        match self {
            Self::Edit { .. } => "edit",
            Self::Create { .. } => "create",
            Self::Delete { .. } => "delete",
            Self::Rename { .. } => "rename",
        }
    }

    /// The primary path (the source for a rename).
    pub fn path(&self) -> &str {
        match self {
            Self::Edit { path, .. } | Self::Create { path, .. } | Self::Delete { path, .. } => path,
            Self::Rename { from, .. } => from,
        }
    }

    /// Annotation ids this change refers to, in order of appearance.
    pub fn annotation_ids(&self) -> Vec<&str> {
        match self {
            Self::Edit { edits, .. } => edits
                .iter()
                .filter_map(|e| e.annotation.as_deref())
                .collect(),
            Self::Create { annotation, .. }
            | Self::Delete { annotation, .. }
            | Self::Rename { annotation, .. } => annotation.as_deref().into_iter().collect(),
        }
    }
}

/// A label attached to edits, as in LSP `ChangeAnnotation`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChangeAnnotation {
    pub id: String,
    pub label: String,
    /// The change must be explicitly approved before it is applied.
    #[serde(default)]
    pub needs_confirmation: bool,
}

/// An approval: who approved and when.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Approval {
    pub by: String,
    pub at_unix_ms: u64,
}

/// Lifecycle of a proposal. The caller records transitions; [`crate::apply`] reads it only
/// to require [`ProposalStatus::Approved`] when annotations need confirmation.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum ProposalStatus {
    Proposed,
    Validated,
    #[serde(rename_all = "camelCase")]
    Rejected {
        problems: Vec<Problem>,
    },
    #[serde(rename_all = "camelCase")]
    Approved {
        by: String,
        at_unix_ms: u64,
    },
    Applied,
    PartiallyApplied,
    RolledBack,
    #[serde(rename_all = "camelCase")]
    Failed {
        reason: String,
    },
    Superseded,
}

/// A proposed multi-file change.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Proposal {
    pub id: ProposalId,
    pub source: ProposalSource,
    pub created_unix_ms: u64,
    pub base: BaseSnapshot,
    pub changes: Vec<FileChange>,
    /// Human-readable preconditions (recorded, not machine-checked).
    #[serde(default)]
    pub preconditions: Vec<String>,
    /// Human-readable description of the expected result (recorded, not checked).
    #[serde(default)]
    pub expected_result: Option<String>,
    #[serde(default)]
    pub annotations: Vec<ChangeAnnotation>,
    pub status: ProposalStatus,
}

impl Proposal {
    /// A new proposal with a fresh id, the current time and status `Proposed`.
    pub fn new(source: ProposalSource, base: BaseSnapshot, changes: Vec<FileChange>) -> Self {
        Self {
            id: ProposalId::generate(),
            source,
            created_unix_ms: unix_ms(),
            base,
            changes,
            preconditions: Vec::new(),
            expected_result: None,
            annotations: Vec::new(),
            status: ProposalStatus::Proposed,
        }
    }

    /// [`patch_id`] of this proposal's changes.
    pub fn patch_id(&self) -> String {
        patch_id(&self.changes)
    }

    /// The approval recorded in the status, if any.
    pub fn approval(&self) -> Option<Approval> {
        match &self.status {
            ProposalStatus::Approved { by, at_unix_ms } => Some(Approval {
                by: by.clone(),
                at_unix_ms: *at_unix_ms,
            }),
            _ => None,
        }
    }
}

/// Normalise a workspace-relative path: drop `.` and empty components, join with `/`.
/// Refuses empty paths, absolute paths, `..` components and NUL bytes.
pub fn normalize_rel(path: &str) -> Result<String, String> {
    if path.contains('\0') {
        return Err("the path contains a NUL byte".to_string());
    }
    if path.starts_with('/') || path.starts_with('\\') {
        return Err("absolute paths are not allowed; use a workspace-relative path".to_string());
    }
    let mut parts = Vec::new();
    for component in std::path::Path::new(path).components() {
        match component {
            std::path::Component::Normal(part) => match part.to_str() {
                Some(part) => parts.push(part),
                None => return Err("the path is not valid UTF-8".to_string()),
            },
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                return Err("`..` components are not allowed".to_string());
            }
            std::path::Component::RootDir | std::path::Component::Prefix(_) => {
                return Err(
                    "absolute paths are not allowed; use a workspace-relative path".to_string(),
                );
            }
        }
    }
    if parts.is_empty() {
        return Err("the path is empty".to_string());
    }
    Ok(parts.join("/"))
}

/// Version tag of the canonical form hashed by [`patch_id`].
const PATCH_ID_DOMAIN: &str = "titan-changes/patch-id/v1\n";

/// Stable identifier of a set of changes: BLAKE3 (hex, 64 digits) of a canonical
/// serialisation of the changes with normalised paths. Independent of the proposal id,
/// timestamps, source, status and annotations' labels, so the same edits proposed twice
/// (or by two different producers) get the same id.
pub fn patch_id(changes: &[FileChange]) -> String {
    let canonical: Vec<FileChange> = changes
        .iter()
        .map(|change| {
            let norm = |p: &str| normalize_rel(p).unwrap_or_else(|_| p.to_string());
            match change {
                FileChange::Edit {
                    path,
                    expected,
                    edits,
                } => FileChange::Edit {
                    path: norm(path),
                    expected: canonical_expected(expected),
                    edits: edits.clone(),
                },
                FileChange::Create {
                    path,
                    content,
                    annotation,
                } => FileChange::Create {
                    path: norm(path),
                    content: content.clone(),
                    annotation: annotation.clone(),
                },
                FileChange::Delete {
                    path,
                    expected,
                    annotation,
                } => FileChange::Delete {
                    path: norm(path),
                    expected: canonical_expected(expected),
                    annotation: annotation.clone(),
                },
                FileChange::Rename {
                    from,
                    to,
                    expected,
                    annotation,
                } => FileChange::Rename {
                    from: norm(from),
                    to: norm(to),
                    expected: canonical_expected(expected),
                    annotation: annotation.clone(),
                },
            }
        })
        .collect();
    let mut bytes = PATCH_ID_DOMAIN.as_bytes().to_vec();
    // Serialising plain derived structs cannot fail; an empty body would still be stable.
    bytes.extend(serde_json::to_vec(&canonical).unwrap_or_default());
    ContentHash::of(&bytes).to_hex()
}

/// Hex hashes compared case-insensitively elsewhere; canonicalise them to lowercase here.
fn canonical_expected(expected: &ExpectedBase) -> ExpectedBase {
    match expected {
        ExpectedBase::Buffer { revision, hash } => ExpectedBase::Buffer {
            revision: *revision,
            hash: hash.to_ascii_lowercase(),
        },
        ExpectedBase::Disk { hash, len } => ExpectedBase::Disk {
            hash: hash.to_ascii_lowercase(),
            len: *len,
        },
        ExpectedBase::Absent => ExpectedBase::Absent,
    }
}
