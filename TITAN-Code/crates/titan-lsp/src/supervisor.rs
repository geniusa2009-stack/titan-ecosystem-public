//! The supervisor: lifecycle, request bookkeeping, restart policy.

use std::collections::{HashMap, VecDeque};
use std::panic::{self, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use crate::config::SupervisorConfig;
use crate::message::{self, Incoming};
use crate::transport::{ConnEvent, Connection, lock};
use crate::types::{LspError, LspEvent, LspMetrics, ServerState, ShutdownReport};

/// Callback receiving every [`LspEvent`]. It runs on the supervisor's control thread, never
/// while an internal lock is held, so it may call back into the [`Supervisor`] (for example
/// to re-send `didOpen` after a restart). It should return quickly (forward the event into
/// the caller's own loop): while it runs, the supervisor processes nothing else. A panic in
/// the callback is caught and counted in [`LspMetrics::callback_panics`].
pub type EventCallback = Box<dyn Fn(LspEvent) + Send + 'static>;

/// Messages to the control thread.
#[derive(Debug)]
pub(crate) enum Msg {
    Conn(ConnEvent),
    /// Re-evaluate deadlines (a request with a new deadline was registered).
    Wake,
    /// Deliver an event produced on a caller thread (cancellation), in order.
    Emit(LspEvent),
    Stop {
        graceful: bool,
    },
}

#[derive(Debug)]
struct Pending {
    generation: u64,
    deadline: Instant,
}

struct Inner {
    state: ServerState,
    generation: u64,
    capabilities: Option<Value>,
    server_info: Option<Value>,
    position_encoding: String,
    pid: Option<u32>,
    /// Outgoing queue of the current connection; `Some` only while `Running`.
    writer: Option<SyncSender<Vec<u8>>>,
    /// User requests awaiting an answer, by ticket (= JSON-RPC id).
    pending: HashMap<u64, Pending>,
    next_id: u64,
    metrics: LspMetrics,
}

impl Inner {
    fn allocate_id(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    /// Queues `value` on `writer`, counting bytes or the drop.
    fn send_counted(&mut self, writer: &SyncSender<Vec<u8>>, value: &Value) -> bool {
        let Ok(body) = message::encode(value) else {
            return false;
        };
        let len = body.len() as u64;
        match writer.try_send(body) {
            Ok(()) => {
                self.metrics.bytes_out += len;
                true
            }
            Err(TrySendError::Full(_)) => {
                self.metrics.dropped_outgoing += 1;
                false
            }
            Err(TrySendError::Disconnected(_)) => false,
        }
    }
}

struct Shared {
    inner: Mutex<Inner>,
}

fn deadline_after(timeout: Duration) -> Instant {
    let now = Instant::now();
    now.checked_add(timeout)
        .or_else(|| now.checked_add(Duration::from_secs(86_400 * 365)))
        .unwrap_or(now)
}

/// A supervised language-server process. See the crate documentation for the lifecycle.
///
/// All methods return immediately; results arrive as [`LspEvent`]s. Dropping the supervisor
/// kills the server without the `shutdown` handshake; use [`Supervisor::shutdown`] for the
/// graceful path.
pub struct Supervisor {
    shared: Arc<Shared>,
    control: Sender<Msg>,
    thread: Option<JoinHandle<ShutdownReport>>,
    request_timeout: Duration,
}

impl Supervisor {
    /// Starts the supervisor thread, which spawns and initializes the server. Returns
    /// immediately; progress is reported through `on_event`.
    pub fn start(config: SupervisorConfig, on_event: EventCallback) -> Self {
        let shared = Arc::new(Shared {
            inner: Mutex::new(Inner {
                state: ServerState::Starting,
                generation: 0,
                capabilities: None,
                server_info: None,
                position_encoding: "utf-16".to_string(),
                pid: None,
                writer: None,
                pending: HashMap::new(),
                next_id: 1,
                metrics: LspMetrics::default(),
            }),
        });
        let (tx, rx) = mpsc::channel();
        let request_timeout = config.request_timeout;
        let root = std::path::absolute(&config.root).unwrap_or_else(|_| config.root.clone());
        let control = Control {
            shared: Arc::clone(&shared),
            folders: message::workspace_folders(&root),
            root,
            config,
            rx,
            tx: tx.clone(),
            on_event,
            phase: Phase::Idle,
            generation: 0,
            quiescent: None,
            crash_times: VecDeque::new(),
            events: Vec::new(),
            shutdown_acknowledged: false,
            report: None,
            next_liveness_check: Instant::now(),
            exit_noticed: None,
        };
        let thread = match thread::Builder::new()
            .name("lsp-supervisor".to_string())
            .spawn(move || control.run())
        {
            Ok(handle) => Some(handle),
            Err(e) => {
                let mut inner = lock(&shared.inner);
                let reason = format!("cannot start the supervisor thread: {e}");
                inner.metrics.last_error = Some(reason.clone());
                inner.state = ServerState::Failed { reason };
                None
            }
        };
        Self {
            shared,
            control: tx,
            thread,
            request_timeout,
        }
    }

    pub fn state(&self) -> ServerState {
        lock(&self.shared.inner).state.clone()
    }

    /// Generation of the current (or last) server process; 1 for the first, +1 per restart.
    pub fn generation(&self) -> u64 {
        lock(&self.shared.inner).generation
    }

    /// `capabilities` from the `InitializeResult` of the current generation (`None` until
    /// it is initialized, and again while restarting).
    pub fn server_capabilities(&self) -> Option<Value> {
        lock(&self.shared.inner).capabilities.clone()
    }

    /// `serverInfo` from the `InitializeResult` of the current generation, if the server sent it.
    pub fn server_info(&self) -> Option<Value> {
        lock(&self.shared.inner).server_info.clone()
    }

    /// Negotiated position encoding (`positionEncoding` of the server, default `utf-16`).
    pub fn position_encoding(&self) -> String {
        lock(&self.shared.inner).position_encoding.clone()
    }

    /// Process id of the current server process, if one is alive.
    pub fn pid(&self) -> Option<u32> {
        lock(&self.shared.inner).pid
    }

    pub fn metrics(&self) -> LspMetrics {
        lock(&self.shared.inner).metrics.clone()
    }

    /// Sends a notification. Returns the generation it was sent to.
    pub fn notify(&self, method: &str, params: Value) -> Result<u64, LspError> {
        let mut inner = lock(&self.shared.inner);
        let writer = Self::running_writer(&inner)?;
        let body = message::encode(&message::notification(method, params))?;
        let len = body.len() as u64;
        Self::queue(&writer, body)?;
        inner.metrics.notifications_out += 1;
        inner.metrics.bytes_out += len;
        Ok(inner.generation)
    }

    /// Sends a request and returns its ticket. The ticket completes exactly once with an
    /// [`LspEvent::Response`]: the server's answer, [`LspError::Timeout`] after `timeout`
    /// (default [`SupervisorConfig::request_timeout`]), [`LspError::Cancelled`],
    /// [`LspError::ConnectionLost`] or [`LspError::Stopped`].
    pub fn request(
        &self,
        method: &str,
        params: Value,
        timeout: Option<Duration>,
    ) -> Result<u64, LspError> {
        let deadline = deadline_after(timeout.unwrap_or(self.request_timeout));
        let mut inner = lock(&self.shared.inner);
        let writer = Self::running_writer(&inner)?;
        let id = inner.next_id;
        let body = message::encode(&message::request(id, method, params))?;
        let len = body.len() as u64;
        Self::queue(&writer, body)?;
        inner.next_id += 1;
        let generation = inner.generation;
        inner.pending.insert(
            id,
            Pending {
                generation,
                deadline,
            },
        );
        inner.metrics.requests_sent += 1;
        inner.metrics.bytes_out += len;
        drop(inner);
        let _ = self.control.send(Msg::Wake);
        Ok(id)
    }

    /// Cancels a pending request: sends `$/cancelRequest` and completes the ticket with
    /// [`LspError::Cancelled`]; a later answer from the server is dropped (counted in
    /// [`LspMetrics::late_responses`]). Returns false if the ticket was not pending (it has
    /// already completed or never existed).
    pub fn cancel(&self, ticket: u64) -> bool {
        let mut inner = lock(&self.shared.inner);
        let Some(pending) = inner.pending.remove(&ticket) else {
            return false;
        };
        inner.metrics.requests_cancelled += 1;
        if let Some(writer) = inner.writer.clone() {
            let cancel = message::notification("$/cancelRequest", json!({ "id": ticket }));
            inner.send_counted(&writer, &cancel);
        }
        drop(inner);
        let _ = self.control.send(Msg::Emit(LspEvent::Response {
            ticket,
            generation: pending.generation,
            result: Err(LspError::Cancelled),
        }));
        true
    }

    /// Graceful stop: `shutdown` request, then `exit` notification, then up to
    /// [`SupervisorConfig::shutdown_timeout`] (in total) for the process to exit before it
    /// is killed. Pending requests that are still unanswered complete with
    /// [`LspError::Stopped`]. When this returns, the server process has been reaped.
    ///
    /// Must not be called from the event callback (the control thread cannot wait for
    /// itself); there it degrades to an immediate kill.
    pub fn shutdown(mut self) -> ShutdownReport {
        let Some(handle) = self.thread.take() else {
            return self.report_without_thread();
        };
        if handle.thread().id() == thread::current().id() {
            let _ = self.control.send(Msg::Stop { graceful: false });
            return self.report_without_thread();
        }
        let _ = self.control.send(Msg::Stop { graceful: true });
        match handle.join() {
            Ok(report) => report,
            Err(_) => self.report_without_thread(),
        }
    }

    fn report_without_thread(&self) -> ShutdownReport {
        ShutdownReport {
            shutdown_acknowledged: false,
            forced_kill: false,
            metrics: self.metrics(),
        }
    }

    fn running_writer(inner: &Inner) -> Result<SyncSender<Vec<u8>>, LspError> {
        if !inner.state.is_running() {
            return Err(LspError::NotRunning);
        }
        inner.writer.clone().ok_or(LspError::NotRunning)
    }

    fn queue(writer: &SyncSender<Vec<u8>>, body: Vec<u8>) -> Result<(), LspError> {
        writer.try_send(body).map_err(|e| match e {
            TrySendError::Full(_) => LspError::QueueFull,
            TrySendError::Disconnected(_) => {
                LspError::ConnectionLost("the writer thread has stopped".to_string())
            }
        })
    }
}

impl Drop for Supervisor {
    fn drop(&mut self) {
        if let Some(handle) = self.thread.take() {
            let _ = self.control.send(Msg::Stop { graceful: false });
            if handle.thread().id() != thread::current().id() {
                let _ = handle.join();
            }
        }
    }
}

enum Phase {
    /// No process and none scheduled (before the first start, or after `Failed`).
    Idle,
    Backoff {
        until: Instant,
    },
    Initializing {
        conn: Connection,
        init_id: u64,
        deadline: Instant,
    },
    Running {
        conn: Connection,
    },
    ShuttingDown {
        conn: Connection,
        shutdown_id: u64,
        deadline: Instant,
    },
    Done,
}

enum TimerAction {
    InitTimeout,
    Restart,
    ShutdownTimeout,
}

struct Control {
    shared: Arc<Shared>,
    config: SupervisorConfig,
    root: PathBuf,
    folders: Value,
    rx: Receiver<Msg>,
    tx: Sender<Msg>,
    on_event: EventCallback,
    phase: Phase,
    /// Generation of the connection in `phase` (mirrors `Inner::generation`).
    generation: u64,
    quiescent: Option<bool>,
    crash_times: VecDeque<Instant>,
    events: Vec<LspEvent>,
    shutdown_acknowledged: bool,
    report: Option<ShutdownReport>,
    /// Next time to check that the process is still alive (EOF normally reports its death;
    /// polling covers a process whose `stdout` is kept open by a child it spawned).
    next_liveness_check: Instant,
    /// When polling first saw the process gone; the reader gets [`EXIT_GRACE`] to deliver
    /// what is left in the pipe before the connection is declared lost.
    exit_noticed: Option<Instant>,
}

const LIVENESS_INTERVAL: Duration = Duration::from_millis(250);
const EXIT_GRACE: Duration = Duration::from_secs(1);

impl Control {
    fn run(mut self) -> ShutdownReport {
        self.start_generation();
        loop {
            self.flush_events();
            if let Some(report) = self.report.take() {
                return report;
            }
            let msg = match self.next_wakeup() {
                Some(at) => match self
                    .rx
                    .recv_timeout(at.saturating_duration_since(Instant::now()))
                {
                    Ok(msg) => Some(msg),
                    Err(RecvTimeoutError::Timeout) => None,
                    Err(RecvTimeoutError::Disconnected) => Some(Msg::Stop { graceful: false }),
                },
                None => Some(self.rx.recv().unwrap_or(Msg::Stop { graceful: false })),
            };
            if let Some(msg) = msg {
                self.handle(msg);
            }
            self.handle_timers(Instant::now());
        }
    }

    fn flush_events(&mut self) {
        for event in std::mem::take(&mut self.events) {
            let on_event = &self.on_event;
            if panic::catch_unwind(AssertUnwindSafe(|| on_event(event))).is_err() {
                lock(&self.shared.inner).metrics.callback_panics += 1;
            }
        }
    }

    fn next_wakeup(&self) -> Option<Instant> {
        let phase_deadline = match &self.phase {
            Phase::Backoff { until } => Some(*until),
            Phase::Initializing { deadline, .. } => Some((*deadline).min(self.next_liveness_check)),
            Phase::ShuttingDown { deadline, .. } => Some(*deadline),
            Phase::Running { .. } => Some(self.next_liveness_check),
            Phase::Idle | Phase::Done => None,
        };
        let pending_deadline = lock(&self.shared.inner)
            .pending
            .values()
            .map(|p| p.deadline)
            .min();
        match (phase_deadline, pending_deadline) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        }
    }

    fn conn(&self) -> Option<&Connection> {
        match &self.phase {
            Phase::Initializing { conn, .. }
            | Phase::Running { conn }
            | Phase::ShuttingDown { conn, .. } => Some(conn),
            Phase::Idle | Phase::Backoff { .. } | Phase::Done => None,
        }
    }

    /// Queues an internal message on the current connection (if any).
    fn send(&self, value: &Value) {
        if let Some(writer) = self.conn().and_then(Connection::sender) {
            lock(&self.shared.inner).send_counted(&writer, value);
        }
    }

    fn set_state(&mut self, state: ServerState) {
        lock(&self.shared.inner).state = state.clone();
        self.events.push(LspEvent::State {
            generation: self.generation,
            state,
        });
    }

    fn handle(&mut self, msg: Msg) {
        match msg {
            Msg::Wake => {}
            Msg::Emit(event) => self.events.push(event),
            Msg::Stop { graceful } => self.stop(graceful),
            Msg::Conn(ConnEvent::Incoming {
                generation,
                message,
                bytes,
            }) => {
                if generation == self.generation && self.conn().is_some() {
                    lock(&self.shared.inner).metrics.bytes_in += bytes as u64;
                    self.incoming(message);
                }
            }
            Msg::Conn(ConnEvent::Lost { generation, reason }) => {
                if generation == self.generation && self.conn().is_some() {
                    if matches!(self.phase, Phase::ShuttingDown { .. }) {
                        self.finish_shutdown();
                    } else {
                        self.connection_failed(reason);
                    }
                }
            }
        }
    }

    fn incoming(&mut self, message: Value) {
        match message::classify(message) {
            Err(reason) => self.connection_failed(format!("protocol violation: {reason}")),
            Ok(Incoming::Response { id, result }) => self.response(id, result),
            Ok(Incoming::Request { id, method, params }) => {
                let answer = match message::answer_server_request(
                    &method,
                    &params,
                    &self.config.workspace_configuration,
                    &self.folders,
                ) {
                    Ok(result) => message::response_ok(id, result),
                    Err((code, text)) => message::response_err(id, code, &text),
                };
                lock(&self.shared.inner).metrics.server_requests += 1;
                self.send(&answer);
            }
            Ok(Incoming::Notification { method, params }) => {
                lock(&self.shared.inner).metrics.notifications_in += 1;
                let quiescent = (method == "experimental/serverStatus")
                    .then(|| params.get("quiescent").and_then(Value::as_bool))
                    .flatten();
                self.events.push(LspEvent::Notification {
                    generation: self.generation,
                    method,
                    params,
                });
                if let Some(quiescent) = quiescent {
                    self.set_quiescent(quiescent);
                }
            }
        }
    }

    fn set_quiescent(&mut self, quiescent: bool) {
        if self.quiescent == Some(quiescent) {
            return;
        }
        self.quiescent = Some(quiescent);
        if matches!(self.phase, Phase::Running { .. }) {
            self.set_state(ServerState::Running {
                quiescent: Some(quiescent),
            });
        }
    }

    fn response(&mut self, id: Option<u64>, result: Result<Value, LspError>) {
        let internal = match &self.phase {
            Phase::Initializing { init_id, .. } => Some((*init_id, true)),
            Phase::ShuttingDown { shutdown_id, .. } => Some((*shutdown_id, false)),
            _ => None,
        };
        match (internal, id) {
            (Some((internal_id, true)), Some(id)) if internal_id == id => {
                self.initialize_answered(result);
                return;
            }
            (Some((internal_id, false)), Some(id)) if internal_id == id => {
                self.shutdown_answered();
                return;
            }
            _ => {}
        }
        let mut inner = lock(&self.shared.inner);
        let Some(id) = id else {
            inner.metrics.unknown_responses += 1;
            return;
        };
        let removed = inner.pending.remove(&id);
        match removed {
            Some(pending) => {
                inner.metrics.requests_completed += 1;
                drop(inner);
                self.events.push(LspEvent::Response {
                    ticket: id,
                    generation: pending.generation,
                    result,
                });
            }
            None if id < inner.next_id => inner.metrics.late_responses += 1,
            None => inner.metrics.unknown_responses += 1,
        }
    }

    fn initialize_answered(&mut self, result: Result<Value, LspError>) {
        let result = match result {
            Ok(result) => result,
            Err(e) => return self.connection_failed(format!("initialize failed: {e}")),
        };
        let phase = std::mem::replace(&mut self.phase, Phase::Idle);
        let Phase::Initializing { conn, .. } = phase else {
            self.phase = phase;
            return;
        };
        let capabilities = result
            .get("capabilities")
            .cloned()
            .unwrap_or_else(|| json!({}));
        let encoding = capabilities
            .get("positionEncoding")
            .and_then(Value::as_str)
            .unwrap_or("utf-16")
            .to_string();
        self.phase = Phase::Running { conn };
        self.send(&message::notification("initialized", json!({})));
        {
            let mut inner = lock(&self.shared.inner);
            inner.metrics.notifications_out += 1;
            inner.capabilities = Some(capabilities);
            inner.server_info = result.get("serverInfo").cloned();
            inner.position_encoding = encoding;
            inner.writer = self.conn().and_then(Connection::sender);
        }
        self.set_state(ServerState::Running {
            quiescent: self.quiescent,
        });
    }

    fn shutdown_answered(&mut self) {
        self.shutdown_acknowledged = true;
        self.send(&message::notification("exit", Value::Null));
        if let Phase::ShuttingDown { conn, .. } = &mut self.phase {
            // With every sender gone the writer flushes `exit` and closes the server's stdin.
            conn.close_input();
        }
    }

    fn start_generation(&mut self) {
        self.generation += 1;
        self.quiescent = None;
        self.exit_noticed = None;
        self.next_liveness_check = deadline_after(LIVENESS_INTERVAL);
        {
            let mut inner = lock(&self.shared.inner);
            inner.generation = self.generation;
            inner.capabilities = None;
            inner.server_info = None;
            inner.position_encoding = "utf-16".to_string();
            inner.pid = None;
            inner.writer = None;
            inner.metrics.starts += 1;
            if self.generation > 1 {
                inner.metrics.restarts += 1;
            }
        }
        self.set_state(ServerState::Starting);
        let spawned = Connection::spawn(
            &self.config.command,
            self.generation,
            &self.tx,
            self.config.max_message_bytes,
            self.config.outgoing_queue,
            self.config.stderr_tail_bytes,
        );
        match spawned {
            Err(failure) => {
                {
                    let mut inner = lock(&self.shared.inner);
                    inner.metrics.spawn_failures += 1;
                    inner.metrics.last_error = Some(failure.message.clone());
                }
                if failure.permanent {
                    self.fail(failure.message);
                } else {
                    self.schedule_restart(failure.message);
                }
            }
            Ok(conn) => {
                let init_id = {
                    let mut inner = lock(&self.shared.inner);
                    inner.pid = Some(conn.pid());
                    inner.allocate_id()
                };
                let params =
                    message::initialize_params(&self.root, &self.config.initialization_options);
                self.phase = Phase::Initializing {
                    conn,
                    init_id,
                    deadline: deadline_after(self.config.init_timeout),
                };
                self.send(&message::request(init_id, "initialize", params));
                self.set_state(ServerState::Initializing);
            }
        }
    }

    /// The connection died or broke the protocol while not shutting down.
    fn connection_failed(&mut self, reason: String) {
        let phase = std::mem::replace(&mut self.phase, Phase::Idle);
        let mut conn = match phase {
            Phase::Initializing { conn, .. } | Phase::Running { conn } => conn,
            other => {
                self.phase = other;
                return;
            }
        };
        // Give a dying process a moment so its real exit status is reported.
        let exit = conn.finish(Duration::from_millis(200));
        let stderr_tail = conn.stderr_tail();
        drop(conn);
        let reason = format!("{reason} ({})", exit.description);
        let mut failed: Vec<(u64, u64)> = {
            let mut inner = lock(&self.shared.inner);
            inner.writer = None;
            inner.pid = None;
            inner.capabilities = None;
            inner.server_info = None;
            inner.metrics.crashes += 1;
            inner.metrics.last_error = Some(reason.clone());
            let failed: Vec<(u64, u64)> = inner
                .pending
                .drain()
                .map(|(ticket, pending)| (ticket, pending.generation))
                .collect();
            inner.metrics.requests_failed += failed.len() as u64;
            failed
        };
        failed.sort_unstable();
        for (ticket, generation) in failed {
            self.events.push(LspEvent::Response {
                ticket,
                generation,
                result: Err(LspError::ConnectionLost(reason.clone())),
            });
        }
        self.events.push(LspEvent::Crashed {
            generation: self.generation,
            reason: reason.clone(),
            stderr_tail,
        });
        self.schedule_restart(reason);
    }

    fn schedule_restart(&mut self, reason: String) {
        let now = Instant::now();
        self.crash_times.push_back(now);
        while let Some(&first) = self.crash_times.front() {
            if now.duration_since(first) > self.config.restart_window {
                self.crash_times.pop_front();
            } else {
                break;
            }
        }
        let crashes = self.crash_times.len();
        if crashes > self.config.max_restarts as usize {
            self.fail(format!(
                "language server failed {crashes} times within {:?}; giving up (last: {reason})",
                self.config.restart_window
            ));
            return;
        }
        let attempt = u32::try_from(crashes).unwrap_or(u32::MAX);
        let retry_in = self.config.backoff(attempt);
        self.phase = Phase::Backoff {
            until: deadline_after(retry_in),
        };
        self.set_state(ServerState::Restarting { attempt, retry_in });
    }

    fn fail(&mut self, reason: String) {
        self.phase = Phase::Idle;
        lock(&self.shared.inner).metrics.last_error = Some(reason.clone());
        self.set_state(ServerState::Failed { reason });
    }

    fn handle_timers(&mut self, now: Instant) {
        let expired: Vec<(u64, u64)> = {
            let mut inner = lock(&self.shared.inner);
            let mut expired: Vec<(u64, u64)> = inner
                .pending
                .iter()
                .filter(|(_, p)| p.deadline <= now)
                .map(|(ticket, p)| (*ticket, p.generation))
                .collect();
            expired.sort_unstable();
            for (ticket, _) in &expired {
                inner.pending.remove(ticket);
            }
            inner.metrics.requests_timed_out += expired.len() as u64;
            expired
        };
        for (ticket, generation) in expired {
            self.send(&message::notification(
                "$/cancelRequest",
                json!({ "id": ticket }),
            ));
            self.events.push(LspEvent::Response {
                ticket,
                generation,
                result: Err(LspError::Timeout),
            });
        }
        if now >= self.next_liveness_check {
            self.next_liveness_check = deadline_after(LIVENESS_INTERVAL);
            let exited = match &mut self.phase {
                Phase::Initializing { conn, .. } | Phase::Running { conn } => conn.has_exited(),
                _ => false,
            };
            if exited {
                let noticed = *self.exit_noticed.get_or_insert(now);
                if now.duration_since(noticed) >= EXIT_GRACE {
                    self.connection_failed(
                        "server process exited but its output stayed open".to_string(),
                    );
                    return;
                }
            }
        }
        let action = match &self.phase {
            Phase::Initializing { deadline, .. } if *deadline <= now => {
                Some(TimerAction::InitTimeout)
            }
            Phase::Backoff { until } if *until <= now => Some(TimerAction::Restart),
            Phase::ShuttingDown { deadline, .. } if *deadline <= now => {
                Some(TimerAction::ShutdownTimeout)
            }
            _ => None,
        };
        match action {
            Some(TimerAction::InitTimeout) => self.connection_failed(format!(
                "initialize was not answered within {:?}",
                self.config.init_timeout
            )),
            Some(TimerAction::Restart) => self.start_generation(),
            Some(TimerAction::ShutdownTimeout) => self.finish_shutdown(),
            None => {}
        }
    }

    fn stop(&mut self, graceful: bool) {
        let phase = std::mem::replace(&mut self.phase, Phase::Idle);
        match phase {
            Phase::Running { conn } if graceful => {
                let shutdown_id = {
                    let mut inner = lock(&self.shared.inner);
                    inner.writer = None;
                    inner.allocate_id()
                };
                self.phase = Phase::ShuttingDown {
                    conn,
                    shutdown_id,
                    deadline: deadline_after(self.config.shutdown_timeout),
                };
                self.send(&message::request(shutdown_id, "shutdown", Value::Null));
                self.set_state(ServerState::ShuttingDown);
            }
            Phase::ShuttingDown { .. } if graceful => self.phase = phase,
            Phase::Running { mut conn }
            | Phase::Initializing { mut conn, .. }
            | Phase::ShuttingDown { mut conn, .. } => {
                let exit = conn.finish(Duration::ZERO);
                self.complete_stop(exit.killed);
            }
            Phase::Idle | Phase::Backoff { .. } => self.complete_stop(false),
            Phase::Done => self.phase = Phase::Done,
        }
    }

    /// The process closed its output, or the shutdown deadline passed.
    fn finish_shutdown(&mut self) {
        let phase = std::mem::replace(&mut self.phase, Phase::Idle);
        let Phase::ShuttingDown {
            mut conn, deadline, ..
        } = phase
        else {
            self.phase = phase;
            return;
        };
        let exit = conn.finish(deadline.saturating_duration_since(Instant::now()));
        self.complete_stop(exit.killed);
    }

    fn complete_stop(&mut self, killed: bool) {
        let mut failed: Vec<(u64, u64)> = {
            let mut inner = lock(&self.shared.inner);
            inner.writer = None;
            inner.pid = None;
            if killed {
                inner.metrics.forced_kills += 1;
            }
            let failed: Vec<(u64, u64)> = inner
                .pending
                .drain()
                .map(|(ticket, pending)| (ticket, pending.generation))
                .collect();
            inner.metrics.requests_failed += failed.len() as u64;
            failed
        };
        failed.sort_unstable();
        for (ticket, generation) in failed {
            self.events.push(LspEvent::Response {
                ticket,
                generation,
                result: Err(LspError::Stopped),
            });
        }
        self.phase = Phase::Done;
        self.set_state(ServerState::Stopped);
        self.report = Some(ShutdownReport {
            shutdown_acknowledged: self.shutdown_acknowledged,
            forced_kill: killed,
            metrics: lock(&self.shared.inner).metrics.clone(),
        });
    }
}
