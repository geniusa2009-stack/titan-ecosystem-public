//! Tests against a real rust-analyzer. They need the binary, so they are ignored by default;
//! CI runs them with `--include-ignored` and sets `TITAN_RUST_ANALYZER` to
//! `rustup which rust-analyzer` of the pinned toolchain.

mod common;

use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use common::{Events, kill_9, process_alive};
use serde_json::{Value, json};
use tempfile::TempDir;
use titan_lsp::{
    LspCommand, LspError, LspEvent, ServerState, Supervisor, SupervisorConfig, codes, path_to_uri,
};

/// Writes to the process's real stderr, bypassing libtest's output capture, so the CI log
/// keeps what rust-analyzer actually answered even when the tests pass.
macro_rules! report {
    ($($arg:tt)*) => {{
        use std::io::Write as _;
        let _ = writeln!(std::io::stderr().lock(), "[rust-analyzer test] {}", format_args!($($arg)*));
    }};
}

const QUIESCENT_TIMEOUT: Duration = Duration::from_secs(120);
const T: Duration = Duration::from_secs(60);

const CARGO_TOML: &str = r#"[package]
name = "titan_lsp_fixture"
version = "0.1.0"
edition = "2021"

[dependencies]

[workspace]
"#;

const OTHER_RS: &str = "/// Adds one to the input.
pub fn helper_add_one(value: u32) -> u32 {
    value + 1
}

pub struct OtherThing;
";

/// What is on disk: no call to `helper_add_one`.
const LIB_RS_ON_DISK: &str = "mod other;

pub fn on_disk_only() -> u32 {
    0
}
";

/// The unsaved buffer sent with didOpen: it calls `helper_add_one` on lines 3 and 4.
const LIB_RS_BUFFER: &str = "mod other;

pub fn caller_in_buffer() -> u32 {
    let first = other::helper_add_one(1);
    first + other::helper_add_one(2)
}
";

const LIB_RS_TYPE_ERROR: &str = "mod other;

pub fn broken() -> u32 {
    let x: u32 = \"s\";
    x
}
";

const LIB_RS_FIXED: &str = "mod other;

pub fn broken() -> u32 {
    let x: u32 = 7;
    x
}
";

fn rust_analyzer() -> PathBuf {
    match std::env::var_os("TITAN_RUST_ANALYZER") {
        Some(path) if !path.is_empty() => PathBuf::from(path),
        _ => panic!(
            "TITAN_RUST_ANALYZER is not set. Point it at a rust-analyzer binary, e.g. \
             `TITAN_RUST_ANALYZER=$(rustup which rust-analyzer) cargo test -p titan-lsp -- --include-ignored`"
        ),
    }
}

struct Fixture {
    _dir: TempDir,
    root: PathBuf,
    lib_uri: String,
    other_uri: String,
}

fn fixture() -> Fixture {
    let dir = tempfile::tempdir().expect("tempdir");
    let root = fs::canonicalize(dir.path()).expect("canonicalize");
    fs::create_dir_all(root.join("src")).expect("mkdir src");
    fs::write(root.join("Cargo.toml"), CARGO_TOML).expect("write Cargo.toml");
    fs::write(root.join("src/lib.rs"), LIB_RS_ON_DISK).expect("write lib.rs");
    fs::write(root.join("src/other.rs"), OTHER_RS).expect("write other.rs");
    Fixture {
        lib_uri: path_to_uri(&root.join("src/lib.rs")),
        other_uri: path_to_uri(&root.join("src/other.rs")),
        root,
        _dir: dir,
    }
}

/// Keeps rust-analyzer light: no `cargo check`, no build scripts, no proc-macro server, no
/// cache priming. `type-mismatch` (E0308) is an experimental native diagnostic in
/// rust-analyzer, so experimental diagnostics are enabled for the diagnostics test.
fn ra_settings() -> Value {
    json!({
        "checkOnSave": false,
        "cargo": { "buildScripts": { "enable": false } },
        "procMacro": { "enable": false },
        "cachePriming": { "enable": false },
        "diagnostics": { "experimental": { "enable": true } }
    })
}

fn start(fixture: &Fixture) -> (Supervisor, Events) {
    let command = LspCommand::new(rust_analyzer()).cwd(&fixture.root);
    let mut config = SupervisorConfig::new(command, &fixture.root);
    config.initialization_options = ra_settings();
    config.workspace_configuration = json!({ "rust-analyzer": ra_settings() });
    config.init_timeout = T;
    config.request_timeout = T;
    config.shutdown_timeout = Duration::from_secs(10);
    config.backoff_initial = Duration::from_millis(200);
    let (events, callback) = Events::new();
    (Supervisor::start(config, callback), events)
}

