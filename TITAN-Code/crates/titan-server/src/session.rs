//! Server state and request handling. Runs only on the event loop thread.

mod changes;
mod disk;
mod displaced;
mod graph;
mod impact;
mod intel;
mod lsp;
mod memory;
mod refactor;
mod resources;
mod watch;

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::mpsc::Sender;
use std::time::{Duration, Instant, SystemTime};

use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};
use titan_doc::{DiskSnapshot, DiskState, DocError, Document, stable_hash};
use titan_index::{
    EVIDENCE, FileIndex, LineCol, ParsedFile, ProjectIndex, SourceVersion, SyntaxParser, analyze,
    input_edits,
};
use titan_protocol::PROTOCOL_VERSION;
use titan_protocol::messages::{
    self, ApplyEditsParams, CancelParams, ClientCapabilities, DidChangeParams, DocumentCloseParams,
    DocumentIdParams, DocumentInfo, DocumentOpenParams, DocumentReadResult, Edit, EditResult,
    HealthResult, HelloParams, HelloResult, IndexProgressParams, IndexStatusParams,
    IndexStatusResult, JournalIdParams, ListFilesParams, ListFilesResult, MatchSourceKind,
    Position, PositionEncoding, Range, RecoveryDegradedParams, RecoveryDurability,
    RecoveryJournalInfo, RecoveryListResult, RecoveryRestoreParams, RecoveryRestoreResult,
    RevisionParams, SaveParams, SaveResult, SearchParams, SearchResult, ServerCapabilities,
    SymbolInfo, SymbolsParams, SymbolsResult, SyntaxDiagnosticsParams, WorkspaceCloseParams,
    WorkspaceOpenParams, WorkspaceOpenResult, methods, notifications,
};
use titan_protocol::rpc::{JSONRPC_VERSION, Request, Response, RpcError, codes};
use titan_recovery::{BaseInfo, JournalConfig, JournalStore, JournalWriter, RecoveryState};
use titan_tasks::{CancelToken, PoolConfig, Priority, TaskId, TaskOutcome, TaskPool, TaskReport};
use titan_text::{
    ByteOffset, Change, ChangeKind, Point, TextBuffer, TextEdit, TextError, TextSnapshot,
    Utf16Point,
};
use titan_workspace::{MatchSource, Overlay, SearchQuery, Workspace, WorkspaceError};
use tree_sitter::Tree;

use crate::{Event, ServerOptions};

pub(crate) use lsp::default_rust_analyzer_settings;

const DEFAULT_LIST_LIMIT: usize = 10_000;
const MAX_LIST_LIMIT: usize = 100_000;
const DEFAULT_SEARCH_RESULTS: usize = 1_000;
const MAX_SEARCH_RESULTS: usize = 10_000;
const DEFAULT_SYMBOL_RESULTS: usize = 200;
const MAX_SYMBOL_RESULTS: usize = 5_000;
const MAX_INDEXED_FILE_BYTES: u64 = 4 * 1024 * 1024;
const MAX_DIAGNOSTICS: usize = 100;
const DEFAULT_WAIT_MS: u64 = 10_000;
const MAX_WAIT_MS: u64 = 120_000;
const JOURNAL_SYNC_INTERVAL: Duration = Duration::from_millis(200);
/// After a journal write fails, wait this long before trying a fresh journal.
const JOURNAL_RETRY: Duration = Duration::from_secs(5);
const IDLE_WAKEUP: Duration = Duration::from_millis(250);
const BACKGROUND_METHODS: &[&str] = &[
    methods::WORKSPACE_SEARCH,
    methods::WORKSPACE_LIST_FILES,
    methods::RECOVERY_LIST,
    methods::INDEX_STATUS,
    methods::DOCUMENT_CHECK_DISK,
    methods::INTEL_DOCUMENT_SYMBOLS,
    methods::INTEL_WORKSPACE_SYMBOLS,
    methods::INTEL_DEFINITION,
    methods::INTEL_REFERENCES,
    methods::INTEL_HOVER,
    methods::INTEL_COMPLETION,
    methods::TWIN_IMPACT,
    methods::CHANGES_VERIFY,
    methods::REFACTOR_RENAME,
];

thread_local! {
    /// One parser per worker thread, reused across tasks.
    static PARSER: RefCell<Option<SyntaxParser>> = const { RefCell::new(None) };
}

fn with_parser<T>(f: impl FnOnce(&mut SyntaxParser) -> Option<T>) -> Option<T> {
    PARSER.with(|cell| {
        let mut slot = cell.borrow_mut();
        if slot.is_none() {
            *slot = SyntaxParser::new().ok();
        }
        slot.as_mut().and_then(f)
    })
}

/// What a background task produced.
pub(crate) enum TaskResult {
    Response(Result<Value, RpcError>),
    Parse {
        tree: Option<Tree>,
        parsed: Option<ParsedFile>,
        incremental: bool,
        facts: Option<Box<titan_index::graph::FileFacts>>,
    },
    Scan(ScanOutput),
    FileIndex(Option<FileIndex>),
    Disk(Result<Box<DiskSnapshot>, String>),
    Graph(Box<graph::GraphOutput>),
}

pub(crate) struct ScanOutput {
    /// `None` entry: unchanged since the version already indexed.
    files: Vec<(PathBuf, Option<FileIndex>)>,
    keep: BTreeSet<PathBuf>,
    skipped: usize,
}

enum Pending {
    Request {
        id: Option<Value>,
        key: Option<String>,
    },
    Parse {
        doc_id: u64,
        revision: u64,
    },
    Scan {
        epoch: u64,
    },
    FileIndex {
        path: PathBuf,
    },
    DiskCheck {
        doc_id: u64,
        epoch: u64,
        cause: &'static str,
    },
    DiskReload {
        doc_id: u64,
        epoch: u64,
        id: Option<Value>,
        expected_revision: u64,
        disk_hash: Option<String>,
    },
    Graph {
        epoch: u64,
    },
    Verify {
        id: Option<Value>,
        proposal_id: String,
    },
}

#[derive(Default)]
struct SyntaxState {
    /// Last parsed tree, edited in step with every change since (valid `old_tree` input).
    tree: Option<Tree>,
    in_flight: Option<TaskId>,
    needs_parse: bool,
    /// Syntax diagnostics of the last published parse, with its revision.
    diagnostics: Option<(u64, Vec<messages::Diagnostic>)>,
}

/// What the server knows about the file behind an open document.
#[derive(Clone, Debug, PartialEq, Eq)]
enum DiskStatus {
    /// The disk holds the version the document is based on.
    InSync,
    /// Someone else changed the file; the buffer was kept (it had unsaved changes).
    Modified(DiskState),
    /// The file is gone.
    Deleted,
    /// The file exists but cannot be loaded (now binary, too large, unreadable).
    Unreadable(String),
}

impl DiskStatus {
    fn as_str(&self) -> &'static str {
        match self {
            Self::InSync => "inSync",
            Self::Modified(_) => "modified",
            Self::Deleted => "deleted",
            Self::Unreadable(_) => "unreadable",
        }
    }
}

struct OpenDocument {
    doc: Document,
    is_rust: bool,
    syntax: SyntaxState,
    journal: Option<JournalWriter>,
    journal_failed: bool,
    /// Earliest time to retry journaling after a failure.
    journal_retry_at: Option<Instant>,
    /// The journal that failed; kept (it is valid up to its last complete record) until a
    /// fresh one has been written.
    failed_journal: Option<JournalWriter>,
    disk_status: DiskStatus,
    /// Incremented whenever the document's disk version changes through this server (save,
    /// reload, resolution). Disk checks started under an older epoch are discarded.
    disk_epoch: u64,
    disk_check: Option<TaskId>,
    disk_recheck: Option<&'static str>,
    /// `document.checkDisk` requests waiting for the check in flight.
    disk_waiters: Vec<Value>,
}

impl OpenDocument {
    fn new(doc: Document, is_rust: bool) -> Self {
        Self {
            doc,
            is_rust,
            syntax: SyntaxState {
                needs_parse: is_rust,
                ..SyntaxState::default()
            },
            journal: None,
            journal_failed: false,
            journal_retry_at: None,
            failed_journal: None,
            disk_status: DiskStatus::InSync,
            disk_epoch: 0,
            disk_check: None,
            disk_recheck: None,
            disk_waiters: Vec::new(),
        }
    }

    fn recovery_state(&self, enabled: bool) -> &'static str {
        if !enabled {
            "disabled"
        } else if self.journal_failed {
            "failed"
        } else if self.journal.is_some() {
            "journaling"
        } else {
            "clean"
        }
    }

    /// Remove journals: nothing unsaved remains.
    fn drop_journals(&mut self) {
        for journal in [self.journal.take(), self.failed_journal.take()]
            .into_iter()
            .flatten()
        {
            if let Err(e) = journal.remove() {
                eprintln!("titand: could not remove journal: {e}");
            }
        }
        self.journal_failed = false;
        self.journal_retry_at = None;
    }
}

struct Waiter {
    id: Value,
    key: String,
    deadline: Instant,
}

#[derive(Clone, Copy, Debug, Default)]
struct Counters {
    requests_cancelled: u64,
    requests_timed_out: u64,
    busy_rejections: u64,
    stale_results_dropped: u64,
    incremental_parses: u64,
    full_parses: u64,
    journal_appends: u64,
    journal_compactions: u64,
    journal_syncs: u64,
    journal_failures: u64,
    journal_retries: u64,
    disk_checks: u64,
    disk_conflicts: u64,
    disk_reloads: u64,
    stale_disk_checks: u64,
    late_writes: u64,
}

#[derive(Clone, Copy, Debug, Default)]
struct ScanCounts {
    reindexed: usize,
    unchanged: usize,
    removed: usize,
    skipped: usize,
}

enum Reply {
    Now(Result<Value, ServerError>),
    Deferred,
}

/// Everything that can go wrong while handling a request, mapped to a stable RPC code.
#[derive(Debug)]
enum ServerError {
    Rpc(RpcError),
    Text(TextError),
    Doc(DocError),
    Workspace(WorkspaceError),
}

