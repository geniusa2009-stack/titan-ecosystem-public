//! TITAN Code language-server client: one supervised LSP 3.17 server process.
//!
//! The editor core must keep working when a language server is slow, wrong, or dead. This
//! crate therefore treats the server as an untrusted, crash-prone child process and never
//! lets it block the caller: every method of [`Supervisor`] returns immediately and all
//! results arrive as [`LspEvent`]s through a callback.
//!
//! # Threads
//!
//! * **Control thread** (one per [`Supervisor`]): owns the lifecycle, the restart policy and
//!   all timers (request deadlines, `initialize` deadline, restart backoff, shutdown
//!   deadline). It is the only thread that emits events, so events arrive in one total
//!   order, and it never invokes the callback while holding an internal lock.
//! * **Per process generation**: a *reader* thread (`stdout` -> Content-Length frames via
//!   [`titan_protocol::framing`] -> JSON), a *writer* thread (bounded queue -> one framed
//!   message per write -> `stdin`) and a *stderr* thread that drains `stderr` into a ring
//!   buffer of the last [`SupervisorConfig::stderr_tail_bytes`] bytes for crash reports.
//!
//! Callers (`request`, `notify`, `cancel`) only take a short lock and push to the writer
//! queue with `try_send`; a full queue returns [`LspError::QueueFull`] instead of blocking.
//!
//! # Lifecycle
//!
//! `Starting` (spawn; program + args, never a shell) -> `Initializing` (`initialize` with
//! `processId`, `rootUri`, `workspaceFolders`, `clientInfo {name: "titand"}` and the
//! capabilities in [`client_capabilities`], answered within
//! [`SupervisorConfig::init_timeout`]) -> `initialized` notification -> `Running`.
//! The negotiated `positionEncoding` is recorded (default `utf-16`, the only one offered).
//!
//! rust-analyzer's `experimental/serverStatus` notification (enabled through the
//! `experimental.serverStatusNotification` client capability) is reflected in
//! [`ServerState::Running`]`{ quiescent }`: `None` = the server never said, `Some(false)` =
//! loading/indexing, `Some(true)` = ready for semantic requests.
//!
//! Server-to-client requests are always answered so the server cannot stall on us:
//! `workspace/configuration` from [`SupervisorConfig::workspace_configuration`],
//! `client/registerCapability`, `client/unregisterCapability`,
//! `window/workDoneProgress/create` (and the `*/refresh` requests) with `null`,
//! `workspace/workspaceFolders` with the root folder, anything else with `-32601`.
//!
//! # Requests and timeouts
//!
//! A request returns a ticket (its JSON-RPC id). Every ticket completes **exactly once**:
//! with the server's answer (result or [`LspError::Server`]), [`LspError::Timeout`] (the
//! control thread enforces the deadline and sends `$/cancelRequest`),
//! [`LspError::Cancelled`] ([`Supervisor::cancel`], also sends `$/cancelRequest`),
//! [`LspError::ConnectionLost`] (the process died) or [`LspError::Stopped`] (shutdown).
//! Responses may arrive in any order and are matched by id; answers that arrive after a
//! timeout or cancellation are dropped and counted, unknown ids are counted and ignored.
//!
//! # Crashes and restarts
//!
//! Process exit, EOF on `stdout`, a framing error, a message above
//! [`SupervisorConfig::max_message_bytes`], malformed JSON or a non-JSON-RPC message, an
//! `initialize` error or timeout, all while not shutting down, end the connection: pending
//! requests fail with `ConnectionLost`, a [`LspEvent::Crashed`] carries the reason, the exit
//! status and the stderr tail, and the server is restarted after
//! `backoff_initial * 2^(n-1)` (capped at `backoff_max`), where `n` is the number of crashes
//! inside [`SupervisorConfig::restart_window`]. More than
//! [`SupervisorConfig::max_restarts`] crashes inside the window -> [`ServerState::Failed`]
//! and no further attempts. A program that cannot be executed at all (not found, permission
//! denied) fails immediately without retries.
//!
//! **Every restart increments the generation.** The new server knows nothing about the
//! caller's documents: on `State { generation: new, state: Running { .. } }` the caller must
//! re-send `didOpen` for its open buffers. [`Supervisor::notify`] returns the generation a
//! notification went to, and every response event names its generation.
//!
//! # Shutdown
//!
//! [`Supervisor::shutdown`] sends `shutdown`, then `exit`, closes `stdin`, waits up to
//! [`SupervisorConfig::shutdown_timeout`] in total, kills the process if it is still alive,
//! and reaps it before returning. Dropping a [`Supervisor`] kills the process immediately.
//!
//! # Limits
//!
//! Message bodies are capped at `max_message_bytes` (default 64 MiB), header lines at
//! [`titan_protocol::framing::MAX_HEADER_LINE`], the outgoing queue at
//! [`SupervisorConfig::outgoing_queue`] messages, the stderr tail at
//! [`SupervisorConfig::stderr_tail_bytes`].
//!
//! # The fake server
//!
//! `src/bin/titan-fake-lsp.rs` is a small, deterministic language server used only by the
//! failure-injection tests (crash on a method, hang, delay, reverse response order, garbage
//! or oversize output, slow or ignored shutdown). It implements just enough LSP to exercise
//! the client and never pretends to analyze code: its answers echo the request so tests can
//! check ids and ordering. Real behaviour is tested against rust-analyzer in
//! `tests/rust_analyzer.rs`.

mod config;
mod message;
mod supervisor;
mod transport;
mod types;
mod uri;

pub use config::{LspCommand, SupervisorConfig};
pub use message::client_capabilities;
pub use supervisor::{EventCallback, Supervisor};
pub use types::{LspError, LspEvent, LspMetrics, ServerState, ShutdownReport, codes};
pub use uri::path_to_uri;
