//! End-to-end semantic intelligence through `titand`:
//! * real rust-analyzer (CI sets `TITAN_RUST_ANALYZER`): initialization, unsaved-buffer
//!   synchronisation, definition, references, hover, symbols, completion, diagnostics tied
//!   to revisions, crash + restart;
//! * the fake language server (deterministic failure injection only): crash loops, hangs,
//!   timeouts, cancellation, stale answers, unavailable server.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_protocol::rpc::codes;
use titan_server::client::Client;

fn titand() -> &'static Path {
    Path::new(env!("CARGO_BIN_EXE_titand"))
}

fn fake_lsp() -> PathBuf {
    let path = titand().with_file_name(format!("titan-fake-lsp{}", std::env::consts::EXE_SUFFIX));
    assert!(
        path.exists(),
        "{} must be built (cargo test --workspace builds it)",
        path.display()
    );
    path
}

fn hello() -> Value {
    json!({ "protocolVersion": 1, "capabilities": {
        "semanticNotifications": true, "changeNotifications": true } })
}

fn start(args: &[String], dir: &Path) -> Client {
    let mut all = vec!["--no-recovery".to_string(), "--no-watch".to_string()];
    all.extend(args.iter().cloned());
    let mut client = Client::spawn_with_args(titand(), &all).expect("spawn titand");
    client.call("session.hello", hello()).unwrap();
    client
        .call(
            "workspace.open",
            json!({ "path": dir.display().to_string() }),
        )
        .unwrap();
    client
}

fn open(client: &mut Client, path: &str) -> u64 {
    client
        .call("document.open", json!({ "path": path }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap()
}

fn edit(
    client: &mut Client,
    id: u64,
    rev: u64,
    line: usize,
    ch: usize,
    end: (usize, usize),
    text: &str,
) -> u64 {
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": rev, "edits": [
                { "range": { "start": { "line": line, "character": ch },
                             "end": { "line": end.0, "character": end.1 } }, "text": text } ] }),
        )
        .unwrap()["revision"]
        .as_u64()
        .unwrap()
}

fn semantic_state(client: &mut Client) -> Value {
    client.call("intel.status", Value::Null).unwrap()["semantic"].clone()
}

