//! End-to-end tests: spawn the real `titand` binary and talk to it over stdio JSON-RPC.

use std::fs;
use std::path::Path;

use serde_json::{Value, json};
use titan_protocol::rpc::codes;
use titan_server::client::{Client, into_result};
use titan_server::probe::{EDITED_MAIN, RECOVERED_LIB, run_vertical_slice};

fn titand() -> &'static Path {
    Path::new(env!("CARGO_BIN_EXE_titand"))
}

fn started_with(args: &[&str], hello: Value) -> Client {
    let mut client = Client::spawn_with_args(titand(), args).expect("spawn titand");
    client.call("session.hello", hello).expect("hello");
    client
}

fn started(encodings: Value) -> Client {
    started_with(
        &["--no-recovery"],
        json!({ "protocolVersion": 1, "positionEncodings": encodings }),
    )
}

fn open_workspace(client: &mut Client, dir: &Path) {
    client
        .call(
            "workspace.open",
            json!({ "path": dir.display().to_string() }),
        )
        .unwrap();
}

fn open_document(client: &mut Client, path: &str) -> u64 {
    client
        .call("document.open", json!({ "path": path }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap()
}

fn insert(
    client: &mut Client,
    id: u64,
    revision: u64,
    line: usize,
    character: usize,
    text: &str,
) -> Value {
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": revision, "edits": [
                { "range": { "start": { "line": line, "character": character },
                             "end": { "line": line, "character": character } }, "text": text } ] }),
        )
        .unwrap()
}

/// A workspace big enough that scanning and searching it take measurable time.
fn big_workspace(dir: &Path, files: usize) {
    fs::create_dir_all(dir.join("src")).unwrap();
    let body: String = (0..1_500)
        .map(|i| format!("pub fn function_{i}(x: u64) -> u64 {{ x + {i} }}\n"))
        .collect();
    for f in 0..files {
        fs::write(dir.join(format!("src/m{f}.rs")), &body).unwrap();
    }
}

#[test]
fn vertical_slice_and_recovery_pass() {
    let project = tempfile::tempdir().unwrap();
    let report = run_vertical_slice(titand(), project.path());
    let summary = serde_json::to_string_pretty(&report).unwrap();
    assert!(report.ok, "probe failed:\n{summary}");
    assert!(report.steps.iter().all(|s| s.ok));
    assert_eq!(
        fs::read_to_string(project.path().join("src/main.rs")).unwrap(),
        EDITED_MAIN
    );
    assert_eq!(
        fs::read_to_string(project.path().join("src/lib.rs")).unwrap(),
        RECOVERED_LIB
    );
}

#[test]
fn utf8_positions_and_invalid_positions() {
    let project = tempfile::tempdir().unwrap();
    fs::write(project.path().join("a.txt"), "سلام x\n").unwrap();
    let mut client = started(json!(["utf-8"]));
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "a.txt");

    // Byte 9 is after "سلام " (8 + 1 bytes).
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": 0, "edits": [
                { "range": { "start": { "line": 0, "character": 9 },
                             "end": { "line": 0, "character": 10 } }, "text": "y" } ] }),
        )
        .unwrap();
    let text = client
        .call("document.read", json!({ "documentId": id }))
        .unwrap()["text"]
        .clone();
    assert_eq!(text, "سلام y\n");

    // Byte 1 is inside the first Arabic letter; line 7 does not exist.
    for (line, character) in [(0, 1), (7, 0)] {
        let err = client
            .call(
                "document.applyEdits",
                json!({ "documentId": id, "expectedRevision": 1, "edits": [
                    { "range": { "start": { "line": line, "character": character },
                                 "end": { "line": line, "character": character } },
                      "text": "!" } ] }),
            )
            .unwrap_err();
        assert_eq!(err.code(), Some(codes::INVALID_POSITION));
    }

    let err = client
        .call("document.close", json!({ "documentId": id }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::UNSAVED_CHANGES));
    client
        .call(
            "document.close",
            json!({ "documentId": id, "discard": true }),
        )
        .unwrap();
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn external_modification_conflict() {
    let project = tempfile::tempdir().unwrap();
    let file = project.path().join("notes.md");
    fs::write(&file, "draft\n").unwrap();
    let mut client = started(json!([]));
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "notes.md");
    insert(&mut client, id, 0, 0, 5, " v2");

    fs::write(&file, "edited elsewhere\n").unwrap();
    let check = client
        .call("document.checkDisk", json!({ "documentId": id }))
        .unwrap();
    assert_eq!(check["changedOnDisk"], true);

    let err = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::EXTERNAL_MODIFICATION));
    assert_eq!(fs::read_to_string(&file).unwrap(), "edited elsewhere\n");

    client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1, "force": true }),
        )
        .unwrap();
    assert_eq!(fs::read_to_string(&file).unwrap(), "draft v2\n");
}

