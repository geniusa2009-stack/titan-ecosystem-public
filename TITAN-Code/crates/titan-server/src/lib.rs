//! TITAN Code headless service.
//!
//! ## Runtime
//! ```text
//!  reader thread ──Message──▶ ┌────────────┐ ──bytes──▶ writer thread
//!  (framing, backpressure)    │ event loop │            (one message per write)
//!  task workers ──Task──────▶ │ (Session)  │
//!  scan progress ──────────▶  └────────────┘
//! ```
//! * The **event loop** owns all document state, so every document has exactly one writer
//!   and edits are applied in request order with no locks. It never blocks on slow work.
//! * Slow work (search, file listing, parsing, workspace scans, recovery listing) runs on the
//!   [`titan_tasks`] pool. Results come back as events. Results computed for a document
//!   revision that is no longer current are dropped (counted in `system.metrics`).
//! * Responses to background requests can therefore arrive **out of order**; every response
//!   carries its request id.
//! * The reader holds at most `max_in_flight_messages` unprocessed messages (credit-based
//!   backpressure), and the task queue is bounded (`-32013` when full).
//!
//! Diagnostics go to stderr only. There is no network listener.

pub mod client;
pub mod probe;
mod session;

use std::io::{self, BufRead, Write};
use std::path::PathBuf;
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread;
use std::time::Duration;

use titan_doc::Limits;
use titan_protocol::framing::{
    DEFAULT_MAX_MESSAGE_BYTES, FramingError, read_message, write_message,
};

use session::{Session, TaskResult};

/// Runtime configuration of one server instance.
#[derive(Clone, Debug)]
pub struct ServerOptions {
    pub limits: Limits,
    /// Directory for crash-recovery journals. `None` disables recovery.
    pub state_dir: Option<PathBuf>,
    pub workers: usize,
    pub queue_capacity: usize,
    pub max_message_bytes: usize,
    pub max_response_bytes: usize,
    pub max_in_flight_messages: usize,
    pub default_request_timeout: Duration,
    /// Watch the workspace for file-system changes (`titan-watch`).
    pub watch: bool,
    /// Language server for Rust documents; `None`: syntax-only intelligence.
    pub lsp: Option<LspOptions>,
    /// Live-heap statistics of the global allocator, when the binary installs a counting
    /// allocator (the `alloc-stats` feature of `titand`).
    pub heap_stats: Option<fn() -> serde_json::Value>,
    /// Build the project graph (`--no-graph` disables it).
    pub graph: bool,
    /// Do not build the graph for workspaces with more indexed files than this.
    pub graph_max_files: usize,
    /// Simulated memory pressure (`--simulate-memory-pressure`), for testing the policy.
    pub simulated_pressure: Option<String>,
}

/// How to run the language server.
#[derive(Clone, Debug)]
pub struct LspOptions {
    /// Shown in evidence (`source`), e.g. `rust-analyzer`.
    pub name: String,
    pub program: PathBuf,
    pub args: Vec<String>,
    /// `initializationOptions` and the answer to `workspace/configuration` (`rust-analyzer`
    /// section).
    pub settings: serde_json::Value,
    pub request_timeout: Duration,
    /// `settings` are the default light settings (reported as a limitation).
    pub light_mode: bool,
}

impl LspOptions {
    /// rust-analyzer with `titand`'s default light settings.
    pub fn rust_analyzer(program: PathBuf) -> Self {
        Self {
            name: "rust-analyzer".to_string(),
            program,
            args: Vec::new(),
            settings: session::default_rust_analyzer_settings(),
            request_timeout: Duration::from_secs(30),
            light_mode: true,
        }
    }
}

impl Default for ServerOptions {
    fn default() -> Self {
        let cpus = thread::available_parallelism().map_or(2, |n| n.get());
        Self {
            limits: Limits::default(),
            state_dir: None,
            workers: cpus.clamp(2, 4),
            queue_capacity: 1024,
            max_message_bytes: DEFAULT_MAX_MESSAGE_BYTES,
            max_response_bytes: 64 * 1024 * 1024,
            max_in_flight_messages: 64,
            default_request_timeout: Duration::from_secs(30),
            watch: true,
            lsp: None,
            heap_stats: None,
            graph: true,
            graph_max_files: 20_000,
            simulated_pressure: None,
        }
    }
}