fn wait_state(client: &mut Client, what: &str, ok: impl Fn(&Value) -> bool, secs: u64) -> Value {
    let deadline = Instant::now() + Duration::from_secs(secs);
    loop {
        let state = semantic_state(client);
        if ok(&state) {
            return state;
        }
        assert!(
            Instant::now() < deadline,
            "timed out waiting for {what}: {state}"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn semantic(result: &Value) -> &Value {
    result["results"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["kind"] == "semantic")
        .expect("semantic evidence")
}

fn position(id: u64, rev: u64, line: usize, character: usize) -> Value {
    json!({ "documentId": id, "expectedRevision": rev,
            "position": { "line": line, "character": character } })
}

// ----- real rust-analyzer ------------------------------------------------------------

fn rust_analyzer() -> String {
    std::env::var("TITAN_RUST_ANALYZER").expect(
        "set TITAN_RUST_ANALYZER to a rust-analyzer binary (CI: rustup which rust-analyzer)",
    )
}

fn fixture() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path();
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(
        root.join("Cargo.toml"),
        "[package]\nname = \"fixture\"\nversion = \"0.1.0\"\nedition = \"2021\"\n\n[dependencies]\n",
    )
    .unwrap();
    fs::write(
        root.join("src/lib.rs"),
        "mod other;\n\npub fn entry() -> u32 {\n    0\n}\n",
    )
    .unwrap();
    fs::write(
        root.join("src/other.rs"),
        "/// Adds one.\npub fn helper(x: u32) -> u32 {\n    x + 1\n}\n\npub struct Widget;\n",
    )
    .unwrap();
    dir
}

#[test]
#[ignore = "needs rust-analyzer: set TITAN_RUST_ANALYZER (CI does)"]
fn rust_analyzer_answers_semantic_queries_for_unsaved_buffers() {
    let project = fixture();
    let root = project.path().canonicalize().unwrap();
    let args = vec!["--rust-analyzer".to_string(), rust_analyzer()];
    let mut client = start(&args, &root);
    let hello_caps = client.call("system.metrics", Value::Null).unwrap();
    assert!(
        hello_caps["lsp"]["server"] == "rust-analyzer",
        "{hello_caps}"
    );

    let t0 = Instant::now();
    let id = open(&mut client, "src/lib.rs");
    let ready = wait_state(
        &mut client,
        "rust-analyzer ready",
        |s| s["state"] == "ready",
        180,
    );
    eprintln!("rust-analyzer ready after {:?}: {ready}", t0.elapsed());

    // Unsaved edit: the call to `helper` exists only in the buffer.
    let rev = edit(&mut client, id, 0, 3, 4, (3, 5), "other::helper(41)");
    let on_disk = fs::read_to_string(root.join("src/lib.rs")).unwrap();
    assert!(!on_disk.contains("helper"), "the edit is not saved");

    // Definition (line 3, inside `helper`).
    let mut definition = Value::Null;
    let deadline = Instant::now() + Duration::from_secs(60);
    while Instant::now() < deadline {
        definition = client
            .call("intel.definition", position(id, rev, 3, 12))
            .unwrap();
        if semantic(&definition)["items"]
            .as_array()
            .is_some_and(|i| !i.is_empty())
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    eprintln!("definition: {definition:#}");
    let sem = semantic(&definition);
    assert_eq!(sem["source"], "rust-analyzer");
    assert_eq!(sem["status"], "current");
    assert_eq!(sem["documentRevision"], rev);
    assert_eq!(sem["items"][0]["path"], "src/other.rs");
    assert_eq!(sem["items"][0]["range"]["start"]["line"], 1);
    // The syntax evidence is reported separately, as a name match.
    let syntax = definition["results"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["kind"] == "nameMatch")
        .unwrap();
    assert_eq!(syntax["source"], "tree-sitter");

    // References: the definition and the unsaved use.
    let references = client
        .call(
            "intel.references",
            json!({ "documentId": id, "expectedRevision": rev,
            "position": { "line": 3, "character": 12 }, "includeDeclaration": true }),
        )
        .unwrap();
    let refs = semantic(&references)["items"].as_array().unwrap().clone();
    eprintln!("references: {refs:?}");
    assert!(refs.iter().any(|r| r["path"] == "src/other.rs"));
    assert!(
        refs.iter()
            .any(|r| r["path"] == "src/lib.rs" && r["range"]["start"]["line"] == 3)
    );

    // Hover shows the signature.
    let hover = client
        .call("intel.hover", position(id, rev, 3, 12))
        .unwrap();
    let contents = semantic(&hover)["items"][0]["contents"]
        .as_str()
        .unwrap()
        .to_string();
    eprintln!("hover: {contents}");
    assert!(contents.contains("fn helper(x: u32) -> u32"), "{contents}");

    // Change impact of `helper`: known (rust-analyzer), structural (graph), heuristic.
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let g = client.call("graph.summary", Value::Null).unwrap();
        if g["state"] == "ready" && g["stale"] == false {
            break;
        }
        assert!(Instant::now() < deadline, "graph not ready: {g}");
        std::thread::sleep(Duration::from_millis(100));
    }
    let impact = client
        .call("twin.impact", position(id, rev, 3, 12))
        .unwrap();
    eprintln!("impact: {impact:#}");
    let known = &impact["known"];
    assert_eq!(known["definitions"]["status"], "current");
    assert_eq!(known["definitions"]["items"][0]["path"], "src/other.rs");
    assert!(
        known["references"]["items"]
            .as_array()
            .unwrap()
            .iter()
            .any(|r| r["path"] == "src/lib.rs")
    );
    let callers = known["callers"]["items"].as_array().unwrap();
    assert!(callers.iter().any(|c| c["name"] == "entry"), "{callers:?}");
    assert_eq!(impact["structural"]["module"]["name"], "other");
    assert_eq!(impact["structural"]["symbol"]["name"], "helper");
    assert_eq!(impact["structural"]["package"]["name"], "fixture");
    let affected: Vec<&str> = impact["affectedFiles"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["path"].as_str().unwrap())
        .collect();
    assert!(
        affected.contains(&"src/other.rs") && affected.contains(&"src/lib.rs"),
        "{affected:?}"
    );
    assert_eq!(impact["coverage"]["complete"], false);
    assert_eq!(impact["heuristic"]["nameMatches"]["kind"], "nameMatch");

    // Document symbols (semantic and syntax, side by side).
    let symbols = client
        .call("intel.documentSymbols", json!({ "documentId": id }))
        .unwrap();
    let names: Vec<String> = semantic(&symbols)["items"]
        .as_array()
        .unwrap()
        .iter()
        .map(|s| s["name"].as_str().unwrap().to_string())
        .collect();
    assert!(names.contains(&"entry".to_string()), "{names:?}");

    // Workspace symbols.
    let ws = client
        .call("intel.workspaceSymbols", json!({ "query": "Widget" }))
        .unwrap();
    let found = semantic(&ws)["items"].as_array().unwrap().clone();
    assert!(found.iter().any(|s| s["name"] == "Widget"), "{found:?}");

    // Completion after `other::`.
    let rev = edit(&mut client, id, rev, 3, 4, (3, 21), "other::");
    let completion = client
        .call("intel.completion", position(id, rev, 3, 11))
        .unwrap();
    let labels: Vec<String> = semantic(&completion)["items"]
        .as_array()
        .unwrap()
        .iter()
        .map(|c| c["label"].as_str().unwrap_or_default().to_string())
        .collect();
    eprintln!("completion labels: {labels:?}");
    assert!(labels.iter().any(|l| l.starts_with("helper")), "{labels:?}");

    // Diagnostics for an unsaved type error, tied to the revision that contains it.
    let bad = edit(
        &mut client,
        id,
        rev,
        3,
        4,
        (3, 11),
        "let x: u32 = \"s\";\n    0",
    );
    let diag = wait_semantic_diagnostics(&mut client, id, |p| {
        p["revision"] == bad && p["diagnostics"].as_array().is_some_and(|d| !d.is_empty())
    });
    eprintln!("diagnostics for revision {bad}: {diag:#}");
    assert_eq!(diag["current"], true);
    assert!(
        diag["diagnostics"]
            .as_array()
            .unwrap()
            .iter()
            .any(|d| d["range"]["start"]["line"] == 3)
    );
    let fixed = edit(&mut client, id, bad, 3, 4, (3, 21), "let x: u32 = 1;");
    let diag = wait_semantic_diagnostics(&mut client, id, |p| p["revision"] == fixed);
    assert!(
        !diag["diagnostics"]
            .as_array()
            .unwrap()
            .iter()
            .any(|d| d["message"]
                .as_str()
                .unwrap_or_default()
                .contains("mismatched")),
        "{diag}"
    );
    let intel_diag = client
        .call("intel.diagnostics", json!({ "documentId": id }))
        .unwrap();
    assert_eq!(semantic(&intel_diag)["status"], "current");
    assert_eq!(semantic(&intel_diag)["documentRevision"], fixed);

    // Crash: kill rust-analyzer. titand survives, restarts it and resynchronises.
    let pid = client.call("system.metrics", Value::Null).unwrap()["lsp"]["pid"]
        .as_u64()
        .unwrap();
    let status = std::process::Command::new("kill")
        .args(["-9", &pid.to_string()])
        .status()
        .unwrap();
    assert!(status.success());
    let restarted = wait_state(
        &mut client,
        "restart",
        |s| s["state"] == "ready" && s["generation"].as_u64().unwrap_or(0) >= 2,
        180,
    );
    assert_eq!(restarted["crashes"], 1);
    // Editing never stopped working.
    let rev = edit(
        &mut client,
        id,
        fixed,
        3,
        4,
        (3, 19),
        "let x = other::helper(1);",
    );
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let d = client
            .call("intel.definition", position(id, rev, 3, 20))
            .unwrap();
        if semantic(&d)["items"][0]["path"] == "src/other.rs" {
            assert_eq!(semantic(&d)["generation"], restarted["generation"]);
            break;
        }
        assert!(
            Instant::now() < deadline,
            "no definition after restart: {d}"
        );
        std::thread::sleep(Duration::from_millis(250));
    }
    let start = Instant::now();
    assert!(client.shutdown().unwrap().success());
    eprintln!("shutdown with rust-analyzer took {:?}", start.elapsed());
    // The language server is gone too.
    let alive = std::process::Command::new("kill")
        .args(["-0", &pid.to_string()])
        .status()
        .unwrap();
    assert!(!alive.success(), "the killed pid must not be alive");
}

fn wait_semantic_diagnostics(client: &mut Client, id: u64, ok: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + Duration::from_secs(90);
    loop {
        client.call("system.health", Value::Null).unwrap();
        for n in client.take_notifications() {
            if n["method"] == "document/semanticDiagnostics"
                && n["params"]["documentId"] == id
                && ok(&n["params"])
            {
                return n["params"].clone();
            }
        }
        assert!(
            Instant::now() < deadline,
            "no matching semantic diagnostics"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

// ----- fake server: failure injection -----------------------------------------------

fn fake_args(extra: &[&str]) -> Vec<String> {
    let mut args = vec![
        "--rust-analyzer".to_string(),
        fake_lsp().display().to_string(),
        "--lsp-name".to_string(),
        "titan-fake-lsp".to_string(),
    ];
    for a in extra {
        args.push("--lsp-arg".to_string());
        args.push((*a).to_string());
    }
    args
}

fn small_project() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    fs::write(dir.path().join("a.rs"), "fn a() {}\n").unwrap();
    dir
}

#[test]
fn unavailable_language_server_never_blocks_editing() {
    let project = small_project();
    let args = vec![
        "--rust-analyzer".to_string(),
        "/nonexistent/rust-analyzer".to_string(),
    ];
    let mut client = start(&args, project.path());
    let id = open(&mut client, "a.rs");
    let state = wait_state(&mut client, "failed", |s| s["state"] == "failed", 30);
    assert!(
        state["process"]["spawnFailures"].as_u64().unwrap() >= 1,
        "{state}"
    );
    let rev = edit(&mut client, id, 0, 0, 0, (0, 0), "// still editing\n");
    let def = client
        .call("intel.definition", position(id, rev, 1, 3))
        .unwrap();
    let sem = semantic(&def);
    assert_eq!(sem["status"], "unavailable");
    assert!(sem["error"].as_str().unwrap().contains("failed"), "{sem}");
    // Syntax evidence still answers, labelled as a name match.
    let syntax = def["results"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["kind"] == "nameMatch")
        .unwrap();
    assert_eq!(syntax["status"], "current");
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn crash_loop_is_contained_and_reported() {
    let project = small_project();
    let args = fake_args(&["--crash-on", "textDocument/definition"]);
    let mut client = start(&args, project.path());
    let id = open(&mut client, "a.rs");
    wait_state(
        &mut client,
        "running",
        |s| s["state"] == "indexing" || s["state"] == "ready",
        30,
    );
    let mut statuses = Vec::new();
    // Every definition request crashes the server; after the restart budget it gives up.
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let def = client
            .call("intel.definition", position(id, 0, 0, 3))
            .unwrap();
        statuses.push(semantic(&def)["status"].as_str().unwrap().to_string());
        let state = semantic_state(&mut client);
        if state["state"] == "failed" {
            assert!(state["crashes"].as_u64().unwrap() >= 3, "{state}");
            break;
        }
        assert!(
            Instant::now() < deadline,
            "never gave up: {state} {statuses:?}"
        );
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(statuses.iter().any(|s| s == "failed"), "{statuses:?}");
    // titand is healthy and editing works.
    client.call("system.health", Value::Null).unwrap();
    edit(&mut client, id, 0, 0, 0, (0, 0), "// ok\n");
    let notes = client.take_notifications();
    assert!(
        notes
            .iter()
            .any(|n| n["method"] == "intel/state" && n["params"]["state"] == "failed")
    );
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn hung_requests_time_out_or_are_cancelled() {
    let project = small_project();
    let mut args = fake_args(&["--hang-on", "textDocument/hover"]);
    args.extend(["--lsp-timeout-ms".to_string(), "300".to_string()]);
    let mut client = start(&args, project.path());
    let id = open(&mut client, "a.rs");
    wait_state(&mut client, "running", |s| s["state"] != "starting", 30);
    // Wait until the document is open in the server.
    let deadline = Instant::now() + Duration::from_secs(10);
    while semantic_state(&mut client)["syncedDocuments"] != 1 {
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(50));
    }
    let started = Instant::now();
    let hover = client.call("intel.hover", position(id, 0, 0, 3)).unwrap();
    assert_eq!(semantic(&hover)["status"], "timeout");
    assert!(started.elapsed() < Duration::from_secs(5));

    // Editing is not blocked while a request hangs; the client cancels it.
    let mut args = fake_args(&["--hang-on", "textDocument/hover"]);
    args.extend(["--lsp-timeout-ms".to_string(), "60000".to_string()]);
    let mut client = start(&args, project.path());
    let id = open(&mut client, "a.rs");
    let deadline = Instant::now() + Duration::from_secs(10);
    while semantic_state(&mut client)["syncedDocuments"] != 1 {
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(50));
    }
    let hover_id = client
        .send_request("intel.hover", position(id, 0, 0, 3))
        .unwrap();
    let t = Instant::now();
    edit(
        &mut client,
        id,
        0,
        0,
        0,
        (0, 0),
        "// typing while hover hangs\n",
    );
    assert!(
        t.elapsed() < Duration::from_secs(1),
        "edit waited for the language server"
    );
    client
        .send_notification("$/cancelRequest", json!({ "id": hover_id }))
        .unwrap();
    let response = client.wait_response(hover_id).unwrap();
    assert_eq!(response.error.unwrap().code, codes::REQUEST_CANCELLED);
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn answers_for_an_older_revision_are_marked_stale() {
    let project = small_project();
    let args = fake_args(&["--delay-ms", "400"]);
    let mut client = start(&args, project.path());
    let id = open(&mut client, "a.rs");
    let deadline = Instant::now() + Duration::from_secs(10);
    while semantic_state(&mut client)["syncedDocuments"] != 1 {
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(50));
    }
    let request = client
        .send_request("intel.documentSymbols", json!({ "documentId": id }))
        .unwrap();
    // The document changes while the server is computing.
    edit(&mut client, id, 0, 0, 0, (0, 0), "// newer\n");
    let response = client.wait_response(request).unwrap();
    let result = response.result.unwrap();
    let sem = semantic(&result);
    assert_eq!(sem["status"], "stale", "{result}");
    assert_eq!(sem["documentRevision"], 0);
    assert_eq!(result["currentRevision"], 1);

    // The fake server publishes diagnostics tagged with the version: current only when the
    // version is the current revision.
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let d = client
            .call("intel.diagnostics", json!({ "documentId": id }))
            .unwrap();
        let sem = semantic(&d).clone();
        if sem["documentRevision"] == 1 {
            assert_eq!(sem["status"], "current");
            break;
        }
        assert!(Instant::now() < deadline, "{d}");
        std::thread::sleep(Duration::from_millis(50));
    }
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn without_a_language_server_answers_say_so() {
    let project = small_project();
    let mut client = start(&["--no-lsp".to_string()], project.path());
    let id = open(&mut client, "a.rs");
    let def = client
        .call("intel.definition", position(id, 0, 0, 3))
        .unwrap();
    let sem = semantic(&def);
    assert_eq!(sem["status"], "unavailable");
    assert!(
        sem["error"]
            .as_str()
            .unwrap()
            .contains("no language server")
    );
    let err = client
        .call("intel.definition", position(id, 7, 0, 3))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::STALE_REVISION));
    assert!(client.shutdown().unwrap().success());
}

/// Explicit lifecycle control of the language server.
#[test]
fn language_server_can_be_stopped_and_started() {
    let project = small_project();
    let mut client = start(&fake_args(&[]), project.path());
    wait_state(
        &mut client,
        "running",
        |s| s["state"] == "indexing" || s["state"] == "ready",
        30,
    );
    let pid = semantic_state(&mut client)["pid"].as_u64().unwrap();
    let stopped = client
        .call("intel.server", json!({ "action": "stop" }))
        .unwrap();
    assert_eq!(stopped["state"], "stopped");
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let alive = std::process::Command::new("kill")
            .args(["-0", &pid.to_string()])
            .status()
            .unwrap();
        if !alive.success() {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "the stopped server is still running"
        );
        std::thread::sleep(Duration::from_millis(50));
    }
    client
        .call("intel.server", json!({ "action": "start" }))
        .unwrap();
    wait_state(
        &mut client,
        "running again",
        |s| s["state"] == "indexing" || s["state"] == "ready",
        30,
    );
    let err = client
        .call("intel.server", json!({ "action": "explode" }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::INVALID_PARAMS));
    assert!(client.shutdown().unwrap().success());
}
