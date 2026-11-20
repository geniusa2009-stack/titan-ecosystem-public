//! A small, deterministic fake language server for failure-injection tests of `titan-lsp`.
//!
//! It does not analyze anything. Requests are answered with an echo of the request
//! (`{"method", "id", "params"}`), so tests can verify that ids and tickets are matched.
//! `didOpen`/`didChange` publish one diagnostic carrying the document version.
//!
//! Behaviour flags (all optional):
//!
//! ```text
//! --crash-after-initialize          exit(101) after the `initialized` notification
//! --crash-on <method>               exit(3) when a message with this method arrives
//! --hang-on <method>                never answer requests with this method (even `initialize`)
//! --answer-after-cancel             answer a hung request when it is cancelled (a late answer)
//! --delay-ms <n>                    sleep n ms before every echo answer
//! --reverse-order                   hold echo requests until a `fake/flush` notification,
//!                                   then answer them newest first
//! --garbage-after-initialize        after `initialized`, write a frame whose body is not JSON
//! --bad-header-after-initialize     after `initialized`, write a malformed header
//! --oversize-response <bytes>       answer echo requests with a body of at least <bytes>
//! --exit-on-shutdown-delay-ms <n>   after `exit`, wait n ms before exiting
//! --ignore-shutdown                 never answer `shutdown`, ignore `exit`, survive stdin EOF
//! --server-requests                 after `initialized`, send workspace/configuration,
//!                                   client/registerCapability, window/workDoneProgress/create
//!                                   and an unknown request; report the client's answers in a
//!                                   `fake/clientResponses` notification
//! --server-status                   after `initialized`, send experimental/serverStatus with
//!                                   quiescent false, then true
//! --position-encoding <enc>         report this positionEncoding in the capabilities
//! ```
//!
//! Every `$/cancelRequest` is acknowledged with a `fake/cancelled {id}` notification, the
//! `fake/state` request returns the received `initialize` params and the open documents, and
//! `fake/sendUnknownResponses` first writes two responses with ids the client never issued.

use std::collections::BTreeMap;
use std::io::{self, Write};
use std::process;
use std::thread;
use std::time::Duration;

use serde_json::{Value, json};
use titan_protocol::framing;

#[derive(Default)]
struct Options {
    crash_after_initialize: bool,
    crash_on: Vec<String>,
    hang_on: Vec<String>,
    answer_after_cancel: bool,
    delay_ms: u64,
    reverse_order: bool,
    garbage_after_initialize: bool,
    bad_header_after_initialize: bool,
    oversize_response: Option<usize>,
    exit_delay_ms: u64,
    ignore_shutdown: bool,
    server_requests: bool,
    server_status: bool,
    position_encoding: Option<String>,
}

fn parse_args() -> Result<Options, String> {
    let mut options = Options::default();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = |name: &str| args.next().ok_or_else(|| format!("{name} needs a value"));
        match arg.as_str() {
            "--crash-after-initialize" => options.crash_after_initialize = true,
            "--crash-on" => options.crash_on.push(value("--crash-on")?),
            "--hang-on" => options.hang_on.push(value("--hang-on")?),
            "--answer-after-cancel" => options.answer_after_cancel = true,
            "--delay-ms" => options.delay_ms = number(&value("--delay-ms")?)?,
            "--reverse-order" => options.reverse_order = true,
            "--garbage-after-initialize" => options.garbage_after_initialize = true,
            "--bad-header-after-initialize" => options.bad_header_after_initialize = true,
            "--oversize-response" => {
                let bytes = number(&value("--oversize-response")?)?;
                options.oversize_response =
                    Some(usize::try_from(bytes).map_err(|e| e.to_string())?);
            }
            "--exit-on-shutdown-delay-ms" => {
                options.exit_delay_ms = number(&value("--exit-on-shutdown-delay-ms")?)?;
            }
            "--ignore-shutdown" => options.ignore_shutdown = true,
            "--server-requests" => options.server_requests = true,
            "--server-status" => options.server_status = true,
            "--position-encoding" => {
                options.position_encoding = Some(value("--position-encoding")?);
            }
            other => return Err(format!("unknown argument {other}")),
        }
    }
    Ok(options)
}

fn number(text: &str) -> Result<u64, String> {
    text.parse::<u64>()
        .map_err(|e| format!("bad number {text:?}: {e}"))
}

struct Fake {
    options: Options,
    out: io::Stdout,
    initialize_params: Value,
    open_documents: BTreeMap<String, i64>,
    hung: Vec<Value>,
    held: Vec<(Value, String, Value)>,
    client_responses: Vec<Value>,
    expected_client_responses: usize,
}