pub(crate) enum Event {
    Message(Vec<u8>),
    InputClosed,
    InputError(String),
    Task(titan_tasks::TaskReport<TaskResult>),
    ScanProgress {
        epoch: u64,
        done: usize,
        total: usize,
    },
    Fs(titan_watch::WatchEvent),
    Lsp(Box<titan_lsp::LspEvent>),
}

/// Credit-based backpressure between the reader thread and the event loop.
struct Credits {
    state: Mutex<(usize, bool)>,
    changed: Condvar,
    max: usize,
}

impl Credits {
    fn new(max: usize) -> Self {
        Self {
            state: Mutex::new((0, false)),
            changed: Condvar::new(),
            max: max.max(1),
        }
    }

    /// Wait for a free slot. Returns false once closed.
    fn acquire(&self) -> bool {
        let mut state = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        while state.0 >= self.max && !state.1 {
            state = self
                .changed
                .wait(state)
                .unwrap_or_else(PoisonError::into_inner);
        }
        if state.1 {
            return false;
        }
        state.0 += 1;
        true
    }

    fn release(&self) {
        let mut state = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        state.0 = state.0.saturating_sub(1);
        drop(state);
        self.changed.notify_all();
    }

    fn close(&self) {
        self.state.lock().unwrap_or_else(PoisonError::into_inner).1 = true;
        self.changed.notify_all();
    }
}

fn reader_loop<R: BufRead>(
    mut reader: R,
    events: mpsc::Sender<Event>,
    credits: Arc<Credits>,
    max_body: usize,
) {
    loop {
        if !credits.acquire() {
            return;
        }
        let event = match read_message(&mut reader, max_body) {
            Ok(Some(body)) => Event::Message(body),
            Ok(None) => Event::InputClosed,
            Err(FramingError::Io(err)) => Event::InputError(format!("I/O error: {err}")),
            Err(err) => Event::InputError(err.to_string()),
        };
        let last = !matches!(event, Event::Message(_));
        if events.send(event).is_err() || last {
            return;
        }
    }
}

fn writer_loop<W: Write>(mut writer: W, outgoing: mpsc::Receiver<Vec<u8>>) -> io::Result<()> {
    for body in outgoing {
        write_message(&mut writer, &body)?;
    }
    writer.flush()
}

/// Serve one client until its input ends, a framing error occurs, or it sends
/// `session.shutdown`. Background work is cancelled on exit; journals of documents with
/// unsaved changes are kept for recovery.
pub fn run<R, W>(reader: R, writer: W, options: ServerOptions) -> io::Result<()>
where
    R: BufRead + Send + 'static,
    W: Write + Send + 'static,
{
    let (event_tx, event_rx) = mpsc::channel::<Event>();
    let (out_tx, out_rx) = mpsc::channel::<Vec<u8>>();
    let writer_thread = thread::Builder::new()
        .name("titan-writer".to_string())
        .spawn(move || writer_loop(writer, out_rx))?;
    let credits = Arc::new(Credits::new(options.max_in_flight_messages));
    {
        let events = event_tx.clone();
        let credits = Arc::clone(&credits);
        let max_body = options.max_message_bytes;
        // Not joined: it may be blocked reading input that never ends; it exits with the
        // process or when the input closes.
        thread::Builder::new()
            .name("titan-reader".to_string())
            .spawn(move || reader_loop(reader, events, credits, max_body))?;
    }
    let mut session = Session::new(options, event_tx)?;
    let result = event_loop(&mut session, &event_rx, &out_tx, &credits);
    session.close();
    for body in session.take_outgoing() {
        let _ = out_tx.send(body);
    }
    credits.close();
    drop(out_tx);
    let written = writer_thread
        .join()
        .unwrap_or_else(|_| Err(io::Error::other("writer thread panicked")));
    drop(session);
    result.and(written)
}

