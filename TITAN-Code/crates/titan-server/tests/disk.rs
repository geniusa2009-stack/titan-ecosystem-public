//! End-to-end tests for disk reconciliation, conflict-safe saves and recovery durability,
//! against the real `titand` binary.

use std::fs;
use std::path::Path;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_protocol::rpc::codes;
use titan_server::client::Client;

fn titand() -> &'static Path {
    Path::new(env!("CARGO_BIN_EXE_titand"))
}

fn hello() -> Value {
    json!({ "protocolVersion": 1, "capabilities": {
        "changeNotifications": true, "diskNotifications": true } })
}

fn start(args: &[&str]) -> Client {
    let mut client = Client::spawn_with_args(titand(), args).expect("spawn titand");
    client.call("session.hello", hello()).expect("hello");
    client
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

fn insert(client: &mut Client, id: u64, revision: u64, line: usize, ch: usize, text: &str) {
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": revision, "edits": [
                { "range": { "start": { "line": line, "character": ch },
                             "end": { "line": line, "character": ch } }, "text": text } ] }),
        )
        .unwrap();
}

fn read(client: &mut Client, id: u64) -> Value {
    client
        .call("document.read", json!({ "documentId": id }))
        .unwrap()
}

fn info(client: &mut Client, path: &str) -> Value {
    client
        .call("document.open", json!({ "path": path }))
        .unwrap()
}

/// `document.checkDisk`, then the `document/diskChanged` it caused (if any).
fn check_disk(client: &mut Client, id: u64) -> (Value, Option<Value>) {
    let result = client
        .call("document.checkDisk", json!({ "documentId": id }))
        .unwrap();
    let notes = client.take_notifications();
    let changed = notes
        .into_iter()
        .rev()
        .find(|n| n["method"] == "document/diskChanged")
        .map(|n| n["params"].clone());
    (result, changed)
}