#[test]
fn paths_outside_the_workspace_are_rejected() {
    let project = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let secret = outside.path().join("secret.txt");
    fs::write(&secret, "top secret").unwrap();
    let mut client = started(json!([]));
    open_workspace(&mut client, project.path());

    let relative = format!(
        "../{}/secret.txt",
        outside.path().file_name().unwrap().to_string_lossy()
    );
    for path in [secret.display().to_string(), relative] {
        let err = client
            .call("document.open", json!({ "path": path }))
            .unwrap_err();
        assert_eq!(err.code(), Some(codes::PATH_OUTSIDE_WORKSPACE));
    }
    let err = client
        .call("document.open", json!({ "path": "missing.rs" }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::IO_ERROR));
}

#[test]
fn malformed_input_over_the_wire() {
    let mut client = Client::spawn_with_args(titand(), &["--no-recovery"]).unwrap();
    // Invalid JSON: answered with a parse error, connection stays usable.
    client.send_raw_body(b"{ definitely not json").unwrap();
    let reply = client.read_response().unwrap();
    assert_eq!(reply.error.unwrap().code, codes::PARSE_ERROR);
    let hello = client
        .call("session.hello", json!({ "protocolVersion": 1 }))
        .unwrap();
    assert_eq!(hello["serverName"], "titand");

    // A broken header: final error, then the server closes the stream and exits cleanly.
    client
        .send_raw_bytes(b"Content-Length: not-a-number\r\n\r\n")
        .unwrap();
    let reply = client.read_response().unwrap();
    assert_eq!(reply.error.unwrap().code, codes::INVALID_REQUEST);
    assert!(client.read_response().is_err());
    assert!(client.wait().unwrap().success());
}