fn wait_quiescent(events: &Events, generation: u64) {
    let started = Instant::now();
    events.wait_state(
        QUIESCENT_TIMEOUT,
        &format!("generation {generation} quiescent"),
        |g, s| {
            g == generation
                && *s
                    == ServerState::Running {
                        quiescent: Some(true),
                    }
        },
    );
    report!(
        "rust-analyzer generation {generation} quiescent after {:?}",
        started.elapsed()
    );
}

fn did_open(sup: &Supervisor, uri: &str, version: i64, text: &str) -> u64 {
    sup.notify(
        "textDocument/didOpen",
        json!({ "textDocument": { "uri": uri, "languageId": "rust", "version": version, "text": text } }),
    )
    .expect("didOpen")
}

/// Position (UTF-16; the fixtures are ASCII) of the first occurrence of `needle` on `line`.
fn position_of(text: &str, line: usize, needle: &str) -> Value {
    let line_text = text.lines().nth(line).expect("line exists");
    let character = line_text.find(needle).expect("needle on line");
    json!({ "line": line, "character": character })
}

/// Sends a request until its answer satisfies `accept`. Retries only on `ContentModified`
/// / `RequestCancelled` errors and on answers that are not acceptable yet (rust-analyzer can
/// answer `null` or partial results while it is still loading); any other error fails.
fn request_until(
    sup: &Supervisor,
    events: &Events,
    method: &str,
    params: Value,
    accept: impl Fn(&Value) -> bool,
) -> (u64, Value) {
    let deadline = Instant::now() + Duration::from_secs(90);
    let mut attempts = 0;
    loop {
        attempts += 1;
        let ticket = sup
            .request(method, params.clone(), None)
            .unwrap_or_else(|e| panic!("{method}: {e}"));
        let (generation, result) = events.response(ticket, T + Duration::from_secs(5));
        let last = match result {
            Ok(value) if accept(&value) => {
                report!("{method}: accepted after {attempts} attempt(s)");
                return (generation, value);
            }
            Ok(value) => format!("unacceptable answer: {value}"),
            Err(LspError::Server { code, message, .. })
                if code == codes::CONTENT_MODIFIED || code == codes::REQUEST_CANCELLED =>
            {
                format!("server error {code}: {message}")
            }
            Err(e) => panic!("{method} failed: {e}"),
        };
        report!("{method}: attempt {attempts}: {last}");
        assert!(
            Instant::now() < deadline,
            "{method} never gave an acceptable answer: {last}"
        );
        std::thread::sleep(Duration::from_millis(500));
    }
}

/// (uri, start line) of every location in a definition/references answer
/// (`Location`, `Location[]` or `LocationLink[]`).
fn locations(value: &Value) -> Vec<(String, u64)> {
    let items: Vec<&Value> = match value {
        Value::Array(items) => items.iter().collect(),
        Value::Null => Vec::new(),
        other => vec![other],
    };
    items
        .into_iter()
        .filter_map(|item| {
            let (uri, range) = match item.get("targetUri") {
                Some(uri) => (uri, item.get("targetSelectionRange")?),
                None => (item.get("uri")?, item.get("range")?),
            };
            Some((
                uri.as_str()?.to_string(),
                range.pointer("/start/line")?.as_u64()?,
            ))
        })
        .collect()
}

fn hover_text(value: &Value) -> String {
    fn flatten(value: &Value, out: &mut String) {
        match value {
            Value::String(s) => out.push_str(s),
            Value::Array(items) => items.iter().for_each(|item| flatten(item, out)),
            Value::Object(object) => {
                if let Some(text) = object.get("value") {
                    flatten(text, out);
                }
            }
            _ => {}
        }
    }
    let mut text = String::new();
    if let Some(contents) = value.get("contents") {
        flatten(contents, &mut text);
    }
    text
}

fn symbol_names(value: &Value, out: &mut Vec<String>) {
    if let Some(items) = value.as_array() {
        for item in items {
            if let Some(name) = item.get("name").and_then(Value::as_str) {
                out.push(name.to_string());
            }
            if let Some(children) = item.get("children") {
                symbol_names(children, out);
            }
        }
    }
}

fn completion_labels(value: &Value) -> Vec<String> {
    let items = value.get("items").unwrap_or(value);
    items
        .as_array()
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.get("label").and_then(Value::as_str))
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn definition_params(fixture: &Fixture) -> Value {
    json!({
        "textDocument": { "uri": fixture.lib_uri },
        "position": position_of(LIB_RS_BUFFER, 3, "helper_add_one"),
    })
}