#[test]
fn dirty_conflict_is_reported_kept_and_resolved() {
    let project = tempfile::tempdir().unwrap();
    let file = project.path().join("main.rs");
    fs::write(&file, "fn main() {}\n").unwrap();
    let mut client = start(&["--no-recovery"]);
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "main.rs");
    insert(&mut client, id, 0, 0, 0, "// mine\n");

    // 1. External modification while the buffer is dirty.
    fs::write(&file, "fn main() { theirs(); }\n").unwrap();
    let (check, changed) = check_disk(&mut client, id);
    assert_eq!(check["changedOnDisk"], true);
    assert_eq!(check["disk"], "modified");
    let changed = changed.expect("document/diskChanged");
    assert_eq!(changed["kind"], "modified");
    assert_eq!(changed["action"], "conflict");
    assert_eq!(changed["dirty"], true);
    assert_eq!(changed["cause"], "checkDisk");
    let seen_hash = changed["diskHash"].as_str().unwrap().to_string();
    assert_eq!(seen_hash.len(), 64);

    // 2. The unsaved buffer is preserved; the save is refused with an explicit status.
    assert_eq!(read(&mut client, id)["text"], "// mine\nfn main() {}\n");
    assert_eq!(info(&mut client, "main.rs")["disk"], "modified");
    let err = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::EXTERNAL_MODIFICATION));
    assert_eq!(
        fs::read_to_string(&file).unwrap(),
        "fn main() { theirs(); }\n"
    );

    // 3. A stale resolution (the disk changed again and the client saw it) is refused.
    fs::write(&file, "fn main() { theirs_v2(); }\n").unwrap();
    let (_, changed) = check_disk(&mut client, id);
    let newest = changed.expect("second diskChanged")["diskHash"]
        .as_str()
        .unwrap()
        .to_string();
    assert_ne!(newest, seen_hash);
    let err = client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 1,
                    "resolution": "keepBuffer", "diskHash": seen_hash }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::DISK_CHANGED_AGAIN));

    // 4. Explicit resolution: keep the buffer over the version the user saw, then save.
    let resolved = client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 1,
                    "resolution": "keepBuffer", "diskHash": newest }),
        )
        .unwrap();
    assert_eq!(resolved["reloaded"], false);
    assert_eq!(resolved["document"]["disk"], "inSync");
    assert_eq!(resolved["document"]["dirty"], true);
    let saved = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap();
    assert!(
        ["exchange", "rename"].contains(&saved["strategy"].as_str().unwrap()),
        "{saved}"
    );
    // 5. The bytes on disk are exactly the buffer.
    assert_eq!(fs::read(&file).unwrap(), b"// mine\nfn main() {}\n");

    // Reload resolution: discard the buffer for the disk version.
    insert(&mut client, id, 1, 0, 0, "// more\n");
    fs::write(&file, "pub fn other() {}\n").unwrap();
    let (_, changed) = check_disk(&mut client, id);
    assert_eq!(changed.unwrap()["action"], "conflict");
    let resolved = client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 2, "resolution": "reloadFromDisk" }),
        )
        .unwrap();
    assert_eq!(resolved["reloaded"], true);
    assert_eq!(resolved["document"]["dirty"], false);
    assert_eq!(resolved["document"]["revision"], 3);
    assert_eq!(read(&mut client, id)["text"], "pub fn other() {}\n");
    // Undo brings the discarded buffer back (nothing is lost by a reload).
    client
        .call(
            "document.undo",
            json!({ "documentId": id, "expectedRevision": 3 }),
        )
        .unwrap();
    assert_eq!(
        read(&mut client, id)["text"],
        "// more\n// mine\nfn main() {}\n"
    );
    assert_eq!(fs::read(&file).unwrap(), b"pub fn other() {}\n");

    let err = client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 4, "resolution": "keepBuffer" }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::NO_CONFLICT));
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn clean_buffers_reload_and_deleted_files_keep_the_buffer() {
    let project = tempfile::tempdir().unwrap();
    let file = project.path().join("notes.txt");
    fs::write(&file, "v1\n").unwrap();
    let mut client = start(&["--no-recovery"]);
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "notes.txt");

    // Clean buffer + external change: reloaded, reported, undoable.
    fs::write(&file, "v2 from elsewhere\n").unwrap();
    let (check, changed) = check_disk(&mut client, id);
    assert_eq!(check["changedOnDisk"], false);
    let changed = changed.unwrap();
    assert_eq!(changed["action"], "reloaded");
    assert_eq!(changed["dirty"], false);
    let text = read(&mut client, id);
    assert_eq!(text["text"], "v2 from elsewhere\n");
    assert_eq!(text["dirty"], false);
    assert_eq!(text["revision"], 1);

    // Deleted: the buffer stays open, nothing is written.
    fs::remove_file(&file).unwrap();
    let (check, changed) = check_disk(&mut client, id);
    assert_eq!(check["disk"], "deleted");
    let changed = changed.unwrap();
    assert_eq!(changed["kind"], "deleted");
    assert_eq!(changed["action"], "none");
    assert!(changed.get("diskHash").is_none());
    insert(&mut client, id, 1, 0, 0, "kept: ");
    let err = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 2 }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::EXTERNAL_MODIFICATION));
    assert_eq!(err_kind(&err), "deleted");
    assert!(!file.exists());

    // Keep the buffer: the save recreates the file (never clobbering a new one).
    client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 2, "resolution": "keepBuffer" }),
        )
        .unwrap();
    let saved = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 2 }),
        )
        .unwrap();
    assert_eq!(saved["strategy"], "createNew");
    assert_eq!(fs::read(&file).unwrap(), b"kept: v2 from elsewhere\n");

    // The file comes back with the expected content: reported as restored.
    let (check, _) = check_disk(&mut client, id);
    assert_eq!(check["disk"], "inSync");
    assert!(client.shutdown().unwrap().success());
}