#[test]
fn background_index_follows_unsaved_edits() {
    let project = tempfile::tempdir().unwrap();
    fs::create_dir_all(project.path().join("src")).unwrap();
    fs::write(project.path().join("src/lib.rs"), "pub fn helper() {}\n").unwrap();
    fs::write(project.path().join("src/main.rs"), "fn main() {}\n").unwrap();
    let mut client = started(json!(["utf-16"]));
    open_workspace(&mut client, project.path());

    let status = client
        .call("index.status", json!({ "waitForIdle": true }))
        .unwrap();
    assert_eq!(status["idle"], true);
    assert_eq!(status["evidence"], "syntax:tree-sitter-rust");
    assert_eq!(status["filesIndexed"], 2);
    assert_eq!(status["reindexed"], 2);

    let found = client
        .call("index.symbols", json!({ "query": "helper" }))
        .unwrap();
    assert_eq!(found["matches"][0]["path"], "src/lib.rs");
    assert_eq!(found["matches"][0]["kind"], "function");
    assert_eq!(found["matches"][0]["source"], "disk");

    // Unsaved edits are parsed in the background, incrementally, at their revision.
    let id = open_document(&mut client, "src/main.rs");
    // Let the first (full) parse of the buffer publish, so later parses can reuse its tree.
    client
        .call("index.status", json!({ "waitForIdle": true }))
        .unwrap();
    insert(&mut client, id, 0, 1, 0, "fn added_by_edit() {}\n");
    insert(&mut client, id, 1, 2, 0, "fn second() {}\n");
    let status = client
        .call("index.status", json!({ "waitForIdle": true }))
        .unwrap();
    assert_eq!(status["idle"], true);
    assert!(
        status["incrementalParses"].as_u64().unwrap() >= 1,
        "{status}"
    );

    let found = client
        .call("index.symbols", json!({ "query": "added_by_edit" }))
        .unwrap();
    assert_eq!(found["matches"].as_array().unwrap().len(), 1);
    assert_eq!(found["matches"][0]["source"], "buffer");
    assert_eq!(found["matches"][0]["revision"], 2);
    assert_eq!(
        found["matches"][0]["range"],
        json!({ "start": { "line": 1, "character": 3 }, "end": { "line": 1, "character": 16 } })
    );

    // A rescan re-parses nothing that did not change on disk.
    client.call("index.rescan", Value::Null).unwrap();
    let status = client
        .call("index.status", json!({ "waitForIdle": true }))
        .unwrap();
    assert_eq!(status["reindexed"], 0, "{status}");
    assert_eq!(status["unchanged"], 1, "{status}");
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn deferred_requests_do_not_block_and_can_be_cancelled() {
    let project = tempfile::tempdir().unwrap();
    big_workspace(project.path(), 150);
    let mut client = started(json!([]));
    open_workspace(&mut client, project.path());

    // index.status waits for the (multi-second) scan without blocking the loop...
    let waiting = client
        .send_request(
            "index.status",
            json!({ "waitForIdle": true, "timeoutMs": 60_000 }),
        )
        .unwrap();
    // ...so a later request is answered first: responses arrive out of order.
    let health = client.send_request("system.health", Value::Null).unwrap();
    let first = client.read_response().unwrap();
    assert_eq!(
        first.id, health,
        "health should overtake the waiting request"
    );

    client
        .send_notification("$/cancelRequest", json!({ "id": waiting }))
        .unwrap();
    let cancelled = client.wait_response(waiting).unwrap();
    assert_eq!(cancelled.error.unwrap().code, codes::REQUEST_CANCELLED);

    // A background search can be cancelled too; it ends with a response either way.
    let search = client
        .send_request("workspace.search", json!({ "query": "function_1499" }))
        .unwrap();
    client
        .send_notification("$/cancelRequest", json!({ "id": search }))
        .unwrap();
    let response = client.wait_response(search).unwrap();
    match into_result(response) {
        Ok(result) => assert!(result["matches"].is_array()),
        Err(err) => assert_eq!(err.code(), Some(codes::REQUEST_CANCELLED)),
    }

    let metrics = client.call("system.metrics", Value::Null).unwrap();
    assert!(
        metrics["requests"]["cancelled"].as_u64().unwrap() >= 1,
        "{metrics}"
    );
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn background_requests_time_out() {
    let project = tempfile::tempdir().unwrap();
    big_workspace(project.path(), 150);
    let mut client = started_with(
        &["--no-recovery"],
        json!({ "protocolVersion": 1, "requestTimeoutMs": 1 }),
    );
    open_workspace(&mut client, project.path());
    let err = client
        .call("workspace.search", json!({ "query": "function_1499" }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::REQUEST_TIMEOUT));
    // Editing still works while background work is failing or running.
    assert_eq!(
        client.call("system.health", Value::Null).unwrap()["status"],
        "ok"
    );
}

#[test]
fn notifications_report_changes_and_syntax_diagnostics() {
    let project = tempfile::tempdir().unwrap();
    fs::write(project.path().join("lib.rs"), "fn ok() {}\n").unwrap();
    let mut client = started_with(
        &["--no-recovery"],
        json!({ "protocolVersion": 1, "capabilities": {
            "changeNotifications": true, "indexProgress": true, "syntaxDiagnostics": true } }),
    );
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "lib.rs");
    let clean = client
        .wait_notification("document/syntaxDiagnostics")
        .unwrap();
    assert_eq!(clean["revision"], 0);
    assert_eq!(clean["diagnostics"], json!([]));

    insert(&mut client, id, 0, 1, 0, "fn broken( {\n");
    let change = client.wait_notification("document/didChange").unwrap();
    assert_eq!(change["documentId"], id);
    assert_eq!(
        (
            change["previousRevision"].clone(),
            change["revision"].clone()
        ),
        (json!(0), json!(1))
    );
    assert_eq!(change["kind"], "edit");
    assert_eq!(change["changes"][0]["text"], "fn broken( {\n");

    let broken = client
        .wait_notification("document/syntaxDiagnostics")
        .unwrap();
    assert_eq!(broken["revision"], 1);
    assert_eq!(broken["evidence"], "syntax:tree-sitter-rust");
    assert!(
        !broken["diagnostics"].as_array().unwrap().is_empty(),
        "{broken}"
    );

    client
        .call(
            "document.undo",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap();
    let undo = client.wait_notification("document/didChange").unwrap();
    assert_eq!(undo["kind"], "undo");
    assert_eq!(undo["dirty"], false);
    let progress = client.wait_notification("index/progress").unwrap();
    assert!(progress["phase"].is_string());
}

#[test]
fn crash_recovery_with_external_conflict() {
    let project = tempfile::tempdir().unwrap();
    let state = tempfile::tempdir().unwrap();
    let file = project.path().join("notes.txt");
    fs::write(&file, "original\n").unwrap();
    let state_arg = state.path().display().to_string();
    let args = ["--state-dir", state_arg.as_str()];
    let hello = json!({ "protocolVersion": 1 });

    let mut first = started_with(&args, hello.clone());
    open_workspace(&mut first, project.path());
    let id = open_document(&mut first, "notes.txt");
    insert(&mut first, id, 0, 0, 0, "mine: ");
    first.kill().unwrap();

    // Someone else changes the file before the editor comes back.
    fs::write(&file, "theirs\n").unwrap();

    let mut second = started_with(&args, hello);
    open_workspace(&mut second, project.path());
    let list = second.call("recovery.list", Value::Null).unwrap();
    assert_eq!(list["journals"].as_array().unwrap().len(), 1, "{list}");
    let journal = list["journals"][0].clone();
    assert_eq!(journal["state"], "conflict");
    assert_eq!(journal["disk"], "changedSinceBase");
    let journal_id = journal["journalId"].clone();

    let info = second
        .call("document.open", json!({ "path": "notes.txt" }))
        .unwrap();
    assert_eq!(info["pendingRecovery"], json!([journal_id]));
    second
        .call(
            "document.close",
            json!({ "documentId": info["documentId"] }),
        )
        .unwrap();

    let err = second
        .call("recovery.restore", json!({ "journalId": journal_id }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::RECOVERY_CONFLICT));
    assert_eq!(fs::read_to_string(&file).unwrap(), "theirs\n");

    let restored = second
        .call(
            "recovery.restore",
            json!({ "journalId": journal_id, "acceptConflict": true }),
        )
        .unwrap();
    assert_eq!(restored["conflict"], true);
    let doc = restored["document"]["documentId"].clone();
    let read = second
        .call("document.read", json!({ "documentId": doc }))
        .unwrap();
    assert_eq!(read["text"], "mine: original\n");
    assert_eq!(read["dirty"], true);
    // Nothing was written to disk by recovery.
    assert_eq!(fs::read_to_string(&file).unwrap(), "theirs\n");

    // Unknown and malicious ids are rejected.
    for bad in ["../../etc/passwd", "nope.tjournal"] {
        let err = second
            .call("recovery.restore", json!({ "journalId": bad }))
            .unwrap_err();
        assert_eq!(err.code(), Some(codes::RECOVERY_UNAVAILABLE));
    }
}