fn definition_is_in_other_rs(fixture: &Fixture) -> impl Fn(&Value) -> bool + '_ {
    move |value| locations(value) == vec![(fixture.other_uri.clone(), 1)]
}

#[test]
#[ignore = "needs rust-analyzer: set TITAN_RUST_ANALYZER (CI does)"]
fn rust_analyzer_navigation_hover_symbols_and_completion() {
    let fixture = fixture();
    let (sup, events) = start(&fixture);
    events.wait_running(1, T);

    // initialize + capabilities
    let caps = sup.server_capabilities().expect("capabilities");
    for key in [
        "definitionProvider",
        "referencesProvider",
        "hoverProvider",
        "documentSymbolProvider",
        "workspaceSymbolProvider",
        "completionProvider",
    ] {
        let value = &caps[key];
        assert!(
            *value == json!(true) || value.is_object(),
            "{key} missing: {caps}"
        );
    }
    assert_eq!(sup.position_encoding(), "utf-16");
    report!("serverInfo: {:?}", sup.server_info());
    report!(
        "rust-analyzer positionEncoding capability: {:?}",
        caps.get("positionEncoding")
    );

    // The buffer differs from disk: only the buffer calls helper_add_one.
    assert_eq!(did_open(&sup, &fixture.lib_uri, 1, LIB_RS_BUFFER), 1);
    wait_quiescent(&events, 1);

    // definition: the use in the unsaved buffer resolves into other.rs, line 1 (the name).
    let (_, definition) = request_until(
        &sup,
        &events,
        "textDocument/definition",
        definition_params(&fixture),
        definition_is_in_other_rs(&fixture),
    );
    report!("definition: {definition}");

    // references from the definition: the declaration and both uses in the buffer.
    let references_params = json!({
        "textDocument": { "uri": fixture.other_uri },
        "position": position_of(OTHER_RS, 1, "helper_add_one"),
        "context": { "includeDeclaration": true },
    });
    let (_, references) = request_until(
        &sup,
        &events,
        "textDocument/references",
        references_params,
        |value| {
            let found = locations(value);
            found.contains(&(fixture.other_uri.clone(), 1))
                && found.contains(&(fixture.lib_uri.clone(), 3))
                && found.contains(&(fixture.lib_uri.clone(), 4))
        },
    );
    report!("references: {references}");

    // hover: the signature.
    let hover_params = definition_params(&fixture);
    let (_, hover) = request_until(&sup, &events, "textDocument/hover", hover_params, |v| {
        hover_text(v).contains("pub fn helper_add_one(value: u32) -> u32")
    });
    report!("hover: {}", hover_text(&hover));

    // documentSymbol of the buffer.
    let (_, symbols) = request_until(
        &sup,
        &events,
        "textDocument/documentSymbol",
        json!({ "textDocument": { "uri": fixture.lib_uri } }),
        |v| {
            let mut names = Vec::new();
            symbol_names(v, &mut names);
            names.iter().any(|n| n == "other") && names.iter().any(|n| n == "caller_in_buffer")
        },
    );
    let mut names = Vec::new();
    symbol_names(&symbols, &mut names);
    assert!(
        !names.iter().any(|n| n == "on_disk_only"),
        "symbols come from the buffer, not the disk: {names:?}"
    );
    report!("documentSymbol names: {names:?}");

    // workspace/symbol (rust-analyzer searches types by default): the struct in other.rs.
    let (_, found) = request_until(
        &sup,
        &events,
        "workspace/symbol",
        json!({ "query": "OtherThing" }),
        |v| {
            v.as_array().is_some_and(|items| {
                items.iter().any(|item| {
                    item["name"] == "OtherThing"
                        && item["location"]["uri"] == json!(fixture.other_uri)
                })
            })
        },
    );
    report!("workspace/symbol: {found}");

    // completion right after `other::` on line 3 of the buffer.
    let line = LIB_RS_BUFFER.lines().nth(3).expect("line 3");
    let character = line.find("other::").expect("other::") + "other::".len();
    let (_, completion) = request_until(
        &sup,
        &events,
        "textDocument/completion",
        json!({
            "textDocument": { "uri": fixture.lib_uri },
            "position": { "line": 3, "character": character },
        }),
        |v| {
            let labels = completion_labels(v);
            labels.iter().any(|l| l.starts_with("helper_add_one"))
                && labels.iter().any(|l| l.starts_with("OtherThing"))
        },
    );
    report!("completion labels: {:?}", completion_labels(&completion));

    let pid = sup.pid().expect("pid");
    let report = sup.shutdown();
    assert!(report.shutdown_acknowledged, "{report:?}");
    assert!(!report.forced_kill, "{report:?}");
    assert!(!process_alive(pid), "rust-analyzer {pid} still exists");
    report!("metrics: {:?}", report.metrics);
}