impl From<RpcError> for ServerError {
    fn from(err: RpcError) -> Self {
        Self::Rpc(err)
    }
}

impl From<TextError> for ServerError {
    fn from(err: TextError) -> Self {
        Self::Text(err)
    }
}

impl From<DocError> for ServerError {
    fn from(err: DocError) -> Self {
        Self::Doc(err)
    }
}

impl From<WorkspaceError> for ServerError {
    fn from(err: WorkspaceError) -> Self {
        Self::Workspace(err)
    }
}

fn text_error(err: &TextError) -> RpcError {
    match err {
        TextError::StaleRevision { expected, current } => {
            RpcError::new(codes::STALE_REVISION, err.to_string()).with_data(json!({
                "expectedRevision": expected,
                "currentRevision": current,
            }))
        }
        _ => RpcError::new(codes::INVALID_POSITION, err.to_string()),
    }
}

fn workspace_error(err: &WorkspaceError) -> RpcError {
    let code = match err {
        WorkspaceError::Io { .. } => codes::IO_ERROR,
        WorkspaceError::OutsideWorkspace { .. } => codes::PATH_OUTSIDE_WORKSPACE,
        WorkspaceError::NotADirectory { .. } | WorkspaceError::EmptyQuery => codes::INVALID_PARAMS,
    };
    RpcError::new(code, err.to_string())
}

impl ServerError {
    fn into_rpc(self) -> RpcError {
        match self {
            Self::Rpc(err) => err,
            Self::Text(err) | Self::Doc(DocError::Text(err)) => text_error(&err),
            Self::Doc(DocError::ExternalModification {
                path,
                kind,
                preserved_copy,
            }) => {
                let message = format!(
                    "{} changed on disk ({kind}); the buffer was kept",
                    display_path(&path)
                );
                RpcError::new(codes::EXTERNAL_MODIFICATION, message).with_data(json!({
                    "kind": kind.as_str(),
                    "preservedCopy": preserved_copy.as_deref().map(display_path),
                }))
            }
            Self::Doc(err) => {
                let code = match &err {
                    DocError::Io { .. } => codes::IO_ERROR,
                    DocError::TooLarge { .. } => codes::FILE_TOO_LARGE,
                    DocError::ExternalModification { .. } => codes::EXTERNAL_MODIFICATION,
                    DocError::Binary { .. }
                    | DocError::UnsupportedEncoding { .. }
                    | DocError::NotUtf8 { .. } => codes::UNSUPPORTED_CONTENT,
                    DocError::Text(_) => codes::INVALID_POSITION,
                };
                RpcError::new(code, err.to_string())
            }
            Self::Workspace(err) => workspace_error(&err),
        }
    }
}

fn rpc_error(code: i64, message: impl Into<String>) -> ServerError {
    ServerError::Rpc(RpcError::new(code, message))
}

fn parse<T: DeserializeOwned>(params: Value) -> Result<T, ServerError> {
    let params = if params.is_null() { json!({}) } else { params };
    serde_json::from_value(params)
        .map_err(|e| rpc_error(codes::INVALID_PARAMS, format!("invalid params: {e}")))
}

fn to_json<T: Serialize>(value: T) -> Result<Value, ServerError> {
    serde_json::to_value(value).map_err(|e| rpc_error(codes::INTERNAL_ERROR, e.to_string()))
}

fn to_json_rpc<T: Serialize>(value: T) -> Result<Value, RpcError> {
    serde_json::to_value(value).map_err(|e| RpcError::new(codes::INTERNAL_ERROR, e.to_string()))
}

fn request_key(id: &Value) -> String {
    id.to_string()
}

fn to_offset(
    encoding: PositionEncoding,
    buffer: &TextBuffer,
    position: Position,
) -> Result<ByteOffset, TextError> {
    match encoding {
        PositionEncoding::Utf8 => {
            buffer.point_to_byte(Point::new(position.line, position.character))
        }
        PositionEncoding::Utf16 => {
            buffer.utf16_point_to_byte(Utf16Point::new(position.line, position.character))
        }
    }
}

fn to_position(
    encoding: PositionEncoding,
    buffer: &TextBuffer,
    offset: ByteOffset,
) -> Result<Position, TextError> {
    Ok(match encoding {
        PositionEncoding::Utf8 => {
            let point = buffer.byte_to_point(offset)?;
            Position {
                line: point.line,
                character: point.column,
            }
        }
        PositionEncoding::Utf16 => {
            let point = buffer.byte_to_utf16_point(offset)?;
            Position {
                line: point.line,
                character: point.column,
            }
        }
    })
}

fn line_col_position(encoding: PositionEncoding, at: LineCol) -> Position {
    Position {
        line: at.line,
        character: match encoding {
            PositionEncoding::Utf8 => at.byte_column,
            PositionEncoding::Utf16 => at.utf16_column,
        },
    }
}

fn wire_changes(
    encoding: PositionEncoding,
    before: &TextBuffer,
    edits: &[TextEdit],
) -> Result<Vec<Edit>, TextError> {
    edits
        .iter()
        .map(|e| {
            Ok(Edit {
                range: Range {
                    start: to_position(encoding, before, e.start)?,
                    end: to_position(encoding, before, e.end)?,
                },
                text: e.text.clone(),
            })
        })
        .collect()
}

fn kind_name(kind: ChangeKind) -> &'static str {
    match kind {
        ChangeKind::Edit => "edit",
        ChangeKind::Undo => "undo",
        ChangeKind::Redo => "redo",
    }
}

fn check_revision(doc: &Document, expected: u64) -> Result<(), TextError> {
    let current = doc.text().revision();
    if expected != current {
        return Err(TextError::StaleRevision { expected, current });
    }
    Ok(())
}

fn display_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn is_rust_file(path: &Path) -> bool {
    path.extension().is_some_and(|ext| ext == "rs")
}

/// Read a file for indexing: `(version, text)`, or `None` if too large, binary or not UTF-8.
fn read_for_index(path: &Path) -> Option<(SourceVersion, String)> {
    let meta = std::fs::metadata(path).ok()?;
    if meta.len() > MAX_INDEXED_FILE_BYTES {
        return None;
    }
    let bytes = std::fs::read(path).ok()?;
    if bytes.contains(&0) {
        return None;
    }
    let version = SourceVersion::Disk {
        len: bytes.len() as u64,
        hash: stable_hash(&bytes),
    };
    String::from_utf8(bytes).ok().map(|text| (version, text))
}

fn index_text(version: SourceVersion, text: &str) -> Option<FileIndex> {
    let buffer = TextBuffer::new(text);
    let tree = with_parser(|parser| parser.parse(&buffer, None, &|| false).ok().flatten())?;
    let parsed = analyze(&tree, &buffer, 0);
    Some(FileIndex {
        version,
        symbols: parsed.symbols,
        has_syntax_errors: parsed.has_syntax_errors,
    })
}

fn scan_workspace(
    workspace: &Workspace,
    known: &HashMap<PathBuf, SourceVersion>,
    token: &CancelToken,
    progress: &dyn Fn(usize, usize),
) -> ScanOutput {
    let listing = workspace.list_files(MAX_LIST_LIMIT);
    let rust: Vec<&PathBuf> = listing.files.iter().filter(|p| is_rust_file(p)).collect();
    let total = rust.len();
    let mut out = ScanOutput {
        files: Vec::new(),
        keep: BTreeSet::new(),
        skipped: 0,
    };
    progress(0, total);
    for (i, rel) in rust.into_iter().enumerate() {
        if token.is_cancelled() {
            break;
        }
        let path = workspace.root().join(rel);
        out.keep.insert(path.clone());
        match read_for_index(&path) {
            None => out.skipped += 1,
            Some((version, _)) if known.get(&path) == Some(&version) => {
                out.files.push((path, None));
            }
            Some((version, text)) => match index_text(version, &text) {
                Some(entry) => out.files.push((path, Some(entry))),
                None => out.skipped += 1,
            },
        }
        if (i + 1) % 32 == 0 {
            progress(i + 1, total);
        }
    }
    progress(total, total);
    out
}

/// Recovery journal store for `options`; `None` disables recovery.
fn journal_store(options: &ServerOptions) -> Option<JournalStore> {
    options
        .state_dir
        .as_ref()
        .map(|dir| JournalStore::new(JournalConfig::new(dir.join("recovery"))))
}

pub(crate) struct Session {
    options: ServerOptions,
    initialized: bool,
    shutdown_requested: bool,
    encoding: PositionEncoding,
    caps: ClientCapabilities,
    request_timeout: Duration,
    workspace: Option<Workspace>,
    documents: BTreeMap<u64, OpenDocument>,
    by_path: HashMap<PathBuf, u64>,
    next_document_id: u64,
    started: Instant,
    pool: TaskPool<TaskResult>,
    events: Sender<Event>,
    pending: HashMap<TaskId, Pending>,
    request_tasks: HashMap<String, TaskId>,
    waiters: Vec<Waiter>,
    index: ProjectIndex,
    scan_epoch: u64,
    scanning: Option<TaskId>,
    scan_progress: (usize, usize),
    last_scan: ScanCounts,
    journals: Option<JournalStore>,
    last_sync: Instant,
    counters: Counters,
    outgoing: Vec<Vec<u8>>,
    was_idle: bool,
    watch: watch::WatchState,
    graph: graph::GraphState,
    semantic: lsp::Semantic,
    /// Replaced file versions kept open after saves (late-writer detection).
    displaced: Vec<displaced::Watched>,
    changes: changes::ChangesState,
    resources: resources::ResourceState,
}