fn err_kind(err: &titan_server::client::ClientError) -> String {
    match err {
        titan_server::client::ClientError::Rpc(e) => e
            .data
            .as_ref()
            .and_then(|d| d["kind"].as_str())
            .unwrap_or_default()
            .to_string(),
        other => panic!("not an RPC error: {other:?}"),
    }
}

/// Three crashes in a row, each followed by a restore and more edits: every acknowledged
/// edit survives. Then two restore requests for the same journal race: exactly one wins.
#[test]
fn repeated_crashes_and_concurrent_restores() {
    let project = tempfile::tempdir().unwrap();
    let state = tempfile::tempdir().unwrap();
    let file = project.path().join("lib.rs");
    fs::write(&file, "// base\n").unwrap();
    let state_arg = state.path().display().to_string();
    let args = ["--state-dir", state_arg.as_str()];

    let mut expected = "// base\n".to_string();
    let mut client = start(&args);
    open_workspace(&mut client, project.path());
    let mut id = open_document(&mut client, "lib.rs");
    let mut revision = 0;
    for round in 0..3 {
        let line = format!("// round {round}\n");
        insert(&mut client, id, revision, 0, 0, &line);
        expected.insert_str(0, &line);
        client.kill().unwrap();

        client = start(&args);
        open_workspace(&mut client, project.path());
        let list = client.call("recovery.list", Value::Null).unwrap();
        let journals = list["journals"].as_array().unwrap();
        assert_eq!(journals.len(), 1, "round {round}: {list}");
        assert_eq!(journals[0]["state"], "recoverable");
        let journal_id = journals[0]["journalId"].clone();
        let restored = client
            .call("recovery.restore", json!({ "journalId": journal_id }))
            .unwrap();
        assert_eq!(restored["partial"], false);
        id = restored["document"]["documentId"].as_u64().unwrap();
        revision = restored["document"]["revision"].as_u64().unwrap();
        assert_eq!(
            read(&mut client, id)["text"],
            expected.as_str(),
            "round {round}"
        );
    }
    assert_eq!(
        fs::read_to_string(&file).unwrap(),
        "// base\n",
        "never written"
    );

    // Crash once more, then race two restores of the same journal.
    client.kill().unwrap();
    let mut client = start(&args);
    open_workspace(&mut client, project.path());
    let list = client.call("recovery.list", Value::Null).unwrap();
    let journal_id = list["journals"][0]["journalId"].clone();
    let a = client
        .send_request("recovery.restore", json!({ "journalId": journal_id }))
        .unwrap();
    let b = client
        .send_request("recovery.restore", json!({ "journalId": journal_id }))
        .unwrap();
    let ra = client.wait_response(a).unwrap();
    let rb = client.wait_response(b).unwrap();
    let wins = [&ra, &rb].iter().filter(|r| r.error.is_none()).count();
    assert_eq!(wins, 1, "{ra:?} / {rb:?}");
    let doc = [&ra, &rb].iter().find_map(|r| r.result.clone()).unwrap()["document"]["documentId"]
        .as_u64()
        .unwrap();
    assert_eq!(read(&mut client, doc)["text"], expected.as_str());
    let list = client.call("recovery.list", Value::Null).unwrap();
    assert!(list["journals"].as_array().unwrap().is_empty(), "{list}");
    assert!(client.shutdown().unwrap().success());
}