#[test]
#[ignore = "needs rust-analyzer: set TITAN_RUST_ANALYZER (CI does)"]
fn rust_analyzer_diagnostics_follow_document_versions() {
    let fixture = fixture();
    let (sup, events) = start(&fixture);
    events.wait_running(1, T);
    did_open(&sup, &fixture.lib_uri, 1, LIB_RS_TYPE_ERROR);
    wait_quiescent(&events, 1);

    let has_type_error = |params: &Value| {
        params["diagnostics"].as_array().is_some_and(|diagnostics| {
            diagnostics.iter().any(|d| {
                d.pointer("/range/start/line") == Some(&json!(3))
                    && (d["code"] == "E0308"
                        || d["message"]
                            .as_str()
                            .is_some_and(|m| m.contains("expected u32")))
            })
        })
    };
    let lib_uri = fixture.lib_uri.clone();
    let (_, first) = events.wait_notification(
        0,
        QUIESCENT_TIMEOUT,
        "textDocument/publishDiagnostics",
        |_, p| p["uri"] == json!(lib_uri) && has_type_error(p),
    );
    report!("diagnostics for version 1: {first}");
    let version_present = first.get("version").is_some_and(|v| !v.is_null());
    if version_present {
        assert_eq!(first["version"], 1);
    }
    report!("publishDiagnostics carries a version: {version_present}");

    let mark = events.mark();
    sup.notify(
        "textDocument/didChange",
        json!({
            "textDocument": { "uri": fixture.lib_uri, "version": 2 },
            "contentChanges": [{ "text": LIB_RS_FIXED }],
        }),
    )
    .expect("didChange");
    let (_, fixed) = events.wait_notification(
        mark,
        QUIESCENT_TIMEOUT,
        "textDocument/publishDiagnostics",
        |_, p| {
            p["uri"] == json!(lib_uri)
                && (!version_present || p["version"] == 2)
                && !has_type_error(p)
        },
    );
    report!("diagnostics for version 2: {fixed}");
    sup.shutdown();
}

#[test]
#[ignore = "needs rust-analyzer: set TITAN_RUST_ANALYZER (CI does)"]
fn rust_analyzer_restarts_after_kill_and_shuts_down_cleanly() {
    let fixture = fixture();
    let (sup, events) = start(&fixture);
    events.wait_running(1, T);
    did_open(&sup, &fixture.lib_uri, 1, LIB_RS_BUFFER);
    wait_quiescent(&events, 1);
    let (generation, _) = request_until(
        &sup,
        &events,
        "textDocument/definition",
        definition_params(&fixture),
        definition_is_in_other_rs(&fixture),
    );
    assert_eq!(generation, 1);

    let first_pid = sup.pid().expect("pid");
    kill_9(first_pid);
    match events.wait_for(T, "crash", |e| matches!(e, LspEvent::Crashed { .. })) {
        LspEvent::Crashed {
            generation, reason, ..
        } => {
            assert_eq!(generation, 1);
            report!("crash reason: {reason}");
            assert!(reason.contains("signal: 9"), "{reason}");
        }
        other => panic!("{other:?}"),
    }
    events.wait_running(2, T);
    assert!(!process_alive(first_pid));
    let second_pid = sup.pid().expect("pid");
    assert_ne!(first_pid, second_pid);

    // Resync the buffer into the new process, then navigation works again.
    assert_eq!(did_open(&sup, &fixture.lib_uri, 1, LIB_RS_BUFFER), 2);
    wait_quiescent(&events, 2);
    let (generation, _) = request_until(
        &sup,
        &events,
        "textDocument/definition",
        definition_params(&fixture),
        definition_is_in_other_rs(&fixture),
    );
    assert_eq!(generation, 2);
    let metrics = sup.metrics();
    assert_eq!(metrics.crashes, 1);
    assert_eq!(metrics.restarts, 1);

    let report = sup.shutdown();
    assert!(report.shutdown_acknowledged, "{report:?}");
    assert!(!report.forced_kill, "{report:?}");
    assert!(
        !process_alive(second_pid),
        "rust-analyzer {second_pid} still exists"
    );
    assert!(!process_alive(first_pid));
}