impl Session {
    pub(crate) fn new(options: ServerOptions, events: Sender<Event>) -> io::Result<Self> {
        let task_events = events.clone();
        let pool = TaskPool::new(
            PoolConfig {
                workers: options.workers,
                queue_capacity: options.queue_capacity,
                low_priority_every: 8,
            },
            move |report| {
                let _ = task_events.send(Event::Task(report));
            },
        )?;
        let index = ProjectIndex::new().map_err(io::Error::other)?;
        let options_lsp_name = options.lsp.as_ref().map(|l| l.name.clone());
        Ok(Self {
            journals: journal_store(&options),
            request_timeout: options.default_request_timeout,
            options,
            initialized: false,
            shutdown_requested: false,
            encoding: PositionEncoding::Utf16,
            caps: ClientCapabilities::default(),
            workspace: None,
            documents: BTreeMap::new(),
            by_path: HashMap::new(),
            next_document_id: 1,
            started: Instant::now(),
            pool,
            events,
            pending: HashMap::new(),
            request_tasks: HashMap::new(),
            waiters: Vec::new(),
            index,
            scan_epoch: 0,
            scanning: None,
            scan_progress: (0, 0),
            last_scan: ScanCounts::default(),
            last_sync: Instant::now(),
            counters: Counters::default(),
            outgoing: Vec::new(),
            was_idle: true,
            watch: watch::WatchState::default(),
            graph: graph::GraphState::default(),
            displaced: Vec::new(),
            changes: changes::ChangesState::default(),
            resources: resources::ResourceState::default(),
            semantic: lsp::Semantic {
                name: options_lsp_name,
                ..lsp::Semantic::default()
            },
        })
    }

    pub(crate) fn shutdown_requested(&self) -> bool {
        self.shutdown_requested
    }

    pub(crate) fn take_outgoing(&mut self) -> Vec<Vec<u8>> {
        std::mem::take(&mut self.outgoing)
    }

    // ----- output -------------------------------------------------------------------

    fn push_response(&mut self, response: Response) {
        let id = response.id.clone();
        match serde_json::to_vec(&response) {
            Ok(body) if body.len() <= self.options.max_response_bytes => self.outgoing.push(body),
            Ok(body) => {
                let error = RpcError::new(
                    codes::RESPONSE_TOO_LARGE,
                    format!(
                        "response of {} bytes exceeds the {}-byte limit",
                        body.len(),
                        self.options.max_response_bytes
                    ),
                );
                if let Ok(body) = serde_json::to_vec(&Response::failure(id, error)) {
                    self.outgoing.push(body);
                }
            }
            Err(e) => {
                let error = RpcError::new(codes::INTERNAL_ERROR, e.to_string());
                if let Ok(body) = serde_json::to_vec(&Response::failure(id, error)) {
                    self.outgoing.push(body);
                }
            }
        }
    }

    fn push_notification<T: Serialize>(&mut self, method: &str, params: T) {
        let Ok(params) = serde_json::to_value(params) else {
            return;
        };
        let message = json!({ "jsonrpc": JSONRPC_VERSION, "method": method, "params": params });
        if let Ok(body) = serde_json::to_vec(&message) {
            self.outgoing.push(body);
        }
    }

    fn respond(&mut self, id: Value, result: Result<Value, ServerError>) {
        let response = match result {
            Ok(value) => Response::success(id, value),
            Err(err) => {
                let err = err.into_rpc();
                eprintln!("titand: error {}: {}", err.code, err.message);
                Response::failure(id, err)
            }
        };
        self.push_response(response);
    }

    pub(crate) fn on_framing_error(&mut self, message: &str) {
        self.push_response(Response::failure(
            Value::Null,
            RpcError::new(
                codes::INVALID_REQUEST,
                format!("framing error: {message}; closing the connection"),
            ),
        ));
    }

    // ----- incoming -----------------------------------------------------------------

    pub(crate) fn on_message(&mut self, body: &[u8]) {
        let value: Value = match serde_json::from_slice(body) {
            Ok(value) => value,
            Err(e) => {
                self.push_response(Response::failure(
                    Value::Null,
                    RpcError::new(codes::PARSE_ERROR, format!("invalid JSON: {e}")),
                ));
                return;
            }
        };
        // A present `"id": null` is a request; only an absent id makes a notification.
        let has_id = value.get("id").is_some();
        let raw_id = value.get("id").cloned().unwrap_or(Value::Null);
        if !matches!(raw_id, Value::Null | Value::Number(_) | Value::String(_)) {
            self.push_response(Response::failure(
                Value::Null,
                RpcError::new(codes::INVALID_REQUEST, "id must be a number or a string"),
            ));
            return;
        }
        let request: Request = match serde_json::from_value(value) {
            Ok(request) => request,
            Err(e) => {
                self.push_response(Response::failure(
                    raw_id,
                    RpcError::new(codes::INVALID_REQUEST, format!("invalid request: {e}")),
                ));
                return;
            }
        };
        if request.jsonrpc != JSONRPC_VERSION {
            self.push_response(Response::failure(
                raw_id,
                RpcError::new(codes::INVALID_REQUEST, "jsonrpc must be \"2.0\""),
            ));
            return;
        }
        let id = has_id.then_some(raw_id);
        if let Reply::Now(result) = self.dispatch(&request.method, request.params, id.clone()) {
            if let Some(id) = id {
                self.respond(id, result);
            }
        }
    }