/// The journal directory fills up: editing continues, the client is told recovery is
/// degraded, and once space is available again a fresh journal protects the edits.
#[test]
#[ignore = "needs TITAN_TINY_FS pointing at a small (e.g. 256 KiB) file system; CI provides it"]
fn journal_storage_exhaustion_degrades_and_recovers() {
    let tiny = std::env::var_os("TITAN_TINY_FS")
        .expect("set TITAN_TINY_FS to a directory on a small file system");
    let state = tempfile::tempdir_in(&tiny).unwrap();
    let project = tempfile::tempdir().unwrap();
    fs::write(project.path().join("big.txt"), "start\n").unwrap();
    // Fill the small file system almost completely.
    let filler = state.path().join("filler");
    {
        use std::io::Write;
        let mut f = fs::File::create(&filler).unwrap();
        let block = vec![b'f'; 4096];
        while f.write_all(&block).is_ok() {}
        let _ = f.sync_all();
    }
    let state_arg = state.path().display().to_string();
    let args = ["--state-dir", state_arg.as_str()];
    let mut client = start(&args);
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "big.txt");
    let chunk = "y".repeat(8 * 1024);
    let mut revision = 0;
    let mut degraded = None;
    for _ in 0..8 {
        insert(&mut client, id, revision, 0, 0, &chunk);
        revision += 1;
        if let Some(n) = client
            .take_notifications()
            .into_iter()
            .find(|n| n["method"] == "recovery/degraded")
        {
            degraded = Some(n["params"].clone());
            break;
        }
    }
    let degraded = degraded.expect("recovery/degraded after the disk filled up");
    assert_eq!(degraded["documentId"], id);
    assert_eq!(degraded["retrying"], true);
    assert_eq!(info(&mut client, "big.txt")["recovery"], "failed");
    // Editing is unaffected.
    insert(&mut client, id, revision, 0, 0, "still editing\n");
    revision += 1;

    // Space comes back; after the retry delay the next edit starts a fresh journal.
    fs::remove_file(&filler).unwrap();
    std::thread::sleep(Duration::from_millis(5_200));
    insert(&mut client, id, revision, 0, 0, "protected again\n");
    assert_eq!(info(&mut client, "big.txt")["recovery"], "journaling");
    let expected = read(&mut client, id)["text"].as_str().unwrap().to_string();
    client.kill().unwrap();

    let mut client = start(&args);
    open_workspace(&mut client, project.path());
    let deadline = Instant::now() + Duration::from_secs(10);
    let list = loop {
        let list = client.call("recovery.list", Value::Null).unwrap();
        if !list["journals"].as_array().unwrap().is_empty() || Instant::now() > deadline {
            break list;
        }
    };
    let journals = list["journals"].as_array().unwrap();
    assert_eq!(
        journals.len(),
        1,
        "the failed journal was superseded: {list}"
    );
    assert_eq!(journals[0]["state"], "recoverable");
    let restored = client
        .call(
            "recovery.restore",
            json!({ "journalId": journals[0]["journalId"] }),
        )
        .unwrap();
    let doc = restored["document"]["documentId"].as_u64().unwrap();
    assert_eq!(read(&mut client, doc)["text"], expected.as_str());
}

