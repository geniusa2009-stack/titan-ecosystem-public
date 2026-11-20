//! The headless vertical slice: drive a real `titand` process through the full editing
//! workflow over JSON-RPC and record an auditable report of every step.
//!
//! Workflow: hello → health → open workspace → list files → open document → read →
//! revision-checked multi-range edit (UTF-16 positions, Arabic + emoji) → stale edit
//! rejected → undo → redo → save → verify bytes on disk → close → reopen → verify →
//! search (buffer and disk matches) → shutdown.

use std::fs;
use std::path::Path;

use serde::Serialize;
use serde_json::{Value, json};
use titan_protocol::PROTOCOL_VERSION;
use titan_protocol::rpc::codes;

use crate::client::{Client, ClientError};

pub const ORIGINAL_MAIN: &str = "fn main() {\n    println!(\"hello\");\n}\n// مرحبا 👋 TITAN\n";
pub const EDITED_MAIN: &str =
    "fn main() {\n    println!(\"hello, TITAN\");\n}\n// مرحبا 👋 TITAN ✓\n";
const LIB_RS: &str = "pub fn helper() -> u32 {\n    42\n}\n";

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProbeStep {
    pub name: String,
    pub ok: bool,
    pub detail: Value,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProbeReport {
    pub protocol_version: u32,
    pub server: String,
    pub project: String,
    pub ok: bool,
    pub steps: Vec<ProbeStep>,
}

/// Run the vertical slice against `server` using `project` (an empty directory) as the
/// workspace. Never panics; failures are recorded in the report.
pub fn run_vertical_slice(server: &Path, project: &Path) -> ProbeReport {
    let mut steps = Vec::new();
    let outcome = run_steps(server, project, &mut steps).and_then(|()| {
        let state = tempfile::tempdir().map_err(|e| format!("state dir: {e}"))?;
        run_recovery_steps(server, project, state.path(), &mut steps)
    });
    if let Err(message) = &outcome {
        steps.push(ProbeStep {
            name: "aborted".to_string(),
            ok: false,
            detail: json!({ "error": message }),
        });
    }
    ProbeReport {
        protocol_version: PROTOCOL_VERSION,
        server: server.display().to_string(),
        project: project.display().to_string(),
        ok: outcome.is_ok(),
        steps,
    }
}

fn record(steps: &mut Vec<ProbeStep>, name: &str, detail: Value) {
    steps.push(ProbeStep {
        name: name.to_string(),
        ok: true,
        detail,
    });
}

fn ensure(condition: bool, message: impl FnOnce() -> String) -> Result<(), String> {
    if condition { Ok(()) } else { Err(message()) }
}

fn rpc(err: ClientError) -> String {
    err.to_string()
}

fn write_fixture(project: &Path) -> std::io::Result<()> {
    fs::create_dir_all(project.join("src"))?;
    fs::create_dir_all(project.join("target"))?;
    fs::write(project.join(".gitignore"), "target/\n")?;
    fs::write(project.join("src/main.rs"), ORIGINAL_MAIN)?;
    fs::write(project.join("src/lib.rs"), LIB_RS)?;
    fs::write(project.join("target/generated.rs"), "// ignored TITAN\n")?;
    Ok(())
}

fn run_steps(server: &Path, project: &Path, steps: &mut Vec<ProbeStep>) -> Result<(), String> {
    write_fixture(project).map_err(|e| format!("creating fixture project: {e}"))?;
    record(
        steps,
        "fixture",
        json!({ "files": ["src/main.rs", "src/lib.rs", "target/generated.rs (ignored)"] }),
    );

    let mut client = Client::spawn(server).map_err(rpc)?;

    let hello = client
        .call(
            "session.hello",
            json!({ "protocolVersion": PROTOCOL_VERSION, "clientName": "titan-probe",
                    "positionEncodings": ["utf-16"] }),
        )
        .map_err(rpc)?;
    ensure(hello["positionEncoding"] == "utf-16", || {
        format!("unexpected encoding in {hello}")
    })?;
    record(steps, "session.hello", hello);

    let health = client.call("system.health", Value::Null).map_err(rpc)?;
    ensure(health["status"] == "ok", || format!("unhealthy: {health}"))?;
    record(steps, "system.health", health);

    let root = client
        .call(
            "workspace.open",
            json!({ "path": project.display().to_string() }),
        )
        .map_err(rpc)?;
    record(steps, "workspace.open", root);

    let files = client
        .call("workspace.listFiles", Value::Null)
        .map_err(rpc)?;
    ensure(
        files["files"] == json!(["src/lib.rs", "src/main.rs"]),
        || format!("ignore rules not applied: {files}"),
    )?;
    record(steps, "workspace.listFiles", files);

    let info = client
        .call("document.open", json!({ "path": "src/main.rs" }))
        .map_err(rpc)?;
    let id = info["documentId"]
        .as_u64()
        .ok_or_else(|| format!("no documentId in {info}"))?;
    ensure(info["revision"] == 0, || format!("fresh revision: {info}"))?;
    record(steps, "document.open", info);

    let read = client
        .call("document.read", json!({ "documentId": id }))
        .map_err(rpc)?;
    ensure(read["text"] == ORIGINAL_MAIN, || {
        format!("read mismatch: {read}")
    })?;
    record(
        steps,
        "document.read",
        json!({ "revision": read["revision"], "matchesDisk": true }),
    );

    // Two edits in one transaction. Positions are UTF-16: on line 3, "// مرحبا 👋 TITAN"
    // is 17 code units long (the emoji counts as 2).
    let edit = client
        .call(
            "document.applyEdits",
            json!({
                "documentId": id,
                "expectedRevision": 0,
                "edits": [
                    { "range": { "start": { "line": 3, "character": 17 },
                                 "end":   { "line": 3, "character": 17 } }, "text": " ✓" },
                    { "range": { "start": { "line": 1, "character": 14 },
                                 "end":   { "line": 1, "character": 19 } }, "text": "hello, TITAN" }
                ]
            }),
        )
        .map_err(rpc)?;
    ensure(
        edit["revision"] == 1 && edit["dirty"].as_bool() == Some(true),
        || format!("edit result: {edit}"),
    )?;
    let after_edit = client
        .call("document.read", json!({ "documentId": id }))
        .map_err(rpc)?;
    ensure(after_edit["text"] == EDITED_MAIN, || {
        format!("edited text mismatch: {after_edit}")
    })?;
    record(steps, "document.applyEdits", edit);

    let stale = client.call(
        "document.applyEdits",
        json!({ "documentId": id, "expectedRevision": 0, "edits": [] }),
    );
    let stale_code = stale.as_ref().err().and_then(ClientError::code);
    ensure(stale_code == Some(codes::STALE_REVISION), || {
        format!("stale edit was not rejected: {stale:?}")
    })?;
    record(
        steps,
        "document.applyEdits (stale revision rejected)",
        json!({ "errorCode": stale_code }),
    );

    let undo = client
        .call(
            "document.undo",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .map_err(rpc)?;
    let after_undo = client
        .call("document.read", json!({ "documentId": id }))
        .map_err(rpc)?;
    ensure(
        undo["revision"] == 2
            && after_undo["text"] == ORIGINAL_MAIN
            && undo["dirty"].as_bool() == Some(false),
        || format!("undo: {undo} / {after_undo}"),
    )?;
    record(steps, "document.undo", undo);

    let redo = client
        .call(
            "document.redo",
            json!({ "documentId": id, "expectedRevision": 2 }),
        )
        .map_err(rpc)?;
    let after_redo = client
        .call("document.read", json!({ "documentId": id }))
        .map_err(rpc)?;
    ensure(
        redo["revision"] == 3 && after_redo["text"] == EDITED_MAIN,
        || format!("redo: {redo} / {after_redo}"),
    )?;
    record(steps, "document.redo", redo);

    let save = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 3 }),
        )
        .map_err(rpc)?;
    let on_disk = fs::read_to_string(project.join("src/main.rs"))
        .map_err(|e| format!("reading saved file: {e}"))?;
    ensure(
        on_disk == EDITED_MAIN && save["dirty"].as_bool() == Some(false),
        || format!("saved bytes differ: {on_disk:?}"),
    )?;
    record(
        steps,
        "document.save",
        json!({ "result": save, "diskMatches": true }),
    );

    client
        .call("document.close", json!({ "documentId": id }))
        .map_err(rpc)?;
    let reopened = client
        .call("document.open", json!({ "path": "src/main.rs" }))
        .map_err(rpc)?;
    let new_id = reopened["documentId"]
        .as_u64()
        .ok_or_else(|| format!("no documentId in {reopened}"))?;
    let reread = client
        .call("document.read", json!({ "documentId": new_id }))
        .map_err(rpc)?;
    ensure(
        reread["text"] == EDITED_MAIN && reread["revision"] == 0,
        || format!("reopen mismatch: {reread}"),
    )?;
    record(
        steps,
        "document.close + document.open (persistence)",
        json!({ "documentId": new_id, "revision": reread["revision"], "textMatches": true }),
    );

    let search = client
        .call("workspace.search", json!({ "query": "TITAN" }))
        .map_err(rpc)?;
    let expected_ranges = json!([
        { "start": { "line": 1, "character": 21 }, "end": { "line": 1, "character": 26 } },
        { "start": { "line": 3, "character": 12 }, "end": { "line": 3, "character": 17 } }
    ]);
    let ranges: Vec<Value> = search["matches"]
        .as_array()
        .map(|m| m.iter().map(|x| x["range"].clone()).collect())
        .unwrap_or_default();
    ensure(
        Value::Array(ranges) == expected_ranges && search["matches"][0]["source"] == "buffer",
        || format!("search: {search}"),
    )?;
    record(steps, "workspace.search (open buffer)", search);

    let disk_search = client
        .call("workspace.search", json!({ "query": "helper" }))
        .map_err(rpc)?;
    ensure(
        disk_search["matches"][0]["path"] == "src/lib.rs"
            && disk_search["matches"][0]["source"] == "disk",
        || format!("disk search: {disk_search}"),
    )?;
    record(steps, "workspace.search (disk)", disk_search);

    // Indexing runs in the background; wait (without blocking the server) until idle.
    let status = client
        .call(
            "index.status",
            json!({ "waitForIdle": true, "timeoutMs": 60_000 }),
        )
        .map_err(rpc)?;
    ensure(status["idle"].as_bool() == Some(true), || {
        format!("index did not become idle: {status}")
    })?;
    record(steps, "index.status (wait for background indexing)", status);

    let symbols = client
        .call("index.symbols", json!({ "query": "helper" }))
        .map_err(rpc)?;
    ensure(
        symbols["evidence"] == "syntax:tree-sitter-rust"
            && symbols["matches"][0]["path"] == "src/lib.rs"
            && symbols["matches"][0]["kind"] == "function",
        || format!("index.symbols: {symbols}"),
    )?;
    record(steps, "index.symbols (tree-sitter, syntax only)", symbols);

    let status = client.shutdown().map_err(rpc)?;
    ensure(status.success(), || format!("titand exited with {status}"))?;
    record(
        steps,
        "session.shutdown",
        json!({ "exitCode": status.code() }),
    );
    Ok(())
}