impl Fake {
    fn write(&self, message: &Value) {
        let body = match serde_json::to_vec(message) {
            Ok(body) => body,
            Err(e) => die(&format!("cannot encode: {e}")),
        };
        self.write_raw_frame(&body);
    }

    fn write_raw_frame(&self, body: &[u8]) {
        let mut out = self.out.lock();
        if let Err(e) = framing::write_message(&mut out, body) {
            die(&format!("cannot write: {e}"));
        }
    }

    fn write_bytes(&self, bytes: &[u8]) {
        let mut out = self.out.lock();
        if out.write_all(bytes).and_then(|()| out.flush()).is_err() {
            die("cannot write");
        }
    }

    fn respond(&self, id: Value, result: Value) {
        self.write(&json!({ "jsonrpc": "2.0", "id": id, "result": result }));
    }

    fn notify(&self, method: &str, params: Value) {
        self.write(&json!({ "jsonrpc": "2.0", "method": method, "params": params }));
    }

    fn echo(&self, id: Value, method: &str, params: Value) {
        if self.options.delay_ms > 0 {
            thread::sleep(Duration::from_millis(self.options.delay_ms));
        }
        let result = json!({ "method": method, "id": id, "params": params });
        match self.options.oversize_response {
            Some(bytes) => {
                let padding = "x".repeat(bytes);
                self.respond(id, json!({ "echo": result, "padding": padding }));
            }
            None => self.respond(id, result),
        }
    }

    fn publish_diagnostics(&self, uri: &str, version: i64) {
        self.notify(
            "textDocument/publishDiagnostics",
            json!({
                "uri": uri,
                "version": version,
                "diagnostics": [{
                    "range": {
                        "start": { "line": 0, "character": 0 },
                        "end": { "line": 0, "character": 1 }
                    },
                    "severity": 2,
                    "source": "titan-fake-lsp",
                    "message": format!("fake diagnostic for version {version}")
                }]
            }),
        );
    }

    fn capabilities(&self) -> Value {
        let mut capabilities = json!({
            "textDocumentSync": { "openClose": true, "change": 1, "save": { "includeText": false } },
            "hoverProvider": true,
            "definitionProvider": true,
            "referencesProvider": true,
            "documentSymbolProvider": true,
            "workspaceSymbolProvider": true,
            "completionProvider": { "triggerCharacters": [":", "."] }
        });
        if let (Some(encoding), Some(object)) = (
            self.options.position_encoding.as_ref(),
            capabilities.as_object_mut(),
        ) {
            object.insert("positionEncoding".into(), Value::String(encoding.clone()));
        }
        capabilities
    }

    fn after_initialized(&mut self) {
        if self.options.crash_after_initialize {
            eprintln!("titan-fake-lsp: crash after initialize");
            process::exit(101);
        }
        if self.options.garbage_after_initialize {
            self.write_bytes(b"Content-Length: 12\r\n\r\nthis is junk");
        }
        if self.options.bad_header_after_initialize {
            self.write_bytes(b"this is not a header\r\n\r\n");
        }
        if self.options.server_status {
            let busy = json!({ "health": "ok", "quiescent": false, "message": "loading" });
            self.notify("experimental/serverStatus", busy);
            thread::sleep(Duration::from_millis(50));
            self.notify(
                "experimental/serverStatus",
                json!({ "health": "ok", "quiescent": true }),
            );
        }
        if self.options.server_requests {
            let requests = [
                (
                    "workspace/configuration",
                    json!({ "items": [{ "section": "fake" }, { "section": "fake.nested" }, { "section": "missing" }] }),
                ),
                (
                    "client/registerCapability",
                    json!({ "registrations": [{ "id": "1", "method": "workspace/didChangeWatchedFiles" }] }),
                ),
                (
                    "window/workDoneProgress/create",
                    json!({ "token": "fake-progress" }),
                ),
                ("fake/unknownRequest", json!({})),
            ];
            self.expected_client_responses = requests.len();
            for (index, (method, params)) in requests.into_iter().enumerate() {
                self.write(&json!({
                    "jsonrpc": "2.0",
                    "id": format!("srv-{index}"),
                    "method": method,
                    "params": params
                }));
            }
        }
    }

