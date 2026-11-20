//! Typed params and results. Field names are camelCase on the wire.
//!
//! Positions are zero-based `{ line, character }`. The unit of `character` is negotiated in
//! `session.hello` (`"utf-8"` bytes or `"utf-16"` code units, as in LSP 3.17).

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub mod methods {
    pub const SESSION_HELLO: &str = "session.hello";
    pub const SESSION_SHUTDOWN: &str = "session.shutdown";
    pub const SYSTEM_HEALTH: &str = "system.health";
    pub const WORKSPACE_OPEN: &str = "workspace.open";
    pub const WORKSPACE_CLOSE: &str = "workspace.close";
    pub const WORKSPACE_LIST_FILES: &str = "workspace.listFiles";
    pub const WORKSPACE_SEARCH: &str = "workspace.search";
    pub const DOCUMENT_OPEN: &str = "document.open";
    pub const DOCUMENT_READ: &str = "document.read";
    pub const DOCUMENT_APPLY_EDITS: &str = "document.applyEdits";
    pub const DOCUMENT_UNDO: &str = "document.undo";
    pub const DOCUMENT_REDO: &str = "document.redo";
    pub const DOCUMENT_SAVE: &str = "document.save";
    pub const DOCUMENT_CHECK_DISK: &str = "document.checkDisk";
    pub const DOCUMENT_CLOSE: &str = "document.close";
    /// Resolve a disk conflict reported by `document/diskChanged` (keep or reload).
    pub const DOCUMENT_RESOLVE_CONFLICT: &str = "document.resolveConflict";
    pub const INDEX_STATUS: &str = "index.status";
    pub const INDEX_SYMBOLS: &str = "index.symbols";
    pub const INDEX_RESCAN: &str = "index.rescan";
    pub const RECOVERY_LIST: &str = "recovery.list";
    pub const RECOVERY_RESTORE: &str = "recovery.restore";
    pub const RECOVERY_DISCARD: &str = "recovery.discard";
    pub const SYSTEM_METRICS: &str = "system.metrics";
    /// Process memory, live heap (profiling builds) and per-subsystem estimates.
    pub const SYSTEM_MEMORY: &str = "system.memory";
    /// Semantic intelligence (syntax index + language server), see `docs/protocol.md`.
    pub const INTEL_STATUS: &str = "intel.status";
    pub const INTEL_DOCUMENT_SYMBOLS: &str = "intel.documentSymbols";
    pub const INTEL_WORKSPACE_SYMBOLS: &str = "intel.workspaceSymbols";
    pub const INTEL_DEFINITION: &str = "intel.definition";
    pub const INTEL_REFERENCES: &str = "intel.references";
    pub const INTEL_HOVER: &str = "intel.hover";
    pub const INTEL_COMPLETION: &str = "intel.completion";
    pub const INTEL_DIAGNOSTICS: &str = "intel.diagnostics";
    /// Explicit language-server lifecycle: `{action: "stop" | "start" | "restart"}`.
    pub const INTEL_SERVER: &str = "intel.server";
    /// Project graph (packages, modules, files, symbols, tests).
    pub const GRAPH_SUMMARY: &str = "graph.summary";
    pub const GRAPH_NODE: &str = "graph.node";
    pub const GRAPH_QUERY: &str = "graph.query";
    /// Change impact analysis for a position (known / structural / heuristic / unknown).
    pub const TWIN_IMPACT: &str = "twin.impact";
    /// Verified Changes: propose, preview, approve, apply, verify, restore, inspect.
    pub const CHANGES_PROPOSE: &str = "changes.propose";
    pub const CHANGES_PREVIEW: &str = "changes.preview";
    pub const CHANGES_APPROVE: &str = "changes.approve";
    pub const CHANGES_APPLY: &str = "changes.apply";
    pub const CHANGES_VERIFY: &str = "changes.verify";
    pub const CHANGES_RESTORE: &str = "changes.restore";
    pub const CHANGES_GET: &str = "changes.get";
    pub const CHANGES_LIST: &str = "changes.list";
    /// Rename through the language server, producing a Verified Changes proposal.
    pub const REFACTOR_RENAME: &str = "refactor.rename";
    /// Notification from the client: cancel the request with `params.id`.
    pub const CANCEL_REQUEST: &str = "$/cancelRequest";

    /// Every method the server implements, in a stable order.
    pub const ALL: &[&str] = &[
        SESSION_HELLO,
        SESSION_SHUTDOWN,
        SYSTEM_HEALTH,
        WORKSPACE_OPEN,
        WORKSPACE_CLOSE,
        WORKSPACE_LIST_FILES,
        WORKSPACE_SEARCH,
        DOCUMENT_OPEN,
        DOCUMENT_READ,
        DOCUMENT_APPLY_EDITS,
        DOCUMENT_UNDO,
        DOCUMENT_REDO,
        DOCUMENT_SAVE,
        DOCUMENT_CHECK_DISK,
        DOCUMENT_CLOSE,
        DOCUMENT_RESOLVE_CONFLICT,
        INDEX_STATUS,
        INDEX_SYMBOLS,
        INDEX_RESCAN,
        RECOVERY_LIST,
        RECOVERY_RESTORE,
        RECOVERY_DISCARD,
        SYSTEM_METRICS,
        SYSTEM_MEMORY,
        CANCEL_REQUEST,
        INTEL_STATUS,
        INTEL_DOCUMENT_SYMBOLS,
        INTEL_WORKSPACE_SYMBOLS,
        INTEL_DEFINITION,
        INTEL_REFERENCES,
        INTEL_HOVER,
        INTEL_COMPLETION,
        INTEL_DIAGNOSTICS,
        INTEL_SERVER,
        GRAPH_SUMMARY,
        GRAPH_NODE,
        GRAPH_QUERY,
        TWIN_IMPACT,
        CHANGES_PROPOSE,
        CHANGES_PREVIEW,
        CHANGES_APPROVE,
        CHANGES_APPLY,
        CHANGES_VERIFY,
        CHANGES_RESTORE,
        CHANGES_GET,
        CHANGES_LIST,
        REFACTOR_RENAME,
    ];
}