/// A program holding the file open from before a save keeps appending: titand preserves the
/// late bytes and reports them; the saved file stays exactly what was saved.
#[cfg(unix)]
#[test]
fn late_write_through_an_old_descriptor_is_preserved_and_reported() {
    use std::io::{BufRead, BufReader, Write as _};
    use std::process::{Command, Stdio};
    let project = tempfile::tempdir().unwrap();
    let state = tempfile::tempdir().unwrap();
    let file = project.path().join("app.log");
    fs::write(&file, "line 1\n").unwrap();
    let mut writer = Command::new("sh")
        .arg("-c")
        .arg(r#"exec 3>>"$1"; echo ready; read x; printf 'late line\n' >&3; echo done; read y"#)
        .arg("sh")
        .arg(&file)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut w_in = writer.stdin.take().unwrap();
    let mut w_out = BufReader::new(writer.stdout.take().unwrap());
    let mut line = String::new();
    w_out.read_line(&mut line).unwrap();
    assert_eq!(line.trim(), "ready");

    let state_arg = state.path().display().to_string();
    let mut client = start(&["--state-dir", state_arg.as_str(), "--no-watch"]);
    open_workspace(&mut client, project.path());
    let id = open_document(&mut client, "app.log");
    insert(&mut client, id, 0, 1, 0, "saved line\n");
    client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap();
    writeln!(w_in, "go").unwrap();
    line.clear();
    w_out.read_line(&mut line).unwrap();
    assert_eq!(line.trim(), "done");

    let deadline = Instant::now() + Duration::from_secs(10);
    let note = loop {
        client.call("system.health", Value::Null).unwrap();
        if let Some(n) = client
            .take_notifications()
            .into_iter()
            .find(|n| n["method"] == "document/lateWrite")
        {
            break n["params"].clone();
        }
        assert!(Instant::now() < deadline, "no document/lateWrite");
        std::thread::sleep(Duration::from_millis(50));
    };
    eprintln!("late write reported: {note}");
    assert_eq!(note["documentId"], id);
    let preserved = note["preservedCopy"].as_str().expect("preserved copy");
    assert!(
        preserved.starts_with(&state_arg),
        "kept in the state dir: {preserved}"
    );
    assert_eq!(fs::read(preserved).unwrap(), b"line 1\nlate line\n");
    assert_eq!(fs::read(&file).unwrap(), b"line 1\nsaved line\n");
    let metrics = client.call("system.metrics", Value::Null).unwrap();
    assert_eq!(metrics["disk"]["lateWrites"], 1);
    drop(w_in);
    let _ = writer.kill();
    let _ = writer.wait();
    assert!(client.shutdown().unwrap().success());
}

/// titand is killed at arbitrary moments while it saves a large document repeatedly: the
/// file is always one complete version, and the unsaved edits are recoverable.
#[test]
fn killed_during_saves_leaves_a_complete_file_and_a_recoverable_journal() {
    let project = tempfile::tempdir().unwrap();
    let state = tempfile::tempdir().unwrap();
    let file = project.path().join("big.txt");
    let line = "0123456789abcdefghijklmnopqrstuvwxyz\n";
    let base: String = line.repeat(60_000); // ~2.2 MB
    fs::write(&file, &base).unwrap();
    let state_arg = state.path().display().to_string();
    let args = ["--state-dir", state_arg.as_str(), "--no-watch"];
    let mut versions: Vec<Vec<u8>> = vec![base.clone().into_bytes()];
    for round in 0..6u64 {
        let mut client = start(&args);
        open_workspace(&mut client, project.path());
        // Restore earlier unsaved work if a journal exists, else open the file.
        let list = client.call("recovery.list", Value::Null).unwrap();
        let (id, mut revision) = match list["journals"].as_array().unwrap().first() {
            Some(j) if j["state"] == "recoverable" || j["state"] == "conflict" => {
                let r = client
                    .call(
                        "recovery.restore",
                        json!({ "journalId": j["journalId"], "acceptConflict": true }),
                    )
                    .unwrap();
                (
                    r["document"]["documentId"].as_u64().unwrap(),
                    r["document"]["revision"].as_u64().unwrap(),
                )
            }
            _ => (open_document(&mut client, "big.txt"), 0),
        };
        let text = read(&mut client, id)["text"].as_str().unwrap().to_string();
        let mut expected = text;
        // Several saves in flight; kill after a random-ish delay.
        let mut sent = Vec::new();
        for k in 0..5u64 {
            let marker = format!("round {round} save {k}\n");
            insert(&mut client, id, revision, 0, 0, &marker);
            revision += 1;
            expected.insert_str(0, &marker);
            versions.push(expected.clone().into_bytes());
            sent.push(
                client
                    .send_request(
                        "document.save",
                        json!({ "documentId": id, "expectedRevision": revision }),
                    )
                    .unwrap(),
            );
        }
        std::thread::sleep(Duration::from_millis(3 + round * 7));
        client.kill().unwrap();
        let on_disk = fs::read(&file).unwrap();
        assert!(
            versions.iter().any(|v| v == &on_disk),
            "round {round}: the file is not one complete version ({} bytes)",
            on_disk.len()
        );
    }
    // Leftover temporary files from a kill are possible; report them, never partial targets.
    let leftovers: Vec<String> = fs::read_dir(project.path())
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(".titan-save-"))
        .collect();
    eprintln!("temporary files left by kills: {leftovers:?}");
}