    fn handle(&mut self, message: Value) {
        let method = message
            .get("method")
            .and_then(Value::as_str)
            .map(str::to_string);
        let id = message.get("id").cloned();
        let params = message.get("params").cloned().unwrap_or(Value::Null);
        let Some(method) = method else {
            // A response to one of our server-to-client requests.
            self.client_responses.push(message);
            if self.client_responses.len() == self.expected_client_responses {
                let responses = Value::Array(std::mem::take(&mut self.client_responses));
                self.notify("fake/clientResponses", json!({ "responses": responses }));
            }
            return;
        };
        if self.options.crash_on.contains(&method) {
            eprintln!("titan-fake-lsp: crash-on {method}");
            process::exit(3);
        }
        match (method.as_str(), id) {
            (method, Some(id)) if self.options.hang_on.iter().any(|m| m == method) => {
                self.hung.push(id);
            }
            ("initialize", Some(id)) => {
                self.initialize_params = params;
                let capabilities = self.capabilities();
                self.respond(
                    id,
                    json!({
                        "capabilities": capabilities,
                        "serverInfo": { "name": "titan-fake-lsp", "version": env!("CARGO_PKG_VERSION") }
                    }),
                );
            }
            ("initialized", None) => self.after_initialized(),
            ("shutdown", Some(id)) => {
                if !self.options.ignore_shutdown {
                    self.respond(id, Value::Null);
                }
            }
            ("exit", None) => {
                if !self.options.ignore_shutdown {
                    thread::sleep(Duration::from_millis(self.options.exit_delay_ms));
                    process::exit(0);
                }
            }
            ("$/cancelRequest", None) => {
                let cancelled = params.get("id").cloned().unwrap_or(Value::Null);
                if let Some(index) = self.hung.iter().position(|id| *id == cancelled) {
                    let id = self.hung.remove(index);
                    if self.options.answer_after_cancel {
                        self.respond(id, json!({ "late": true }));
                    }
                }
                self.notify("fake/cancelled", json!({ "id": cancelled }));
            }
            ("textDocument/didOpen", None) => {
                let document = params.get("textDocument").cloned().unwrap_or(Value::Null);
                let uri = document.get("uri").and_then(Value::as_str).unwrap_or("");
                let version = document.get("version").and_then(Value::as_i64).unwrap_or(0);
                self.open_documents.insert(uri.to_string(), version);
                self.publish_diagnostics(uri, version);
            }
            ("textDocument/didChange", None) => {
                let document = params.get("textDocument").cloned().unwrap_or(Value::Null);
                let uri = document.get("uri").and_then(Value::as_str).unwrap_or("");
                let version = document.get("version").and_then(Value::as_i64).unwrap_or(0);
                self.open_documents.insert(uri.to_string(), version);
                self.publish_diagnostics(uri, version);
            }
            ("textDocument/didClose", None) => {
                let uri = params
                    .pointer("/textDocument/uri")
                    .and_then(Value::as_str)
                    .unwrap_or("");
                self.open_documents.remove(uri);
            }
            ("fake/flush", None) => {
                let held = std::mem::take(&mut self.held);
                for (id, method, params) in held.into_iter().rev() {
                    self.echo(id, &method, params);
                }
            }
            ("fake/state", Some(id)) => {
                let state = json!({
                    "initializeParams": self.initialize_params,
                    "openDocuments": self.open_documents,
                });
                self.respond(id, state);
            }
            ("fake/sendUnknownResponses", Some(id)) => {
                // Responses to ids the client never issued, then the real answer.
                self.respond(json!(987_654_321u64), Value::Null);
                self.respond(json!("not-a-number"), Value::Null);
                self.echo(id, "fake/sendUnknownResponses", params);
            }
            (method, Some(id)) => {
                if self.options.reverse_order {
                    self.held.push((id, method.to_string(), params));
                } else {
                    self.echo(id, method, params);
                }
            }
            (_, None) => {}
        }
    }
}

fn die(message: &str) -> ! {
    eprintln!("titan-fake-lsp: {message}");
    process::exit(2);
}

fn main() {
    let options = match parse_args() {
        Ok(options) => options,
        Err(e) => die(&e),
    };
    let ignore_shutdown = options.ignore_shutdown;
    let mut fake = Fake {
        options,
        out: io::stdout(),
        initialize_params: Value::Null,
        open_documents: BTreeMap::new(),
        hung: Vec::new(),
        held: Vec::new(),
        client_responses: Vec::new(),
        expected_client_responses: 0,
    };
    eprintln!("titan-fake-lsp: started (pid {})", process::id());
    let stdin = io::stdin();
    let mut input = stdin.lock();
    loop {
        match framing::read_message(&mut input, framing::DEFAULT_MAX_MESSAGE_BYTES) {
            Ok(Some(body)) => match serde_json::from_slice::<Value>(&body) {
                Ok(message) => fake.handle(message),
                Err(e) => die(&format!("client sent malformed JSON: {e}")),
            },
            Ok(None) => break,
            Err(e) => die(&format!("client framing error: {e}")),
        }
    }
    if ignore_shutdown {
        // Simulate a server that hangs on to life after its input is closed.
        loop {
            thread::sleep(Duration::from_secs(1));
        }
    }
}