    fn dispatch(&mut self, method: &str, params: Value, id: Option<Value>) -> Reply {
        if !self.initialized && method != methods::SESSION_HELLO {
            return Reply::Now(Err(rpc_error(
                codes::NOT_INITIALIZED,
                "session.hello must be the first request",
            )));
        }
        match method {
            methods::SESSION_HELLO => Reply::Now(parse(params).and_then(|p| self.hello(p))),
            methods::SESSION_SHUTDOWN => {
                self.shutdown_requested = true;
                Reply::Now(Ok(Value::Null))
            }
            methods::CANCEL_REQUEST => {
                if let Ok(p) = parse::<CancelParams>(params) {
                    self.cancel_request(&p.id);
                }
                Reply::Now(Ok(Value::Null))
            }
            methods::SYSTEM_HEALTH => Reply::Now(to_json(self.health())),
            methods::SYSTEM_METRICS => Reply::Now(Ok(self.metrics())),
            methods::SYSTEM_MEMORY => Reply::Now(Ok(self.memory_report())),
            methods::WORKSPACE_OPEN => {
                Reply::Now(parse(params).and_then(|p| self.workspace_open(p)))
            }
            methods::WORKSPACE_CLOSE => {
                Reply::Now(parse(params).and_then(|p| self.workspace_close(p)))
            }
            methods::WORKSPACE_LIST_FILES => match parse(params) {
                Ok(p) => self.list_files(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::WORKSPACE_SEARCH => match parse(params) {
                Ok(p) => self.search(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::DOCUMENT_OPEN => Reply::Now(
                parse(params)
                    .and_then(|p| self.document_open(p))
                    .and_then(to_json),
            ),
            methods::DOCUMENT_READ => Reply::Now(parse(params).and_then(|p| self.document_read(p))),
            methods::DOCUMENT_APPLY_EDITS => {
                Reply::Now(parse(params).and_then(|p| self.apply_edits(p)))
            }
            methods::DOCUMENT_UNDO => {
                Reply::Now(parse(params).and_then(|p| self.undo_redo(p, false)))
            }
            methods::DOCUMENT_REDO => {
                Reply::Now(parse(params).and_then(|p| self.undo_redo(p, true)))
            }
            methods::DOCUMENT_SAVE => Reply::Now(parse(params).and_then(|p| self.save(p))),
            methods::DOCUMENT_CHECK_DISK => match parse(params) {
                Ok(p) => self.check_disk(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::DOCUMENT_RESOLVE_CONFLICT => match parse(params) {
                Ok(p) => self.resolve_conflict(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::DOCUMENT_CLOSE => {
                Reply::Now(parse(params).and_then(|p| self.document_close(p)))
            }
            methods::INDEX_STATUS => match parse(params) {
                Ok(p) => self.index_status(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::INDEX_SYMBOLS => Reply::Now(parse(params).and_then(|p| self.index_symbols(p))),
            methods::INDEX_RESCAN => Reply::Now(self.index_rescan()),
            methods::RECOVERY_LIST => self.recovery_list(id),
            methods::GRAPH_SUMMARY => Reply::Now(self.graph_summary()),
            methods::CHANGES_PROPOSE => Reply::Now(self.changes_propose(params)),
            methods::CHANGES_PREVIEW => Reply::Now(self.changes_preview(params)),
            methods::CHANGES_APPROVE => Reply::Now(self.changes_approve(params)),
            methods::CHANGES_APPLY => Reply::Now(self.changes_apply(params)),
            methods::CHANGES_RESTORE => Reply::Now(self.changes_restore(params)),
            methods::CHANGES_GET => Reply::Now(self.changes_get(params)),
            methods::CHANGES_LIST => Reply::Now(Ok(self.changes_list())),
            methods::INTEL_SERVER => Reply::Now(self.intel_server(params)),
            methods::CHANGES_VERIFY => self.changes_verify(params, id),
            methods::REFACTOR_RENAME => self.refactor_rename(params, id),
            methods::TWIN_IMPACT => match parse(params) {
                Ok(p) => self.twin_impact(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::INTEL_STATUS => Reply::Now(self.intel_status()),
            methods::INTEL_DIAGNOSTICS => Reply::Now(
                parse::<DocumentIdParams>(params)
                    .and_then(|p| self.intel_diagnostics(p.document_id)),
            ),
            methods::INTEL_DOCUMENT_SYMBOLS => match parse::<DocumentIdParams>(params) {
                Ok(p) => self.intel_document_symbols(p.document_id, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::INTEL_WORKSPACE_SYMBOLS => match parse(params) {
                Ok(p) => self.intel_workspace_symbols(p, id),
                Err(e) => Reply::Now(Err(e)),
            },
            methods::INTEL_DEFINITION
            | methods::INTEL_REFERENCES
            | methods::INTEL_HOVER
            | methods::INTEL_COMPLETION => {
                let query = match method {
                    methods::INTEL_DEFINITION => intel::Query::Definition,
                    methods::INTEL_REFERENCES => intel::Query::References,
                    methods::INTEL_HOVER => intel::Query::Hover,
                    _ => intel::Query::Completion,
                };
                match parse(params) {
                    Ok(p) => self.intel_position(query, p, id),
                    Err(e) => Reply::Now(Err(e)),
                }
            }
            methods::GRAPH_NODE => Reply::Now(parse(params).and_then(|p| self.graph_node(p))),
            methods::GRAPH_QUERY => Reply::Now(parse(params).and_then(|p| self.graph_query(p))),
            methods::RECOVERY_RESTORE => {
                Reply::Now(parse(params).and_then(|p| self.recovery_restore(p)))
            }
            methods::RECOVERY_DISCARD => {
                Reply::Now(parse(params).and_then(|p| self.recovery_discard(p)))
            }
            other => Reply::Now(Err(rpc_error(
                codes::METHOD_NOT_FOUND,
                format!("unknown method {other:?}"),
            ))),
        }
    }

    // ----- background tasks ---------------------------------------------------------

    fn submit_request<F>(
        &mut self,
        id: Option<Value>,
        name: &'static str,
        priority: Priority,
        job: F,
    ) -> Reply
    where
        F: FnOnce(&CancelToken) -> Result<Value, RpcError> + Send + 'static,
    {
        let submitted =
            self.pool
                .submit(name, priority, Some(self.request_timeout), move |token| {
                    TaskResult::Response(job(token))
                });
        match submitted {
            Ok((task, _)) => {
                let key = id.as_ref().map(request_key);
                if let Some(key) = &key {
                    self.request_tasks.insert(key.clone(), task);
                }
                self.pending.insert(task, Pending::Request { id, key });
                Reply::Deferred
            }
            Err(e) => {
                self.counters.busy_rejections += 1;
                Reply::Now(Err(rpc_error(codes::SERVER_BUSY, e.to_string())))
            }
        }
    }

    fn cancel_request(&mut self, id: &Value) {
        let key = request_key(id);
        if self.cancel_intel_request(&key) {
            return;
        }
        if let Some(task) = self.request_tasks.get(&key) {
            // The task reports `Cancelled`; the response is sent then.
            self.pool.cancel(*task);
            return;
        }
        if let Some(pos) = self.waiters.iter().position(|w| w.key == key) {
            let waiter = self.waiters.remove(pos);
            self.counters.requests_cancelled += 1;
            self.push_response(Response::failure(
                waiter.id,
                RpcError::new(codes::REQUEST_CANCELLED, "request cancelled"),
            ));
        }
    }

    pub(crate) fn on_task(&mut self, report: TaskReport<TaskResult>) {
        let Some(pending) = self.pending.remove(&report.id) else {
            return;
        };
        match pending {
            Pending::Request { id, key } => {
                if let Some(key) = key {
                    self.request_tasks.remove(&key);
                }
                let result = match report.outcome {
                    TaskOutcome::Completed(TaskResult::Response(result)) => result,
                    TaskOutcome::Completed(_) => Err(RpcError::new(
                        codes::INTERNAL_ERROR,
                        "unexpected task result",
                    )),
                    TaskOutcome::Cancelled => {
                        self.counters.requests_cancelled += 1;
                        Err(RpcError::new(codes::REQUEST_CANCELLED, "request cancelled"))
                    }
                    TaskOutcome::TimedOut => {
                        self.counters.requests_timed_out += 1;
                        Err(RpcError::new(
                            codes::REQUEST_TIMEOUT,
                            format!(
                                "request exceeded its {} ms deadline",
                                self.request_timeout.as_millis()
                            ),
                        ))
                    }
                    TaskOutcome::Panicked(message) => Err(RpcError::new(
                        codes::INTERNAL_ERROR,
                        format!("internal error: {message}"),
                    )),
                };
                if let Some(id) = id {
                    let response = match result {
                        Ok(value) => Response::success(id, value),
                        Err(err) => Response::failure(id, err),
                    };
                    self.push_response(response);
                }
            }
            Pending::Parse { doc_id, revision } => {
                self.on_parse_done(doc_id, revision, report.outcome);
            }
            Pending::Scan { epoch } => self.on_scan_done(epoch, report.outcome),
            Pending::Graph { epoch } => self.on_graph_built(epoch, report.outcome),
            Pending::Verify { id, proposal_id } => {
                let result = match report.outcome {
                    TaskOutcome::Completed(TaskResult::Response(r)) => r,
                    other => Err(RpcError::new(
                        codes::INTERNAL_ERROR,
                        format!(
                            "verification did not complete: {}",
                            task_outcome_name(&other)
                        ),
                    )),
                };
                self.on_verified(id, proposal_id, result);
            }
            Pending::DiskCheck {
                doc_id,
                epoch,
                cause,
            } => self.on_disk_checked(doc_id, epoch, cause, report.outcome),
            Pending::DiskReload {
                doc_id,
                epoch,
                id,
                expected_revision,
                disk_hash,
            } => self.on_disk_reloaded(
                doc_id,
                epoch,
                id,
                expected_revision,
                disk_hash,
                report.outcome,
            ),
            Pending::FileIndex { path } => {
                if !self.by_path.contains_key(&path) {
                    match report.outcome {
                        TaskOutcome::Completed(TaskResult::FileIndex(Some(entry))) => {
                            self.index.publish(&path, entry);
                        }
                        TaskOutcome::Completed(TaskResult::FileIndex(None)) => {
                            self.index.remove(&path);
                        }
                        _ => {}
                    }
                }
            }
        }
    }

    // ----- periodic work ------------------------------------------------------------

    pub(crate) fn next_wakeup(&self) -> Duration {
        let now = Instant::now();
        self.waiters
            .iter()
            .map(|w| w.deadline.saturating_duration_since(now))
            .min()
            .map_or(IDLE_WAKEUP, |d| d.min(IDLE_WAKEUP))
    }

    pub(crate) fn on_tick(&mut self) {
        if self.last_sync.elapsed() >= JOURNAL_SYNC_INTERVAL {
            self.last_sync = Instant::now();
            for od in self.documents.values_mut() {
                if let Some(journal) = od.journal.as_mut() {
                    match journal.sync() {
                        Ok(()) => self.counters.journal_syncs += 1,
                        Err(e) => {
                            eprintln!("titand: journal sync failed: {e}");
                            self.counters.journal_failures += 1;
                        }
                    }
                }
            }
        }
        // Parses that could not be queued earlier (queue full) are retried here.
        let retry: Vec<u64> = self
            .documents
            .iter()
            .filter(|(_, od)| od.syntax.needs_parse && od.syntax.in_flight.is_none())
            .map(|(id, _)| *id)
            .collect();
        for id in retry {
            self.schedule_parse(id);
        }
        self.check_resources();
        self.apply_pending_graph_updates();
        self.maybe_build_graph();
        self.check_displaced();
        self.ensure_lsp_synced();
        self.check_idle();
    }

    fn index_idle(&self) -> bool {
        self.scanning.is_none()
            && self
                .documents
                .values()
                .all(|od| od.syntax.in_flight.is_none() && !od.syntax.needs_parse)
            && !self
                .pending
                .values()
                .any(|p| matches!(p, Pending::FileIndex { .. }))
    }

    fn pending_parses(&self) -> usize {
        self.documents
            .values()
            .filter(|od| od.syntax.in_flight.is_some() || od.syntax.needs_parse)
            .count()
    }

    fn check_idle(&mut self) {
        let idle = self.index_idle();
        let now = Instant::now();
        if !self.waiters.is_empty() {
            let (ready, waiting): (Vec<Waiter>, Vec<Waiter>) = std::mem::take(&mut self.waiters)
                .into_iter()
                .partition(|w| idle || w.deadline <= now);
            self.waiters = waiting;
            for waiter in ready {
                let status = to_json(self.status());
                self.respond(waiter.id, status);
            }
        }
        if idle != self.was_idle && self.caps.index_progress {
            let params = IndexProgressParams {
                phase: if idle { "idle" } else { "indexing" }.to_string(),
                files_done: self.scan_progress.0,
                files_total: self.scan_progress.1,
                pending_parses: self.pending_parses(),
                generation: self.index.generation(),
            };
            self.push_notification(notifications::INDEX_PROGRESS, params);
        }
        self.was_idle = idle;
    }

    pub(crate) fn on_scan_progress(&mut self, epoch: u64, done: usize, total: usize) {
        if epoch != self.scan_epoch {
            return;
        }
        self.scan_progress = (done, total);
        if self.caps.index_progress {
            let params = IndexProgressParams {
                phase: "scanning".to_string(),
                files_done: done,
                files_total: total,
                pending_parses: self.pending_parses(),
                generation: self.index.generation(),
            };
            self.push_notification(notifications::INDEX_PROGRESS, params);
        }
    }

    /// Cancel background work. Documents with unsaved changes keep their (synced) journals;
    /// other journals are removed. Pending requests are answered as cancelled.
    pub(crate) fn close(&mut self) {
        self.release_displaced();
        self.stop_watcher();
        self.stop_language_server(true);
        self.pool.cancel_all();
        for od in self.documents.values_mut() {
            if let Some(mut journal) = od.journal.take() {
                if od.doc.is_dirty() {
                    let _ = journal.sync();
                } else {
                    let _ = journal.remove();
                }
            }
        }
        let waiting = std::mem::take(&mut self.waiters);
        let disk_waiting: Vec<Value> = self
            .documents
            .values_mut()
            .flat_map(|od| std::mem::take(&mut od.disk_waiters))
            .collect();
        let pending: Vec<Value> = self
            .pending
            .values()
            .filter_map(|p| match p {
                Pending::Request { id, .. }
                | Pending::DiskReload { id, .. }
                | Pending::Verify { id, .. } => id.clone(),
                _ => None,
            })
            .chain(waiting.into_iter().map(|w| w.id))
            .chain(disk_waiting)
            .collect();
        self.pending.clear();
        for id in pending {
            self.push_response(Response::failure(
                id,
                RpcError::new(codes::REQUEST_CANCELLED, "the server is shutting down"),
            ));
        }
    }

    // ----- session ------------------------------------------------------------------

    fn hello(&mut self, params: HelloParams) -> Result<Value, ServerError> {
        if params.protocol_version != PROTOCOL_VERSION {
            return Err(RpcError::new(
                codes::UNSUPPORTED_PROTOCOL_VERSION,
                format!(
                    "protocol version {} is not supported",
                    params.protocol_version
                ),
            )
            .with_data(json!({ "supported": [PROTOCOL_VERSION] }))
            .into());
        }
        self.encoding = params
            .position_encodings
            .first()
            .copied()
            .unwrap_or(PositionEncoding::Utf16);
        self.caps = params.capabilities;
        if let Some(ms) = params.request_timeout_ms {
            self.request_timeout = Duration::from_millis(ms.max(1));
        }
        self.initialized = true;
        to_json(HelloResult {
            protocol_version: PROTOCOL_VERSION,
            server_name: "titand".to_string(),
            server_version: env!("CARGO_PKG_VERSION").to_string(),
            position_encoding: self.encoding,
            methods: methods::ALL.iter().map(|m| (*m).to_string()).collect(),
            capabilities: ServerCapabilities {
                change_notifications: true,
                index_progress: true,
                syntax_diagnostics: true,
                cancellation: true,
                recovery: self.journals.is_some(),
                background_methods: BACKGROUND_METHODS
                    .iter()
                    .map(|m| (*m).to_string())
                    .collect(),
                file_watching: self.options.watch,
                language_servers: self.semantic.name.iter().cloned().collect(),
                recovery_durability: self.journals.as_ref().map(|_| RecoveryDurability {
                    process_crash: true,
                    power_loss_window_ms: u64::try_from(JOURNAL_SYNC_INTERVAL.as_millis())
                        .unwrap_or(u64::MAX),
                }),
            },
        })
    }

    fn health(&self) -> HealthResult {
        HealthResult {
            status: "ok".to_string(),
            uptime_ms: u64::try_from(self.started.elapsed().as_millis()).unwrap_or(u64::MAX),
            workspace_root: self.workspace.as_ref().map(|ws| display_path(ws.root())),
            open_documents: self.documents.len(),
        }
    }

    fn metrics(&self) -> Value {
        let tasks = self.pool.metrics();
        let c = self.counters;
        json!({
            "tasks": {
                "submitted": tasks.submitted,
                "completed": tasks.completed,
                "cancelled": tasks.cancelled,
                "timedOut": tasks.timed_out,
                "panicked": tasks.panicked,
                "rejected": tasks.rejected,
                "queued": tasks.queued,
                "running": tasks.running,
                "maxQueueDepth": tasks.max_queue_depth,
                "totalQueueWaitUs": tasks.total_queue_wait_us,
                "totalRunUs": tasks.total_run_us,
            },
            "requests": {
                "cancelled": c.requests_cancelled,
                "timedOut": c.requests_timed_out,
                "busyRejections": c.busy_rejections,
            },
            "parses": {
                "incremental": c.incremental_parses,
                "full": c.full_parses,
                "staleResultsDropped": c.stale_results_dropped,
            },
            "journal": {
                "enabled": self.journals.is_some(),
                "appends": c.journal_appends,
                "compactions": c.journal_compactions,
                "syncs": c.journal_syncs,
                "failures": c.journal_failures,
                "retries": c.journal_retries,
            },
            "resources": self.resources_json(),
            "watch": self.watch.metrics(),
            "lsp": self.lsp_metrics(),
            "graph": self.graph_metrics(),
            "disk": {
                "checks": c.disk_checks,
                "conflicts": c.disk_conflicts,
                "reloads": c.disk_reloads,
                "staleChecksDiscarded": c.stale_disk_checks,
                "lateWrites": c.late_writes,
                "replacedVersionsWatched": self.displaced.len(),
            },
        })
    }

    // ----- workspace ----------------------------------------------------------------

    fn workspace(&self) -> Result<&Workspace, ServerError> {
        self.workspace
            .as_ref()
            .ok_or_else(|| rpc_error(codes::NO_WORKSPACE, "no workspace is open"))
    }

    fn ensure_no_unsaved(&self, discard: bool) -> Result<(), ServerError> {
        if discard {
            return Ok(());
        }
        let dirty: Vec<u64> = self
            .documents
            .iter()
            .filter(|(_, od)| od.doc.is_dirty())
            .map(|(id, _)| *id)
            .collect();
        if dirty.is_empty() {
            return Ok(());
        }
        Err(RpcError::new(
            codes::UNSAVED_CHANGES,
            "open documents have unsaved changes",
        )
        .with_data(json!({ "documentIds": dirty }))
        .into())
    }

    fn close_all_documents(&mut self) {
        for (_, mut od) in std::mem::take(&mut self.documents) {
            if let Some(task) = od.syntax.in_flight {
                self.pool.cancel(task);
            }
            od.drop_journals();
        }
        self.by_path.clear();
    }

    fn reset_index(&mut self) -> Result<(), ServerError> {
        if let Some(task) = self.scanning.take() {
            self.pool.cancel(task);
        }
        self.scan_epoch += 1;
        self.index =
            ProjectIndex::new().map_err(|e| rpc_error(codes::INTERNAL_ERROR, e.to_string()))?;
        self.last_scan = ScanCounts::default();
        self.scan_progress = (0, 0);
        Ok(())
    }

    fn workspace_open(&mut self, params: WorkspaceOpenParams) -> Result<Value, ServerError> {
        self.ensure_no_unsaved(false)?;
        let workspace = Workspace::open(Path::new(&params.path))?;
        let root = display_path(workspace.root());
        self.close_all_documents();
        let root_path = workspace.root().to_path_buf();
        self.workspace = Some(workspace);
        self.reset_index()?;
        self.reset_graph();
        self.start_scan();
        self.start_watcher(&root_path);
        self.recover_changes();
        self.start_language_server(&root_path);
        to_json(WorkspaceOpenResult { root })
    }

    fn workspace_close(&mut self, params: WorkspaceCloseParams) -> Result<Value, ServerError> {
        self.ensure_no_unsaved(params.discard)?;
        self.stop_watcher();
        self.stop_language_server(false);
        self.close_all_documents();
        self.workspace = None;
        self.reset_index()?;
        self.reset_graph();
        Ok(Value::Null)
    }

    fn list_files(&mut self, params: ListFilesParams, id: Option<Value>) -> Reply {
        let workspace = match self.workspace() {
            Ok(ws) => ws.clone(),
            Err(e) => return Reply::Now(Err(e)),
        };
        let limit = params
            .limit
            .unwrap_or(DEFAULT_LIST_LIMIT)
            .min(MAX_LIST_LIMIT);
        self.submit_request(id, "workspace.listFiles", Priority::Normal, move |_| {
            let list = workspace.list_files(limit);
            to_json_rpc(ListFilesResult {
                files: list
                    .files
                    .iter()
                    .map(|p| display_path(p.as_path()))
                    .collect(),
                truncated: list.truncated,
            })
        })
    }

    fn search(&mut self, params: SearchParams, id: Option<Value>) -> Reply {
        let workspace = match self.workspace() {
            Ok(ws) => ws.clone(),
            Err(e) => return Reply::Now(Err(e)),
        };
        let mut query = SearchQuery::literal(params.query);
        query.case_sensitive = params.case_sensitive;
        query.max_results = params
            .max_results
            .unwrap_or(DEFAULT_SEARCH_RESULTS)
            .min(MAX_SEARCH_RESULTS);
        // O(1) snapshots on the loop; their text is materialised on the worker.
        let snapshots: Vec<(PathBuf, TextSnapshot)> = self
            .documents
            .values()
            .map(|od| (od.doc.path().to_path_buf(), od.doc.text().snapshot()))
            .collect();
        let utf16 = self.encoding == PositionEncoding::Utf16;
        self.submit_request(id, "workspace.search", Priority::Normal, move |token| {
            let overlays: HashMap<PathBuf, Overlay> = snapshots
                .into_iter()
                .map(|(path, snap)| {
                    let overlay = Overlay {
                        text: snap.buffer().text(),
                        revision: snap.revision(),
                    };
                    (path, overlay)
                })
                .collect();
            let found = workspace
                .search_cancellable(&query, &overlays, &|| token.is_cancelled())
                .map_err(|e| workspace_error(&e))?;
            let matches = found
                .matches
                .into_iter()
                .map(|m| {
                    let (start, end) = if utf16 {
                        (m.column_utf16, m.end_column_utf16)
                    } else {
                        (m.column, m.end_column)
                    };
                    let (source, revision) = match m.source {
                        MatchSource::Disk => (MatchSourceKind::Disk, None),
                        MatchSource::Buffer { revision } => {
                            (MatchSourceKind::Buffer, Some(revision))
                        }
                    };
                    messages::SearchMatch {
                        path: display_path(&m.path),
                        range: Range {
                            start: Position {
                                line: m.line,
                                character: start,
                            },
                            end: Position {
                                line: m.line,
                                character: end,
                            },
                        },
                        line_text: m.line_text,
                        source,
                        revision,
                    }
                })
                .collect();
            to_json_rpc(SearchResult {
                matches,
                truncated: found.truncated,
                files_searched: found.files_searched,
                files_skipped: found.files_skipped,
            })
        })
    }

    // ----- documents ----------------------------------------------------------------

    fn open_doc(&self, id: u64) -> Result<&OpenDocument, ServerError> {
        self.documents.get(&id).ok_or_else(|| {
            rpc_error(
                codes::DOCUMENT_NOT_FOUND,
                format!("document {id} is not open"),
            )
        })
    }

    fn open_doc_mut(&mut self, id: u64) -> Result<&mut OpenDocument, ServerError> {
        self.documents.get_mut(&id).ok_or_else(|| {
            rpc_error(
                codes::DOCUMENT_NOT_FOUND,
                format!("document {id} is not open"),
            )
        })
    }

    fn active_journals(&self) -> HashSet<PathBuf> {
        self.documents
            .values()
            .filter_map(|od| od.journal.as_ref().map(|j| j.path().to_path_buf()))
            .collect()
    }

    fn info(&self, id: u64) -> Result<DocumentInfo, ServerError> {
        let od = self.open_doc(id)?;
        let workspace = self.workspace()?;
        let path = workspace
            .relative(od.doc.path())
            .map(|p| display_path(&p))
            .unwrap_or_else(|| display_path(od.doc.path()));
        let pending_recovery = self
            .journals
            .as_ref()
            .map(|store| store.ids_for_document(od.doc.path(), &self.active_journals()))
            .unwrap_or_default();
        let buffer = od.doc.text().buffer();
        Ok(DocumentInfo {
            document_id: id,
            path,
            revision: od.doc.text().revision(),
            dirty: od.doc.is_dirty(),
            line_ending: od.doc.line_ending().as_str().to_string(),
            has_bom: od.doc.has_bom(),
            line_count: buffer.line_count(),
            len_bytes: buffer.len_bytes(),
            pending_recovery,
            disk: od.disk_status.as_str().to_string(),
            recovery: od.recovery_state(self.journals.is_some()).to_string(),
        })
    }

    fn document_open(&mut self, params: DocumentOpenParams) -> Result<DocumentInfo, ServerError> {
        let path = self
            .workspace()?
            .resolve_existing(Path::new(&params.path))?;
        if let Some(&id) = self.by_path.get(&path) {
            return self.info(id);
        }
        let doc = Document::open(&path, &self.options.limits)?;
        let id = self.next_document_id;
        self.next_document_id += 1;
        let is_rust = is_rust_file(&path);
        self.documents.insert(id, OpenDocument::new(doc, is_rust));
        self.by_path.insert(path, id);
        self.schedule_parse(id);
        self.ensure_lsp_synced();
        self.info(id)
    }

    fn document_read(&self, params: DocumentIdParams) -> Result<Value, ServerError> {
        let od = self.open_doc(params.document_id)?;
        to_json(DocumentReadResult {
            document_id: params.document_id,
            revision: od.doc.text().revision(),
            dirty: od.doc.is_dirty(),
            text: od.doc.text().buffer().text(),
        })
    }

    fn edit_result(
        &self,
        document_id: u64,
        before: &TextSnapshot,
        change: Option<&Change>,
    ) -> Result<Value, ServerError> {
        let od = self.open_doc(document_id)?;
        let revision = od.doc.text().revision();
        let result = match change {
            Some(change) => EditResult {
                document_id,
                applied: change.revision != change.previous_revision,
                previous_revision: change.previous_revision,
                revision: change.revision,
                dirty: od.doc.is_dirty(),
                changes: wire_changes(self.encoding, before.buffer(), &change.edits)?,
            },
            None => EditResult {
                document_id,
                applied: false,
                previous_revision: revision,
                revision,
                dirty: od.doc.is_dirty(),
                changes: Vec::new(),
            },
        };
        to_json(result)
    }

    fn apply_edits(&mut self, params: ApplyEditsParams) -> Result<Value, ServerError> {
        let encoding = self.encoding;
        let od = self.open_doc_mut(params.document_id)?;
        // Check the revision first: positions are only meaningful against that revision.
        check_revision(&od.doc, params.expected_revision)?;
        let before = od.doc.text().snapshot();
        let edits = params
            .edits
            .into_iter()
            .map(|e| {
                Ok(TextEdit::replace(
                    to_offset(encoding, before.buffer(), e.range.start)?,
                    to_offset(encoding, before.buffer(), e.range.end)?,
                    e.text,
                ))
            })
            .collect::<Result<Vec<_>, TextError>>()?;
        let change = od.doc.text_mut().apply(params.expected_revision, edits)?;
        self.after_change(params.document_id, &before, &change);
        self.edit_result(params.document_id, &before, Some(&change))
    }

    fn undo_redo(&mut self, params: RevisionParams, redo: bool) -> Result<Value, ServerError> {
        let od = self.open_doc_mut(params.document_id)?;
        let before = od.doc.text().snapshot();
        let change = if redo {
            od.doc.text_mut().redo(params.expected_revision)?
        } else {
            od.doc.text_mut().undo(params.expected_revision)?
        };
        if let Some(change) = &change {
            self.after_change(params.document_id, &before, change);
        }
        self.edit_result(params.document_id, &before, change.as_ref())
    }

    /// Everything that follows a committed change: syntax tree edit and reparse, journal
    /// append, change notification.
    fn after_change(&mut self, doc_id: u64, before: &TextSnapshot, change: &Change) {
        if change.revision == change.previous_revision {
            return;
        }
        let store = self.journals.clone();
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        if let Some(tree) = od.syntax.tree.as_mut() {
            for edit in input_edits(before.buffer(), &change.edits) {
                tree.edit(&edit);
            }
        }
        if let Some(task) = od.syntax.in_flight {
            // Its result would describe an outdated revision.
            self.pool.cancel(task);
        }
        od.syntax.needs_parse = od.is_rust;
        let dirty = od.doc.is_dirty();
        let mut degraded = None;
        if let Some(store) = store.as_ref() {
            if dirty {
                degraded = journal_append(od, store, before, change, &mut self.counters);
            } else {
                // Back at the disk version (undo to saved state, reload): nothing to protect.
                od.drop_journals();
            }
        }
        if let Some(reason) = degraded {
            self.push_notification(
                notifications::RECOVERY_DEGRADED,
                RecoveryDegradedParams {
                    document_id: doc_id,
                    reason,
                    retrying: true,
                },
            );
        }
        if self.caps.change_notifications {
            if let Ok(changes) = wire_changes(self.encoding, before.buffer(), &change.edits) {
                let params = DidChangeParams {
                    document_id: doc_id,
                    kind: kind_name(change.kind).to_string(),
                    previous_revision: change.previous_revision,
                    revision: change.revision,
                    dirty,
                    changes,
                };
                self.push_notification(notifications::DOCUMENT_DID_CHANGE, params);
            }
        }
        self.lsp_did_change(doc_id, before, change);
        self.schedule_parse(doc_id);
    }

    /// Saves run on the loop: they must be ordered with edits (the revision is checked),
    /// and the conflict check must see the same state the user saw. Cost: one streaming
    /// read + hash of the old file and one write of the new one.
    fn save(&mut self, params: SaveParams) -> Result<Value, ServerError> {
        let od = self.open_doc_mut(params.document_id)?;
        let result = od.doc.save(params.expected_revision, params.force);
        if let Err(DocError::ExternalModification { .. }) = &result {
            self.counters.disk_conflicts += 1;
            // Let the reconciliation record what is on disk now.
            self.schedule_disk_check(params.document_id, "save");
        }
        let outcome = result?;
        let od = self.open_doc_mut(params.document_id)?;
        od.disk_epoch += 1;
        od.disk_status = DiskStatus::InSync;
        // The unsaved work is now on disk; its journal is no longer needed.
        od.drop_journals();
        let displaced = od.doc.take_displaced();
        let path = od.doc.path().to_path_buf();
        let od = self.open_doc_mut(params.document_id)?;
        let result = to_json(SaveResult {
            document_id: params.document_id,
            revision: outcome.revision,
            bytes_written: outcome.bytes_written,
            dirty: od.doc.is_dirty(),
            strategy: outcome.strategy.as_str().to_string(),
        });
        self.lsp_did_save(params.document_id);
        if let Some(file) = displaced {
            self.watch_displaced(params.document_id, &path, file);
        }
        result
    }

    fn document_close(&mut self, params: DocumentCloseParams) -> Result<Value, ServerError> {
        let od = self.open_doc(params.document_id)?;
        if od.doc.is_dirty() && !params.discard {
            return Err(rpc_error(
                codes::UNSAVED_CHANGES,
                format!("document {} has unsaved changes", params.document_id),
            ));
        }
        self.lsp_did_close(params.document_id);
        let Some(mut od) = self.documents.remove(&params.document_id) else {
            return Ok(Value::Null);
        };
        let path = od.doc.path().to_path_buf();
        self.by_path.remove(&path);
        if let Some(task) = od.syntax.in_flight.take() {
            self.pool.cancel(task);
        }
        if let Some(task) = od.disk_check.take() {
            self.pool.cancel(task);
        }
        for id in std::mem::take(&mut od.disk_waiters) {
            self.push_response(Response::failure(
                id,
                RpcError::new(codes::DOCUMENT_NOT_FOUND, "the document was closed"),
            ));
        }
        od.drop_journals();
        // The index entry described the buffer; re-index the file as it is on disk.
        if od.is_rust {
            self.reindex_from_disk(path);
            self.mark_graph_stale();
        }
        Ok(Value::Null)
    }

    // ----- index --------------------------------------------------------------------

    fn schedule_parse(&mut self, doc_id: u64) {
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        if !od.is_rust || !od.syntax.needs_parse || od.syntax.in_flight.is_some() {
            return;
        }
        let snapshot = od.doc.text().snapshot();
        let revision = snapshot.revision();
        let old_tree = od.syntax.tree.clone();
        let incremental = old_tree.is_some();
        let submitted = self
            .pool
            .submit("parse", Priority::High, None, move |token| {
                let tree = with_parser(|parser| {
                    parser
                        .parse(snapshot.buffer(), old_tree.as_ref(), &|| {
                            token.is_cancelled()
                        })
                        .ok()
                        .flatten()
                });
                let parsed = tree
                    .as_ref()
                    .map(|t| analyze(t, snapshot.buffer(), MAX_DIAGNOSTICS));
                let facts = tree
                    .as_ref()
                    .map(|t| Box::new(titan_index::graph::facts_from_tree(t, snapshot.buffer())));
                TaskResult::Parse {
                    tree,
                    parsed,
                    incremental,
                    facts,
                }
            });
        if let Ok((task, _)) = submitted {
            od.syntax.in_flight = Some(task);
            od.syntax.needs_parse = false;
            self.pending
                .insert(task, Pending::Parse { doc_id, revision });
        }
    }

    /// Publish a parse result only if it describes the document's current revision.
    fn on_parse_done(&mut self, doc_id: u64, revision: u64, outcome: TaskOutcome<TaskResult>) {
        let encoding = self.encoding;
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        od.syntax.in_flight = None;
        let current = od.doc.text().revision();
        let mut diagnostics = None;
        let mut graph_update = None;
        match outcome {
            TaskOutcome::Completed(TaskResult::Parse {
                tree: Some(tree),
                parsed: Some(parsed),
                incremental,
                facts,
            }) if current == revision => {
                od.syntax.tree = Some(tree);
                if incremental {
                    self.counters.incremental_parses += 1;
                } else {
                    self.counters.full_parses += 1;
                }
                let path = od.doc.path().to_path_buf();
                diagnostics = Some(SyntaxDiagnosticsParams {
                    document_id: doc_id,
                    revision,
                    evidence: EVIDENCE.to_string(),
                    diagnostics: parsed
                        .diagnostics
                        .iter()
                        .map(|d| messages::Diagnostic {
                            range: Range {
                                start: line_col_position(encoding, d.start),
                                end: line_col_position(encoding, d.end),
                            },
                            severity: "error".to_string(),
                            message: d.message.clone(),
                        })
                        .collect(),
                });
                if let Some(facts) = facts {
                    graph_update = Some((path.clone(), revision, facts));
                }
                if let Some(d) = &diagnostics {
                    od.syntax.diagnostics = Some((revision, d.diagnostics.clone()));
                }
                self.index.publish(
                    &path,
                    FileIndex {
                        version: SourceVersion::Buffer { revision },
                        symbols: parsed.symbols,
                        has_syntax_errors: parsed.has_syntax_errors,
                    },
                );
            }
            TaskOutcome::Completed(TaskResult::Parse { tree: Some(_), .. }) => {
                // The document moved on while this parse ran: never publish it.
                self.counters.stale_results_dropped += 1;
                od.syntax.needs_parse = true;
            }
            _ => {
                if current != revision {
                    od.syntax.needs_parse = true;
                }
            }
        }
        if let Some(params) = diagnostics.filter(|_| self.caps.syntax_diagnostics) {
            self.push_notification(notifications::SYNTAX_DIAGNOSTICS, params);
        }
        if let Some((path, revision, facts)) = graph_update {
            self.graph_file_parsed(&path, SourceVersion::Buffer { revision }, *facts);
        }
        self.schedule_parse(doc_id);
    }

    fn start_scan(&mut self) {
        let Some(workspace) = self.workspace.clone() else {
            return;
        };
        if let Some(task) = self.scanning.take() {
            self.pool.cancel(task);
        }
        self.scan_epoch += 1;
        let epoch = self.scan_epoch;
        let known: HashMap<PathBuf, SourceVersion> = self
            .index
            .versions()
            .map(|(path, version)| (path.to_path_buf(), version))
            .collect();
        let events = self.events.clone();
        let submitted = self
            .pool
            .submit("index.scan", Priority::Low, None, move |token| {
                let progress = |done: usize, total: usize| {
                    let _ = events.send(Event::ScanProgress { epoch, done, total });
                };
                TaskResult::Scan(scan_workspace(&workspace, &known, token, &progress))
            });
        match submitted {
            Ok((task, _)) => {
                self.scanning = Some(task);
                self.scan_progress = (0, 0);
                self.pending.insert(task, Pending::Scan { epoch });
            }
            Err(e) => eprintln!("titand: could not start the workspace scan: {e}"),
        }
    }

    fn on_scan_done(&mut self, epoch: u64, outcome: TaskOutcome<TaskResult>) {
        if epoch != self.scan_epoch {
            return;
        }
        self.scanning = None;
        let TaskOutcome::Completed(TaskResult::Scan(out)) = outcome else {
            return;
        };
        let mut counts = ScanCounts {
            skipped: out.skipped,
            ..ScanCounts::default()
        };
        for (path, entry) in out.files {
            // Open documents are indexed from their buffers, which win over disk.
            if self.by_path.contains_key(&path) {
                continue;
            }
            match entry {
                Some(entry) => {
                    self.index.publish(&path, entry);
                    counts.reindexed += 1;
                }
                None => counts.unchanged += 1,
            }
        }
        let mut keep = out.keep;
        keep.extend(self.by_path.keys().cloned());
        counts.removed = self.index.retain_only(&keep);
        self.last_scan = counts;
        self.mark_graph_stale();
    }

    fn status(&self) -> IndexStatusResult {
        let stats = self.index.stats();
        IndexStatusResult {
            evidence: EVIDENCE.to_string(),
            generation: stats.generation,
            idle: self.index_idle(),
            scanning: self.scanning.is_some(),
            pending_parses: self.pending_parses(),
            files_indexed: stats.files,
            symbols: stats.symbols,
            files_with_syntax_errors: stats.files_with_syntax_errors,
            reindexed: self.last_scan.reindexed,
            unchanged: self.last_scan.unchanged,
            removed: self.last_scan.removed,
            skipped: self.last_scan.skipped,
            stale_results_dropped: self.counters.stale_results_dropped,
            incremental_parses: self.counters.incremental_parses,
            full_parses: self.counters.full_parses,
        }
    }

    fn index_status(&mut self, params: IndexStatusParams, id: Option<Value>) -> Reply {
        if let Err(e) = self.workspace() {
            return Reply::Now(Err(e));
        }
        match id {
            Some(id) if params.wait_for_idle && !self.index_idle() => {
                let ms = params
                    .timeout_ms
                    .unwrap_or(DEFAULT_WAIT_MS)
                    .min(MAX_WAIT_MS);
                self.waiters.push(Waiter {
                    key: request_key(&id),
                    id,
                    deadline: Instant::now() + Duration::from_millis(ms),
                });
                Reply::Deferred
            }
            _ => Reply::Now(to_json(self.status())),
        }
    }

    fn index_rescan(&mut self) -> Result<Value, ServerError> {
        self.workspace()?;
        self.start_scan();
        Ok(json!({ "started": true }))
    }

    fn index_symbols(&self, params: SymbolsParams) -> Result<Value, ServerError> {
        if params.query.is_empty() {
            return Err(rpc_error(codes::INVALID_PARAMS, "query must not be empty"));
        }
        let workspace = self.workspace()?;
        let limit = params
            .max_results
            .unwrap_or(DEFAULT_SYMBOL_RESULTS)
            .min(MAX_SYMBOL_RESULTS);
        let encoding = self.encoding;
        let matches = self
            .index
            .search(&params.query, limit)
            .into_iter()
            .map(|m| {
                let (source, revision) = match m.version {
                    SourceVersion::Buffer { revision } => (MatchSourceKind::Buffer, Some(revision)),
                    SourceVersion::Disk { .. } => (MatchSourceKind::Disk, None),
                };
                SymbolInfo {
                    name: m.symbol.name.clone(),
                    kind: m.symbol.kind.as_str().to_string(),
                    path: workspace
                        .relative(m.path)
                        .map(|p| display_path(&p))
                        .unwrap_or_else(|| display_path(m.path)),
                    range: Range {
                        start: line_col_position(encoding, m.symbol.name_start),
                        end: line_col_position(encoding, m.symbol.name_end),
                    },
                    container_name: m.container.map(|c| c.name.clone()),
                    source,
                    revision,
                }
            })
            .collect();
        to_json(SymbolsResult {
            evidence: EVIDENCE.to_string(),
            generation: self.index.generation(),
            indexing: !self.index_idle(),
            matches,
        })
    }

    // ----- recovery -----------------------------------------------------------------

    fn recovery_list(&mut self, id: Option<Value>) -> Reply {
        let Some(store) = self.journals.clone() else {
            return Reply::Now(to_json(RecoveryListResult {
                enabled: false,
                journals: Vec::new(),
                removed_expired: 0,
                removed_over_budget: 0,
            }));
        };
        let active = self.active_journals();
        let root = self.workspace.as_ref().map(|ws| ws.root().to_path_buf());
        self.submit_request(id, "recovery.list", Priority::Low, move |_| {
            let cleanup = store
                .cleanup(&active, SystemTime::now())
                .unwrap_or_default();
            let reports = store
                .list(&active)
                .map_err(|e| RpcError::new(codes::IO_ERROR, e.to_string()))?;
            let journals = reports
                .into_iter()
                .map(|r| RecoveryJournalInfo {
                    path: r.document_path.as_ref().map(|p| {
                        root.as_ref()
                            .and_then(|root| p.strip_prefix(root).ok())
                            .map_or_else(|| display_path(p), display_path)
                    }),
                    journal_id: r.journal_id,
                    state: r.state.as_str().to_string(),
                    disk: r.disk.as_str().to_string(),
                    recovered_revision: r.recovered_revision,
                    edit_records: r.edit_records,
                    truncated_tail: r.truncated_tail,
                    corruption: r.corruption,
                    reason: r.reason,
                    created_unix_ms: r.created_unix_ms,
                })
                .collect();
            to_json_rpc(RecoveryListResult {
                enabled: true,
                journals,
                removed_expired: cleanup.removed_expired,
                removed_over_budget: cleanup.removed_over_budget,
            })
        })
    }

    /// Open the journal's document and replace its text with the recovered text in one
    /// transaction (undo returns to the disk content). The file itself is not written.
    fn recovery_restore(&mut self, params: RecoveryRestoreParams) -> Result<Value, ServerError> {
        let unavailable = |msg: String| rpc_error(codes::RECOVERY_UNAVAILABLE, msg);
        let store = self
            .journals
            .clone()
            .ok_or_else(|| unavailable("recovery is disabled".to_string()))?;
        let report = store
            .inspect_id(&params.journal_id)
            .ok_or_else(|| unavailable(format!("unknown journal {:?}", params.journal_id)))?;
        let conflict = report.state == RecoveryState::Conflict;
        let partial = report.truncated_tail || report.corruption.is_some();
        let corruption = report.corruption.clone();
        match report.state {
            RecoveryState::Recoverable => {}
            RecoveryState::Conflict if params.accept_conflict => {}
            RecoveryState::Conflict => {
                return Err(RpcError::new(
                    codes::RECOVERY_CONFLICT,
                    report
                        .reason
                        .clone()
                        .unwrap_or_else(|| "the file changed on disk".to_string()),
                )
                .with_data(json!({ "disk": report.disk.as_str() }))
                .into());
            }
            RecoveryState::NoChanges | RecoveryState::Unrecoverable => {
                return Err(unavailable(format!(
                    "journal is {}: {}",
                    report.state.as_str(),
                    report.reason.clone().unwrap_or_default()
                )));
            }
        }
        let text = report
            .recovered_text
            .ok_or_else(|| unavailable("no recovered text".to_string()))?;
        let doc_path = report
            .document_path
            .ok_or_else(|| unavailable("journal has no document path".to_string()))?;
        let path = self.workspace()?.resolve_existing(&doc_path)?;
        if self.by_path.contains_key(&path) {
            return Err(rpc_error(
                codes::INVALID_PARAMS,
                "close the document before restoring it",
            ));
        }
        let info = self.document_open(DocumentOpenParams {
            path: path.to_string_lossy().into_owned(),
        })?;
        let id = info.document_id;
        let od = self.open_doc_mut(id)?;
        let before = od.doc.text().snapshot();
        if before.buffer().text() != text {
            let end = ByteOffset(before.buffer().len_bytes());
            let change = od
                .doc
                .text_mut()
                .apply(0, vec![TextEdit::replace(ByteOffset(0), end, text)])?;
            self.after_change(id, &before, &change);
        }
        if let Err(e) = store.discard(&params.journal_id) {
            eprintln!("titand: could not remove restored journal: {e}");
        }
        to_json(RecoveryRestoreResult {
            document: self.info(id)?,
            recovered_revision: report.recovered_revision.unwrap_or(0),
            conflict,
            partial,
            corruption,
        })
    }

    fn recovery_discard(&mut self, params: JournalIdParams) -> Result<Value, ServerError> {
        let store = self
            .journals
            .clone()
            .ok_or_else(|| rpc_error(codes::RECOVERY_UNAVAILABLE, "recovery is disabled"))?;
        let in_use = self.active_journals().iter().any(|p| {
            p.file_name()
                .is_some_and(|n| n.to_string_lossy() == params.journal_id)
        });
        if in_use {
            return Err(rpc_error(
                codes::INVALID_PARAMS,
                "that journal belongs to an open document",
            ));
        }
        let discarded = store
            .discard(&params.journal_id)
            .map_err(|e| rpc_error(codes::IO_ERROR, e.to_string()))?;
        Ok(json!({ "discarded": discarded }))
    }
}

/// Append `change` to the document's journal, creating the journal on the first change.
/// Returns a reason when journaling just failed (the caller notifies the client). After a
/// failure, journaling is retried with a fresh, fully checkpointed journal on the first
/// change after [`JOURNAL_RETRY`].
fn journal_append(
    od: &mut OpenDocument,
    store: &JournalStore,
    before: &TextSnapshot,
    change: &Change,
    counters: &mut Counters,
) -> Option<String> {
    let retrying = od.journal_failed;
    if retrying {
        if od.journal_retry_at.is_some_and(|at| Instant::now() < at) {
            return None;
        }
        counters.journal_retries += 1;
        if let Some(old) = od.journal.take() {
            od.failed_journal.get_or_insert(old);
        }
    }
    let mut fail = |od: &mut OpenDocument, what: &str, e: io::Error| {
        let reason = format!("recovery journal {what} failed: {e}");
        eprintln!("titand: {reason}");
        counters.journal_failures += 1;
        od.journal_failed = true;
        od.journal_retry_at = Some(Instant::now() + JOURNAL_RETRY);
        if let Some(old) = od.journal.take() {
            od.failed_journal.get_or_insert(old);
        }
        // Report the first failure only, not every failed retry.
        (!retrying).then_some(reason)
    };
    if od.journal.is_none() {
        let disk = od.doc.disk_state();
        let base = BaseInfo {
            path: od.doc.path().to_path_buf(),
            disk_len: disk.len,
            disk_hash: disk.hash,
            has_bom: od.doc.has_bom(),
            base_revision: before.revision(),
        };
        // `before` is the disk content unless the buffer already had unsaved changes (a
        // retry, or a buffer kept over a changed/deleted file): then always checkpoint.
        let checkpoint = retrying || od.disk_status != DiskStatus::InSync || !disk.exists;
        let created = if checkpoint {
            JournalWriter::create_checkpointed(store.config(), base, before.buffer())
        } else {
            JournalWriter::create(store.config(), base, before.buffer())
        };
        match created {
            Ok(writer) => od.journal = Some(writer),
            Err(e) => return fail(od, "creation", e),
        }
    }
    let writer = od.journal.as_mut()?;
    if let Err(e) = writer.append(change) {
        return fail(od, "write", e);
    }
    counters.journal_appends += 1;
    if writer.needs_compaction() {
        match writer.compact(change.revision, od.doc.text().buffer()) {
            Ok(()) => counters.journal_compactions += 1,
            Err(e) => {
                eprintln!("titand: recovery journal compaction failed: {e}");
                counters.journal_failures += 1;
            }
        }
    }
    if retrying {
        // A fresh, complete journal exists again; the failed one is superseded.
        od.journal_failed = false;
        od.journal_retry_at = None;
        if let Some(old) = od.failed_journal.take() {
            let _ = old.remove();
        }
    }
    None
}

fn task_outcome_name<T>(outcome: &TaskOutcome<T>) -> &'static str {
    match outcome {
        TaskOutcome::Completed(_) => "completed with an unexpected result",
        TaskOutcome::Cancelled => "cancelled",
        TaskOutcome::TimedOut => "timed out",
        TaskOutcome::Panicked(_) => "panicked",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    fn session() -> (Session, mpsc::Receiver<Event>) {
        let (tx, rx) = mpsc::channel();
        (Session::new(ServerOptions::default(), tx).unwrap(), rx)
    }

    fn call(session: &mut Session, request: Value) -> Value {
        let body = serde_json::to_vec(&request).unwrap();
        session.on_message(&body);
        let out = session.take_outgoing();
        assert_eq!(out.len(), 1, "expected exactly one message");
        serde_json::from_slice(&out[0]).unwrap()
    }

    fn hello(session: &mut Session) {
        let reply = call(
            session,
            json!({"jsonrpc":"2.0","id":0,"method":"session.hello","params":{"protocolVersion":1}}),
        );
        assert!(reply.get("result").is_some(), "{reply}");
    }

    #[test]
    fn requests_before_hello_are_rejected() {
        let (mut session, _rx) = session();
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":1,"method":"system.health"}),
        );
        assert_eq!(reply["error"]["code"], codes::NOT_INITIALIZED);
    }

    #[test]
    fn invalid_envelopes() {
        let (mut session, _rx) = session();
        hello(&mut session);
        let reply = call(
            &mut session,
            json!({"jsonrpc":"1.0","id":1,"method":"system.health"}),
        );
        assert_eq!(reply["error"]["code"], codes::INVALID_REQUEST);
        assert_eq!(reply["id"], 1);
        let reply = call(&mut session, json!({"jsonrpc":"2.0","id":2}));
        assert_eq!(reply["error"]["code"], codes::INVALID_REQUEST);
        let reply = call(&mut session, json!({"jsonrpc":"2.0","id":[1],"method":"x"}));
        assert_eq!(reply["error"]["code"], codes::INVALID_REQUEST);
        let reply = call(&mut session, json!([1, 2]));
        assert_eq!(reply["error"]["code"], codes::INVALID_REQUEST);
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":3,"method":"nope"}),
        );
        assert_eq!(reply["error"]["code"], codes::METHOD_NOT_FOUND);
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":4,"method":"document.read","params":{"documentId":"x"}}),
        );
        assert_eq!(reply["error"]["code"], codes::INVALID_PARAMS);
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":5,"method":"document.read","params":{"documentId":9}}),
        );
        assert_eq!(reply["error"]["code"], codes::DOCUMENT_NOT_FOUND);
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":6,"method":"workspace.listFiles"}),
        );
        assert_eq!(reply["error"]["code"], codes::NO_WORKSPACE);
    }

    #[test]
    fn notifications_and_null_ids() {
        let (mut session, _rx) = session();
        hello(&mut session);
        session.on_message(br#"{"jsonrpc":"2.0","method":"system.health"}"#);
        assert!(session.take_outgoing().is_empty());
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":null,"method":"system.health"}),
        );
        assert!(reply["id"].is_null());
        assert!(reply.get("error").is_none());
    }

    #[test]
    fn unsupported_protocol_version() {
        let (mut session, _rx) = session();
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"protocolVersion":99}}),
        );
        assert_eq!(reply["error"]["code"], codes::UNSUPPORTED_PROTOCOL_VERSION);
        assert_eq!(reply["error"]["data"]["supported"][0], PROTOCOL_VERSION);
    }

    #[test]
    fn oversized_responses_become_errors() {
        let (mut session, _rx) = session();
        hello(&mut session);
        session.options.max_response_bytes = 64;
        let reply = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":7,"method":"system.metrics"}),
        );
        assert_eq!(reply["error"]["code"], codes::RESPONSE_TOO_LARGE);
        assert_eq!(reply["id"], 7);
    }

    /// A parse result computed for an older revision is never published.
    #[test]
    fn stale_parse_results_are_dropped() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("a.rs"), "fn old_name() {}\n").unwrap();
        let (mut session, _rx) = session();
        hello(&mut session);
        call(
            &mut session,
            json!({"jsonrpc":"2.0","id":1,"method":"workspace.open",
                   "params":{"path": dir.path().display().to_string()}}),
        );
        let info = call(
            &mut session,
            json!({"jsonrpc":"2.0","id":2,"method":"document.open","params":{"path":"a.rs"}}),
        );
        let id = info["result"]["documentId"].as_u64().unwrap();
        call(
            &mut session,
            json!({"jsonrpc":"2.0","id":3,"method":"document.applyEdits","params":{
                "documentId": id, "expectedRevision": 0, "edits": [
                {"range":{"start":{"line":0,"character":3},"end":{"line":0,"character":11}},
                 "text":"new_name"}]}}),
        );
        // A background parse of revision 0 finishing after the edit.
        let old = TextBuffer::new("fn old_name() {}\n");
        let tree = SyntaxParser::new()
            .unwrap()
            .parse(&old, None, &|| false)
            .unwrap()
            .unwrap();
        let parsed = analyze(&tree, &old, 10);
        let before = session.counters.stale_results_dropped;
        session.on_parse_done(
            id,
            0,
            TaskOutcome::Completed(TaskResult::Parse {
                tree: Some(tree),
                parsed: Some(parsed),
                incremental: false,
                facts: None,
            }),
        );
        assert_eq!(session.counters.stale_results_dropped, before + 1);
        assert!(session.index.search("old_name", 10).is_empty());
        assert!(
            session.documents[&id].syntax.needs_parse
                || session.documents[&id].syntax.in_flight.is_some()
        );
    }
}
