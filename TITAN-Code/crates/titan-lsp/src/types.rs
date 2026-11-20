//! Public value types: errors, states, events, metrics.

use std::time::Duration;

use serde_json::Value;

/// JSON-RPC / LSP error codes the client produces or that callers commonly test for.
pub mod codes {
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const INTERNAL_ERROR: i64 = -32603;
    /// LSP `RequestCancelled`.
    pub const REQUEST_CANCELLED: i64 = -32800;
    /// LSP `ContentModified`: the document changed while the server computed the answer.
    pub const CONTENT_MODIFIED: i64 = -32801;
}

#[derive(Clone, Debug, PartialEq, thiserror::Error)]
pub enum LspError {
    /// The server is not in [`ServerState::Running`] (starting, restarting, failed, stopped).
    #[error("the language server is not running")]
    NotRunning,
    /// The per-request deadline passed; `$/cancelRequest` was sent.
    #[error("the request timed out")]
    Timeout,
    /// [`crate::Supervisor::cancel`] was called; `$/cancelRequest` was sent.
    #[error("the request was cancelled")]
    Cancelled,
    /// The server process died or broke the protocol before answering.
    #[error("connection to the language server was lost: {0}")]
    ConnectionLost(String),
    /// The server answered with a JSON-RPC error.
    #[error("language server error {code}: {message}")]
    Server {
        code: i64,
        message: String,
        data: Option<Value>,
    },
    /// The outgoing queue of the connection is full (the server is not reading its input).
    #[error("the outgoing message queue to the language server is full")]
    QueueFull,
    /// The supervisor shut down before the request was answered.
    #[error("the language server supervisor was shut down")]
    Stopped,
    /// The message could not be serialized.
    #[error("could not encode the message: {0}")]
    Encode(String),
}

impl LspError {
    /// True for the LSP `ContentModified` error, which callers usually retry.
    pub fn is_content_modified(&self) -> bool {
        matches!(self, Self::Server { code, .. } if *code == codes::CONTENT_MODIFIED)
    }
}

/// Lifecycle state of the supervised server.
#[derive(Clone, Debug, PartialEq)]
pub enum ServerState {
    /// Spawning the process.
    Starting,
    /// `initialize` sent, waiting for its result.
    Initializing,
    /// `initialized` sent; requests and notifications are accepted.
    ///
    /// `quiescent` reflects rust-analyzer's `experimental/serverStatus`: `None` until the
    /// server reports (servers that never send it stay `None`), then `Some(false)` while it
    /// loads or indexes and `Some(true)` when it is ready for semantic requests.
    Running { quiescent: Option<bool> },
    /// The server crashed; the next start happens after `retry_in`. `attempt` is the number
    /// of crashes within the restart window.
    Restarting { attempt: u32, retry_in: Duration },
    /// Graceful shutdown in progress.
    ShuttingDown,
    /// The supervisor gave up (too many crashes, or the program cannot be started).
    Failed { reason: String },
    /// Shut down.
    Stopped,
}

impl ServerState {
    pub fn is_running(&self) -> bool {
        matches!(self, Self::Running { .. })
    }
}

/// Events delivered to the `on_event` callback, in order, from the supervisor's control
/// thread and never while an internal lock is held.
#[derive(Clone, Debug, PartialEq)]
pub enum LspEvent {
    /// The state changed. `generation` increments on every (re)start. A `Running` state with
    /// a new generation means a fresh server process: the caller must re-send `didOpen` for
    /// its open documents.
    State { generation: u64, state: ServerState },
    /// A server notification (`textDocument/publishDiagnostics`, `$/progress`,
    /// `window/logMessage`, `experimental/serverStatus`, ...).
    Notification {
        generation: u64,
        method: String,
        params: Value,
    },
    /// Completion of a request. Every ticket completes exactly once.
    Response {
        ticket: u64,
        generation: u64,
        result: Result<Value, LspError>,
    },
    /// The server process of `generation` died or broke the protocol. Pending requests of
    /// that generation have completed with [`LspError::ConnectionLost`].
    Crashed {
        generation: u64,
        reason: String,
        stderr_tail: String,
    },
}

/// Counters since the supervisor started.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LspMetrics {
    /// Process spawn attempts (successful or not).
    pub starts: u64,
    /// Starts after the first one.
    pub restarts: u64,
    /// Connections lost while not shutting down (process exit, EOF, protocol violation,
    /// initialize failure or timeout).
    pub crashes: u64,
    pub spawn_failures: u64,
    pub requests_sent: u64,
    /// Requests answered by the server (result or JSON-RPC error).
    pub requests_completed: u64,
    pub requests_timed_out: u64,
    pub requests_cancelled: u64,
    /// Requests failed by a lost connection or by shutdown.
    pub requests_failed: u64,
    pub notifications_in: u64,
    pub notifications_out: u64,
    /// Server-to-client requests answered.
    pub server_requests: u64,
    /// Responses whose id the client never issued.
    pub unknown_responses: u64,
    /// Responses to requests that had already timed out or been cancelled (dropped).
    pub late_responses: u64,
    /// Internal messages dropped because the outgoing queue was full.
    pub dropped_outgoing: u64,
    /// Bytes of message bodies read / written.
    pub bytes_in: u64,
    pub bytes_out: u64,
    /// Processes killed because they did not exit after `shutdown` + `exit`.
    pub forced_kills: u64,
    /// Panics raised by the `on_event` callback (caught; the supervisor keeps running).
    pub callback_panics: u64,
    pub last_error: Option<String>,
}

/// What [`crate::Supervisor::shutdown`] observed.
#[derive(Clone, Debug, PartialEq)]
pub struct ShutdownReport {
    /// The server answered `shutdown` before the deadline.
    pub shutdown_acknowledged: bool,
    /// The process had to be killed (it did not exit within `shutdown_timeout`, or there was
    /// no orderly path to stop it).
    pub forced_kill: bool,
    pub metrics: LspMetrics,
}