fn event_loop(
    session: &mut Session,
    events: &mpsc::Receiver<Event>,
    out: &mpsc::Sender<Vec<u8>>,
    credits: &Credits,
) -> io::Result<()> {
    loop {
        let wait = session.next_wakeup();
        let event = match events.recv_timeout(wait) {
            Ok(event) => Some(event),
            Err(RecvTimeoutError::Timeout) => None,
            Err(RecvTimeoutError::Disconnected) => return Ok(()),
        };
        let mut stop = false;
        match event {
            Some(Event::Message(body)) => {
                session.on_message(&body);
                credits.release();
            }
            Some(Event::Task(report)) => session.on_task(report),
            Some(Event::Fs(event)) => session.on_fs_event(event),
            Some(Event::Lsp(event)) => session.on_lsp_event(*event),
            Some(Event::ScanProgress { epoch, done, total }) => {
                session.on_scan_progress(epoch, done, total);
            }
            Some(Event::InputClosed) => stop = true,
            Some(Event::InputError(message)) => {
                session.on_framing_error(&message);
                stop = true;
            }
            None => {}
        }
        session.on_tick();
        for body in session.take_outgoing() {
            if out.send(body).is_err() {
                return Err(io::Error::other("the output stream is closed"));
            }
        }
        if stop || session.shutdown_requested() {
            return Ok(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::io::Cursor;
    use titan_protocol::rpc::codes;

    #[derive(Clone, Default)]
    struct SharedBuf(Arc<Mutex<Vec<u8>>>);

    impl Write for SharedBuf {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    fn frame(body: &str) -> Vec<u8> {
        let mut out = Vec::new();
        write_message(&mut out, body.as_bytes()).unwrap();
        out
    }

    fn run_with(input: Vec<u8>) -> Vec<Value> {
        let out = SharedBuf::default();
        run(Cursor::new(input), out.clone(), ServerOptions::default()).unwrap();
        let bytes = out.0.lock().unwrap().clone();
        let mut cursor = Cursor::new(bytes);
        let mut replies = Vec::new();
        while let Some(body) = read_message(&mut cursor, DEFAULT_MAX_MESSAGE_BYTES).unwrap() {
            replies.push(serde_json::from_slice(&body).unwrap());
        }
        replies
    }

    const HELLO: &str =
        r#"{"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"protocolVersion":1}}"#;

    #[test]
    fn parse_error_does_not_close_the_connection() {
        let mut input = frame("{not json");
        input.extend(frame(HELLO));
        let replies = run_with(input);
        assert_eq!(replies.len(), 2);
        assert_eq!(replies[0]["error"]["code"], codes::PARSE_ERROR);
        assert_eq!(replies[1]["result"]["positionEncoding"], "utf-16");
    }

    #[test]
    fn framing_error_gets_a_final_response() {
        let replies = run_with(b"Content-Length: nope\r\n\r\n".to_vec());
        assert_eq!(replies.len(), 1);
        assert_eq!(replies[0]["error"]["code"], codes::INVALID_REQUEST);
        assert!(replies[0]["id"].is_null());
    }

    #[test]
    fn shutdown_stops_the_loop() {
        let mut input = frame(HELLO);
        input.extend(frame(
            r#"{"jsonrpc":"2.0","id":2,"method":"session.shutdown"}"#,
        ));
        input.extend(frame(
            r#"{"jsonrpc":"2.0","id":3,"method":"system.health"}"#,
        ));
        assert_eq!(run_with(input).len(), 2);
    }

    #[test]
    fn credits_block_and_release() {
        let credits = Arc::new(Credits::new(1));
        assert!(credits.acquire());
        let waiter = {
            let credits = Arc::clone(&credits);
            thread::spawn(move || credits.acquire())
        };
        thread::sleep(Duration::from_millis(20));
        assert!(!waiter.is_finished(), "second acquire must wait");
        credits.release();
        assert!(waiter.join().unwrap());
        credits.close();
        assert!(!credits.acquire());
    }
}