pub const RECOVERED_LIB: &str = "// unsaved 2\n// unsaved 1\npub fn helper() -> u32 {\n    42\n}\n";

/// Crash recovery: edit without saving, kill `titand` (SIGKILL), restart it on the same
/// state directory, list and restore the journal, then save.
fn run_recovery_steps(
    server: &Path,
    project: &Path,
    state: &Path,
    steps: &mut Vec<ProbeStep>,
) -> Result<(), String> {
    let args = ["--state-dir".to_string(), state.display().to_string()];
    let hello = json!({ "protocolVersion": PROTOCOL_VERSION, "clientName": "titan-probe" });
    let open = json!({ "path": project.display().to_string() });

    let mut first = Client::spawn_with_args(server, &args).map_err(rpc)?;
    first.call("session.hello", hello.clone()).map_err(rpc)?;
    first.call("workspace.open", open.clone()).map_err(rpc)?;
    let info = first
        .call("document.open", json!({ "path": "src/lib.rs" }))
        .map_err(rpc)?;
    let id = info["documentId"]
        .as_u64()
        .ok_or_else(|| format!("no documentId in {info}"))?;
    for (revision, text) in [(0, "// unsaved 1\n"), (1, "// unsaved 2\n")] {
        first
            .call(
                "document.applyEdits",
                json!({ "documentId": id, "expectedRevision": revision, "edits": [
                    { "range": { "start": { "line": 0, "character": 0 },
                                 "end": { "line": 0, "character": 0 } }, "text": text } ] }),
            )
            .map_err(rpc)?;
    }
    let status = first.kill().map_err(rpc)?;
    let disk = fs::read_to_string(project.join("src/lib.rs"))
        .map_err(|e| format!("reading lib.rs: {e}"))?;
    ensure(disk == LIB_RS, || {
        format!("disk changed before restore: {disk:?}")
    })?;
    record(
        steps,
        "recovery: 2 unsaved edits, then titand killed",
        json!({ "killed": !status.success(), "diskUnchanged": true }),
    );

    let mut second = Client::spawn_with_args(server, &args).map_err(rpc)?;
    second.call("session.hello", hello).map_err(rpc)?;
    second.call("workspace.open", open).map_err(rpc)?;
    let list = second.call("recovery.list", Value::Null).map_err(rpc)?;
    let journal = &list["journals"][0];
    ensure(
        list["journals"].as_array().map(Vec::len) == Some(1)
            && journal["state"] == "recoverable"
            && journal["recoveredRevision"] == 2
            && journal["path"] == "src/lib.rs",
        || format!("recovery.list: {list}"),
    )?;
    let journal_id = journal["journalId"].clone();
    record(steps, "recovery.list after restart", list.clone());

    let restored = second
        .call("recovery.restore", json!({ "journalId": journal_id }))
        .map_err(rpc)?;
    let doc_id = restored["document"]["documentId"]
        .as_u64()
        .ok_or_else(|| format!("no documentId in {restored}"))?;
    let read = second
        .call("document.read", json!({ "documentId": doc_id }))
        .map_err(rpc)?;
    ensure(
        read["text"] == RECOVERED_LIB && read["dirty"].as_bool() == Some(true),
        || format!("restored text: {read}"),
    )?;
    record(steps, "recovery.restore", restored);

    let revision = read["revision"].as_u64().unwrap_or(0);
    second
        .call(
            "document.save",
            json!({ "documentId": doc_id, "expectedRevision": revision }),
        )
        .map_err(rpc)?;
    let disk = fs::read_to_string(project.join("src/lib.rs"))
        .map_err(|e| format!("reading lib.rs: {e}"))?;
    let after = second.call("recovery.list", Value::Null).map_err(rpc)?;
    ensure(
        disk == RECOVERED_LIB && after["journals"].as_array().is_some_and(Vec::is_empty),
        || format!("after save: disk {disk:?}, journals {after}"),
    )?;
    record(
        steps,
        "recovery: saved restored text, journals cleaned up",
        json!({ "diskMatches": true, "journalsLeft": 0 }),
    );
    let status = second.shutdown().map_err(rpc)?;
    ensure(status.success(), || format!("titand exited with {status}"))?;
    Ok(())
}