/// Notifications sent by the server (only for capabilities the client enabled).
pub mod notifications {
    pub const DOCUMENT_DID_CHANGE: &str = "document/didChange";
    pub const INDEX_PROGRESS: &str = "index/progress";
    pub const SYNTAX_DIAGNOSTICS: &str = "document/syntaxDiagnostics";
    /// The file behind an open document changed, was deleted or reappeared on disk.
    pub const DOCUMENT_DISK_CHANGED: &str = "document/diskChanged";
    /// Files in the workspace changed (from the file watcher), or a rescan happened.
    pub const WORKSPACE_FILES_CHANGED: &str = "workspace/filesChanged";
    /// Crash recovery for a document stopped working (for example the disk is full).
    pub const RECOVERY_DEGRADED: &str = "recovery/degraded";
    /// Another program wrote to the version a save replaced (through a descriptor it opened
    /// before the save); those bytes were preserved at `preservedCopy`.
    pub const DOCUMENT_LATE_WRITE: &str = "document/lateWrite";
    /// Diagnostics from the language server, tagged with the revision they describe.
    pub const SEMANTIC_DIAGNOSTICS: &str = "document/semanticDiagnostics";
    /// The language server changed state (starting, ready, crashed, restarting, failed).
    pub const INTEL_STATE: &str = "intel/state";
    /// The memory-pressure level changed (see `system.metrics` → `resources`).
    pub const RESOURCE_PRESSURE: &str = "system/resourcePressure";
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum PositionEncoding {
    #[serde(rename = "utf-8")]
    Utf8,
    #[serde(rename = "utf-16")]
    Utf16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Position {
    pub line: usize,
    pub character: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Range {
    pub start: Position,
    pub end: Position,
}

// ----- session -----------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HelloParams {
    pub protocol_version: u32,
    #[serde(default)]
    pub client_name: Option<String>,
    /// Encodings the client can use, in preference order. Empty means UTF-16 only.
    #[serde(default)]
    pub position_encodings: Vec<PositionEncoding>,
    #[serde(default)]
    pub capabilities: ClientCapabilities,
    /// Deadline for background requests (search, file listing, recovery listing).
    #[serde(default)]
    pub request_timeout_ms: Option<u64>,
}

/// Notifications the client wants to receive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ClientCapabilities {
    #[serde(default)]
    pub change_notifications: bool,
    #[serde(default)]
    pub index_progress: bool,
    #[serde(default)]
    pub syntax_diagnostics: bool,
    /// `document/diskChanged` for open documents.
    #[serde(default)]
    pub disk_notifications: bool,
    /// `workspace/filesChanged`.
    #[serde(default)]
    pub file_events: bool,
    /// `document/semanticDiagnostics` and `intel/state`.
    #[serde(default)]
    pub semantic_notifications: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerCapabilities {
    pub change_notifications: bool,
    pub index_progress: bool,
    pub syntax_diagnostics: bool,
    pub cancellation: bool,
    pub recovery: bool,
    /// Methods executed off the request loop; their responses may arrive out of order.
    pub background_methods: Vec<String>,
    /// Workspace file watching is active.
    #[serde(default)]
    pub file_watching: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub recovery_durability: Option<RecoveryDurability>,
    /// Language servers configured for this process (empty: semantic queries answer from
    /// the syntax index only, marked as such).
    #[serde(default)]
    pub language_servers: Vec<String>,
}

/// What crash recovery guarantees.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryDurability {
    /// Acknowledged edits survive a crash or kill of `titand`.
    pub process_crash: bool,
    /// After an OS crash or power loss, up to this many milliseconds of the most recent
    /// edits can be lost (journal `fsync` is batched).
    pub power_loss_window_ms: u64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct CancelParams {
    pub id: Value,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HelloResult {
    pub protocol_version: u32,
    pub server_name: String,
    pub server_version: String,
    pub position_encoding: PositionEncoding,
    pub methods: Vec<String>,
    pub capabilities: ServerCapabilities,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HealthResult {
    pub status: String,
    pub uptime_ms: u64,
    pub workspace_root: Option<String>,
    pub open_documents: usize,
}

// ----- workspace ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkspaceOpenParams {
    pub path: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkspaceOpenResult {
    pub root: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkspaceCloseParams {
    #[serde(default)]
    pub discard: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ListFilesParams {
    #[serde(default)]
    pub limit: Option<usize>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ListFilesResult {
    pub files: Vec<String>,
    pub truncated: bool,
}

fn default_true() -> bool {
    true
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchParams {
    pub query: String,
    #[serde(default = "default_true")]
    pub case_sensitive: bool,
    #[serde(default)]
    pub max_results: Option<usize>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum MatchSourceKind {
    Disk,
    Buffer,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchMatch {
    pub path: String,
    pub range: Range,
    pub line_text: String,
    pub source: MatchSourceKind,
    /// Buffer revision the match was found in; absent for disk matches.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub revision: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchResult {
    pub matches: Vec<SearchMatch>,
    pub truncated: bool,
    pub files_searched: usize,
    pub files_skipped: usize,
}

// ----- documents ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentOpenParams {
    pub path: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentInfo {
    pub document_id: u64,
    /// Path relative to the workspace root, `/`-separated.
    pub path: String,
    pub revision: u64,
    pub dirty: bool,
    pub line_ending: String,
    pub has_bom: bool,
    pub line_count: usize,
    pub len_bytes: usize,
    /// Recovery journals left by earlier sessions for this file (see `recovery.list`).
    #[serde(default)]
    pub pending_recovery: Vec<String>,
    /// What the server knows about the file on disk: `inSync`, `modified` (changed by
    /// someone else; buffer kept), `deleted`, or `checking`.
    #[serde(default)]
    pub disk: String,
    /// Crash-recovery state: `disabled`, `clean` (nothing unsaved), `journaling`, or
    /// `failed` (unsaved edits are not protected; see `recovery/degraded`).
    #[serde(default)]
    pub recovery: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentIdParams {
    pub document_id: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentReadResult {
    pub document_id: u64,
    pub revision: u64,
    pub dirty: bool,
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Edit {
    pub range: Range,
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ApplyEditsParams {
    pub document_id: u64,
    pub expected_revision: u64,
    pub edits: Vec<Edit>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RevisionParams {
    pub document_id: u64,
    pub expected_revision: u64,
}

/// Result of `document.applyEdits`, `document.undo` and `document.redo`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EditResult {
    pub document_id: u64,
    /// False when there was nothing to undo/redo or the edit list was empty.
    pub applied: bool,
    pub previous_revision: u64,
    pub revision: u64,
    pub dirty: bool,
    /// Applied edits, sorted, with ranges in coordinates of `previousRevision`.
    pub changes: Vec<Edit>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveParams {
    pub document_id: u64,
    pub expected_revision: u64,
    #[serde(default)]
    pub force: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveResult {
    pub document_id: u64,
    pub revision: u64,
    pub bytes_written: u64,
    pub dirty: bool,
    /// How the file was replaced: `exchange` (atomic swap, verified), `rename`, `createNew`.
    #[serde(default)]
    pub strategy: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CheckDiskResult {
    pub document_id: u64,
    pub changed_on_disk: bool,
    /// Disk status after the check (see `DocumentInfo::disk`).
    #[serde(default)]
    pub disk: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResolveConflictParams {
    pub document_id: u64,
    pub expected_revision: u64,
    /// `keepBuffer`: keep the unsaved text and accept the disk version reported by
    /// `document/diskChanged` as the one the next save may replace. `reloadFromDisk`: replace
    /// the buffer with the disk content (one undoable transaction).
    pub resolution: String,
    /// The `diskHash` the client saw. If the disk has changed again since, the request
    /// fails with `DISK_CHANGED_AGAIN` so the user never overwrites a version they have
    /// not seen.
    #[serde(default)]
    pub disk_hash: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResolveConflictResult {
    pub document: DocumentInfo,
    pub resolution: String,
    /// True when the buffer text changed (reload).
    pub reloaded: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DiskChangedParams {
    pub document_id: u64,
    pub path: String,
    /// `modified`, `deleted` or `restored` (the file is back with the expected content).
    pub kind: String,
    /// `reloaded` (clean buffer replaced with the disk content), `conflict` (the buffer has
    /// unsaved changes and was kept; resolve with `document.resolveConflict`), `none`.
    pub action: String,
    pub dirty: bool,
    pub revision: u64,
    /// BLAKE3 of the disk content (absent when deleted).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub disk_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub disk_len: Option<u64>,
    /// What triggered the check: `watcher`, `checkDisk`, `rescan`.
    pub cause: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileChange {
    pub path: String,
    /// `created`, `modified`, `removed`, `renamed`.
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub is_dir: Option<bool>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FilesChangedParams {
    pub seq: u64,
    /// Events were lost (watcher overflow); the server rescanned instead of listing changes.
    pub rescan: bool,
    pub changes: Vec<FileChange>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryDegradedParams {
    pub document_id: u64,
    pub reason: String,
    /// The server retries with a fresh journal on a later edit.
    pub retrying: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentCloseParams {
    pub document_id: u64,
    #[serde(default)]
    pub discard: bool,
}

// ----- index -------------------------------------------------------------------------------

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexStatusParams {
    /// Answer only once indexing is idle (or `timeoutMs` passes). Does not block other
    /// requests.
    #[serde(default)]
    pub wait_for_idle: bool,
    #[serde(default)]
    pub timeout_ms: Option<u64>,
}

/// Result of `index.status`. Indexing runs in the background; this reports its state.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexStatusResult {
    /// Provenance of every symbol, e.g. `"syntax:tree-sitter-rust"`.
    pub evidence: String,
    pub generation: u64,
    pub idle: bool,
    pub scanning: bool,
    pub pending_parses: usize,
    pub files_indexed: usize,
    pub symbols: usize,
    pub files_with_syntax_errors: usize,
    /// What the last workspace scan did.
    pub reindexed: usize,
    pub unchanged: usize,
    pub removed: usize,
    pub skipped: usize,
    /// Background parse results discarded because the document had moved on.
    pub stale_results_dropped: u64,
    pub incremental_parses: u64,
    pub full_parses: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SymbolsParams {
    pub query: String,
    #[serde(default)]
    pub max_results: Option<usize>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SymbolInfo {
    pub name: String,
    pub kind: String,
    pub path: String,
    /// Range of the symbol's name.
    pub range: Range,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub container_name: Option<String>,
    /// Content the symbol was extracted from: an open buffer (with its revision) or disk.
    pub source: MatchSourceKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub revision: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SymbolsResult {
    pub evidence: String,
    pub generation: u64,
    /// True while background indexing work is pending; results may be incomplete.
    pub indexing: bool,
    pub matches: Vec<SymbolInfo>,
}

// ----- notifications ----------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DidChangeParams {
    pub document_id: u64,
    /// `"edit"`, `"undo"` or `"redo"`.
    pub kind: String,
    pub previous_revision: u64,
    pub revision: u64,
    pub dirty: bool,
    /// Ranges in coordinates of `previousRevision`.
    pub changes: Vec<Edit>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexProgressParams {
    /// `"scanning"` or `"idle"`.
    pub phase: String,
    pub files_done: usize,
    pub files_total: usize,
    pub pending_parses: usize,
    pub generation: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Diagnostic {
    pub range: Range,
    pub severity: String,
    pub message: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SyntaxDiagnosticsParams {
    pub document_id: u64,
    pub revision: u64,
    pub evidence: String,
    pub diagnostics: Vec<Diagnostic>,
}

// ----- recovery ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryJournalInfo {
    pub journal_id: String,
    pub path: Option<String>,
    /// `"recoverable"`, `"conflict"`, `"noChanges"` or `"unrecoverable"`.
    pub state: String,
    /// `"matchesBase"`, `"matchesRecovered"`, `"changedSinceBase"`, `"missing"`, `"unreadable"`.
    pub disk: String,
    pub recovered_revision: Option<u64>,
    pub edit_records: usize,
    pub truncated_tail: bool,
    pub corruption: Option<String>,
    pub reason: Option<String>,
    pub created_unix_ms: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryListResult {
    pub enabled: bool,
    pub journals: Vec<RecoveryJournalInfo>,
    pub removed_expired: usize,
    pub removed_over_budget: usize,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryRestoreParams {
    pub journal_id: String,
    #[serde(default)]
    pub accept_conflict: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryRestoreResult {
    pub document: DocumentInfo,
    pub recovered_revision: u64,
    pub conflict: bool,
    /// The journal ended early (torn tail or corruption): the restored text is the last
    /// consistent state, possibly older than the last edit.
    #[serde(default)]
    pub partial: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub corruption: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct JournalIdParams {
    pub journal_id: String,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn wire_names_are_camel_case() {
        let params: ApplyEditsParams = serde_json::from_value(json!({
            "documentId": 1,
            "expectedRevision": 0,
            "edits": [{"range": {"start": {"line": 0, "character": 2},
                                  "end": {"line": 0, "character": 2}}, "text": "x"}]
        }))
        .unwrap();
        assert_eq!(params.edits[0].range.start.character, 2);

        let hello: HelloParams = serde_json::from_value(json!({
            "protocolVersion": 1,
            "positionEncodings": ["utf-8", "utf-16"]
        }))
        .unwrap();
        assert_eq!(
            hello.position_encodings,
            vec![PositionEncoding::Utf8, PositionEncoding::Utf16]
        );
    }

    #[test]
    fn unknown_encoding_is_rejected() {
        let result: Result<HelloParams, _> = serde_json::from_value(json!({
            "protocolVersion": 1,
            "positionEncodings": ["utf-32"]
        }));
        assert!(result.is_err());
    }

    #[test]
    fn search_defaults() {
        let params: SearchParams = serde_json::from_value(json!({"query": "x"})).unwrap();
        assert!(params.case_sensitive);
        assert_eq!(params.max_results, None);
    }

    #[test]
    fn methods_are_unique() {
        let mut all = methods::ALL.to_vec();
        all.sort_unstable();
        all.dedup();
        assert_eq!(all.len(), methods::ALL.len());
    }
}

/// A position in a document, at a revision.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PositionParams {
    pub document_id: u64,
    /// The revision `position` refers to; must be current (stale requests are refused).
    pub expected_revision: u64,
    pub position: Position,
    /// `intel.references` only.
    #[serde(default)]
    pub include_declaration: bool,
    /// Per-request deadline for the language server, in milliseconds.
    #[serde(default)]
    pub timeout_ms: Option<u64>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WorkspaceSymbolsParams {
    pub query: String,
    #[serde(default)]
    pub max_results: Option<usize>,
    #[serde(default)]
    pub timeout_ms: Option<u64>,
}

/// A location in the workspace. `revision` is set when the location refers to an open
/// document's buffer (positions are then relative to that revision); otherwise it refers to
/// the file on disk.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Location {
    pub path: String,
    pub range: Range,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub revision: Option<u64>,
    /// The location is outside the workspace (for example the standard library).
    #[serde(default)]
    pub external: bool,
}

/// One source's answer to an intelligence query. Results from different sources are never
/// merged: each says where it came from, which revision it describes, whether it is still
/// current, and what it cannot know.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Evidence {
    /// `rust-analyzer`, `tree-sitter`.
    pub source: String,
    /// `semantic` (language server), `syntax` (parse tree), `nameMatch` (syntax index lookup
    /// by identifier: a candidate, not a resolved reference).
    pub kind: String,
    /// `current`, `stale` (computed for an older revision), `unavailable`, `failed`,
    /// `timeout`, `cancelled`.
    pub status: String,
    /// Revision of the queried document the answer was computed against.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub document_revision: Option<u64>,
    /// Server generation (language server restarts) or index generation.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub generation: Option<u64>,
    /// Whether the source considered its own knowledge complete when it answered (for
    /// rust-analyzer: quiescent); `None` when unknown.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub complete: Option<bool>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub limitations: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    pub elapsed_ms: u64,
    pub items: Value,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IntelResult {
    pub document_id: Option<u64>,
    /// Current revision of the document when the response was sent.
    pub current_revision: Option<u64>,
    pub results: Vec<Evidence>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SemanticDiagnosticsParams {
    pub document_id: u64,
    pub source: String,
    /// The document revision the diagnostics describe, when the server said so (LSP
    /// `version`); `None` when the server did not tag them.
    pub revision: Option<u64>,
    /// Revision of the document when they arrived.
    pub received_at_revision: u64,
    /// True only when `revision` equals the current revision.
    pub current: bool,
    pub generation: u64,
    pub diagnostics: Vec<Diagnostic>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GraphNodeParams {
    pub id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GraphQueryParams {
    /// `children`, `parents`, `defines`, `definedIn`, `imports`, `importedBy`, `tests`,
    /// `testsOf`.
    pub relation: String,
    pub id: String,
    #[serde(default)]
    pub max_results: Option<usize>,
}
