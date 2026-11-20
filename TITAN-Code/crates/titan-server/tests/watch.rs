//! End-to-end: real `titand`, real file-system events (inotify on Linux, FSEvents on
//! macOS), no `document.checkDisk` calls — every reconciliation below is triggered by the
//! file watcher.

use std::fs;
use std::path::Path;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_protocol::rpc::codes;
use titan_server::client::Client;

fn titand() -> &'static Path {
    Path::new(env!("CARGO_BIN_EXE_titand"))
}

fn start(dir: &Path) -> Client {
    let mut client = Client::spawn_with_args(titand(), &["--no-recovery"]).expect("spawn titand");
    let hello = client
        .call(
            "session.hello",
            json!({ "protocolVersion": 1, "capabilities": {
                "diskNotifications": true, "fileEvents": true } }),
        )
        .unwrap();
    assert_eq!(hello["capabilities"]["fileWatching"], true);
    client
        .call(
            "workspace.open",
            json!({ "path": dir.display().to_string() }),
        )
        .unwrap();
    // Let the watcher settle: wait until a probe file is reported.
    fs::write(dir.join(".probe"), "p").unwrap();
    wait_for(&mut client, "probe event", |n| {
        n["method"] == "workspace/filesChanged"
            && n["params"]["changes"]
                .as_array()
                .is_some_and(|c| c.iter().any(|c| c["path"] == ".probe"))
    });
    client
}

thread_local! {
    /// Every notification seen so far in this test (a later wait may need an earlier one).
    static SEEN: std::cell::RefCell<Vec<Value>> = const { std::cell::RefCell::new(Vec::new()) };
}

/// Poll (with cheap `system.health` round trips) until a notification matches. Matching
/// notifications are consumed; others are kept for later waits.
fn wait_for(client: &mut Client, what: &str, matches: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        let fresh = client.take_notifications();
        let found = SEEN.with(|seen| {
            let mut seen = seen.borrow_mut();
            seen.extend(fresh);
            let pos = seen.iter().position(&matches)?;
            Some(seen.remove(pos)["params"].clone())
        });
        if let Some(params) = found {
            return params;
        }
        if Instant::now() > deadline {
            let seen = SEEN.with(|s| s.borrow().clone());
            panic!("timed out waiting for {what}; saw {seen:#?}");
        }
        std::thread::sleep(Duration::from_millis(25));
        client.call("system.health", Value::Null).unwrap();
    }
}

fn disk_changed(n: &Value, doc: u64, kind: &str) -> bool {
    n["method"] == "document/diskChanged"
        && n["params"]["documentId"] == doc
        && n["params"]["kind"] == kind
}

fn open(client: &mut Client, path: &str) -> u64 {
    client
        .call("document.open", json!({ "path": path }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap()
}

fn read(client: &mut Client, id: u64) -> Value {
    client
        .call("document.read", json!({ "documentId": id }))
        .unwrap()
}

/// The demonstration from the Phase 3 brief: edit without saving, external modification,
/// watcher event, conflict detected, buffer preserved, conflict reported, explicit
/// resolution, bytes verified on disk.
#[test]
fn watcher_detects_conflict_and_explicit_resolution_writes_exact_bytes() {
    let project = tempfile::tempdir().unwrap();
    let root = project.path().canonicalize().unwrap();
    let file = root.join("lib.rs");
    fs::write(&file, "pub fn a() {}\n").unwrap();
    let mut client = start(&root);
    let id = open(&mut client, "lib.rs");
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": 0, "edits": [
                { "range": { "start": { "line": 0, "character": 0 },
                             "end": { "line": 0, "character": 0 } }, "text": "// unsaved\n" } ] }),
        )
        .unwrap();

    let t0 = Instant::now();
    fs::write(&file, "pub fn a() {}\npub fn b() {}\n").unwrap();
    let mut changed = wait_for(&mut client, "conflict", |n| disk_changed(n, id, "modified"));
    eprintln!("watcher → conflict reported in {:?}", t0.elapsed());
    // A non-atomic write can be observed half-done first; the newest report wins.
    std::thread::sleep(Duration::from_millis(300));
    client.call("system.health", Value::Null).unwrap();
    let fresh = client.take_notifications();
    SEEN.with(|seen| {
        let mut seen = seen.borrow_mut();
        seen.extend(fresh);
        seen.retain(|n| {
            if disk_changed(n, id, "modified") {
                changed = n["params"].clone();
                false
            } else {
                true
            }
        });
    });
    assert_eq!(changed["action"], "conflict");
    assert_eq!(changed["cause"], "watcher");
    assert_eq!(changed["dirty"], true);
    assert_eq!(read(&mut client, id)["text"], "// unsaved\npub fn a() {}\n");

    let err = client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::EXTERNAL_MODIFICATION));

    client
        .call(
            "document.resolveConflict",
            json!({ "documentId": id, "expectedRevision": 1, "resolution": "keepBuffer",
                    "diskHash": changed["diskHash"] }),
        )
        .unwrap();
    client
        .call(
            "document.save",
            json!({ "documentId": id, "expectedRevision": 1 }),
        )
        .unwrap();
    assert_eq!(fs::read(&file).unwrap(), b"// unsaved\npub fn a() {}\n");

    // Our own save is seen by the watcher but is not a conflict.
    std::thread::sleep(Duration::from_millis(400));
    client.call("system.health", Value::Null).unwrap();
    let mut echoes: Vec<Value> = client.take_notifications();
    echoes.extend(SEEN.with(|s| s.borrow_mut().drain(..).collect::<Vec<_>>()));
    echoes.retain(|n| n["method"] == "document/diskChanged");
    assert!(
        echoes.is_empty(),
        "own save reported as a change: {echoes:?}"
    );
    let metrics = client.call("system.metrics", Value::Null).unwrap();
    assert_eq!(metrics["watch"]["active"], true);
    assert!(metrics["watch"]["backend"]["rawEvents"].as_u64().unwrap() > 0);
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn deletion_rename_and_atomic_replacement() {
    let project = tempfile::tempdir().unwrap();
    let root = project.path().canonicalize().unwrap();
    fs::write(root.join("gone.rs"), "fn gone() {}\n").unwrap();
    fs::write(root.join("old.rs"), "pub fn renamed_fn() {}\n").unwrap();
    fs::write(root.join("cfg.toml"), "a = 1\n").unwrap();
    let mut client = start(&root);

    // Deletion of a clean, open document: the buffer stays open, marked deleted.
    let gone = open(&mut client, "gone.rs");
    fs::remove_file(root.join("gone.rs")).unwrap();
    let changed = wait_for(&mut client, "deletion", |n| {
        disk_changed(n, gone, "deleted")
    });
    assert_eq!(changed["action"], "none");
    assert_eq!(read(&mut client, gone)["text"], "fn gone() {}\n");

    // Rename of an open document: the old path is deleted for the buffer; the index follows
    // the file to its new name.
    client
        .call(
            "index.status",
            json!({ "waitForIdle": true, "timeoutMs": 10000 }),
        )
        .unwrap();
    let old = open(&mut client, "old.rs");
    fs::rename(root.join("old.rs"), root.join("new.rs")).unwrap();
    wait_for(&mut client, "rename deletes the old path", |n| {
        disk_changed(n, old, "deleted")
    });
    let files = wait_for(&mut client, "new.rs event", |n| {
        n["method"] == "workspace/filesChanged"
            && n["params"]["changes"]
                .as_array()
                .is_some_and(|c| c.iter().any(|c| c["path"] == "new.rs"))
    });
    eprintln!("rename reported as: {files}");
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let found = client
            .call("index.symbols", json!({ "query": "renamed_fn" }))
            .unwrap();
        let paths: Vec<String> = found["matches"]
            .as_array()
            .unwrap()
            .iter()
            .map(|m| m["path"].as_str().unwrap().to_string())
            .collect();
        if paths.contains(&"new.rs".to_string()) {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "index never saw new.rs: {paths:?}"
        );
        std::thread::sleep(Duration::from_millis(50));
    }

    // Atomic replacement by another editor (temp file + rename) of a clean document.
    let cfg = open(&mut client, "cfg.toml");
    let tmp = root.join(".cfg.toml.swp");
    fs::write(&tmp, "a = 2\n").unwrap();
    fs::rename(&tmp, root.join("cfg.toml")).unwrap();
    let changed = wait_for(&mut client, "atomic replacement", |n| {
        disk_changed(n, cfg, "modified")
    });
    assert_eq!(changed["action"], "reloaded");
    let text = read(&mut client, cfg);
    assert_eq!(text["text"], "a = 2\n");
    assert_eq!(text["dirty"], false);
    assert!(client.shutdown().unwrap().success());
}

#[test]
fn project_graph_is_built_and_queried() {
    let project = tempfile::tempdir().unwrap();
    let root = project.path().canonicalize().unwrap();
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(
        root.join("Cargo.toml"),
        "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n",
    )
    .unwrap();
    fs::write(
        root.join("src/lib.rs"),
        "pub mod shapes;\npub fn area() -> u32 { shapes::square(2) }\n\
         #[cfg(test)]\nmod tests {\n    use super::*;\n    #[test]\n    fn area_works() { assert_eq!(area(), 4); }\n}\n",
    )
    .unwrap();
    fs::write(
        root.join("src/shapes.rs"),
        "pub fn square(x: u32) -> u32 { x * x }\n",
    )
    .unwrap();
    let mut client = start(&root);

    let summary = poll_graph(&mut client, |s| {
        s["state"] == "ready" && s["stale"] == false
    });
    assert_eq!(summary["nodes"]["package"], 1);
    assert_eq!(summary["nodes"]["test"], 1);
    assert_eq!(summary["nodes"]["function"], 2, "area and square");
    let find_kind = |client: &mut Client, name: &str, kind: &str| -> Value {
        let r = client
            .call("graph.query", json!({ "relation": "find", "id": name }))
            .unwrap();
        r["results"]
            .as_array()
            .unwrap()
            .iter()
            .find(|n| n["kind"] == kind)
            .cloned()
            .unwrap_or(Value::Null)
    };
    let find = |client: &mut Client, name: &str| -> Value {
        let r = client
            .call("graph.query", json!({ "relation": "find", "id": name }))
            .unwrap();
        r["results"][0].clone()
    };
    let lib = find_kind(&mut client, "demo", "module");
    assert_eq!(lib["kind"], "module");
    assert_eq!(lib["key"], "demo[lib]::crate");
    let children = client
        .call(
            "graph.query",
            json!({ "relation": "children", "id": lib["id"] }),
        )
        .unwrap();
    let names: Vec<&str> = children["results"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r["name"].as_str().unwrap())
        .collect();
    assert!(names.contains(&"shapes"), "{children}");
    let tests = client
        .call(
            "graph.query",
            json!({ "relation": "testsOf", "id": lib["id"] }),
        )
        .unwrap();
    assert_eq!(tests["results"][0]["name"], "area_works");
    let provenance = &tests["results"][0]["edge"]["provenance"];
    assert_eq!(provenance["evidence"], "cfgTestModule");
    assert_eq!(provenance["source"], "tree-sitter");
    assert_eq!(provenance["path"], "src/lib.rs");
    assert_eq!(provenance["freshness"], "current");
    let square = find(&mut client, "square");
    let square_id = square["id"].clone();
    let generation = summary["generation"].as_u64().unwrap();

    // A new module file on disk invalidates the graph; the next build includes it, and
    // existing identities are unchanged.
    fs::write(root.join("src/extra.rs"), "pub fn more() {}\n").unwrap();
    let lib_text = fs::read_to_string(root.join("src/lib.rs")).unwrap();
    fs::write(
        root.join("src/lib.rs"),
        format!("pub mod extra;\n{lib_text}"),
    )
    .unwrap();
    let summary = poll_graph(&mut client, |s| {
        s["state"] == "ready"
            && s["generation"].as_u64().unwrap() > generation
            && s["stale"] == false
    });
    let extra = find(&mut client, "extra");
    assert_eq!(extra["kind"], "module");
    assert_eq!(
        find(&mut client, "square")["id"],
        square_id,
        "stable identity"
    );
    assert!(
        summary["cache"]["reusedLastBuild"].as_u64().unwrap() >= 1,
        "{summary}"
    );
    let err = client
        .call("graph.node", json!({ "id": "999999" }))
        .unwrap_err();
    assert_eq!(err.code(), Some(codes::INVALID_PARAMS));
    assert!(client.shutdown().unwrap().success());
}

fn poll_graph(client: &mut Client, ready: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        let summary = client.call("graph.summary", Value::Null).unwrap();
        if ready(&summary) {
            return summary;
        }
        assert!(Instant::now() < deadline, "graph not ready: {summary}");
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// Simulated memory pressure: graph maintenance is paused (and resumed when it ends is
/// covered by the policy code); editing and the index are unaffected.
#[test]
fn memory_pressure_pauses_the_graph_but_not_editing() {
    let project = tempfile::tempdir().unwrap();
    let root = project.path().canonicalize().unwrap();
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(
        root.join("Cargo.toml"),
        "[package]\nname = \"p\"\nversion = \"0.1.0\"\n",
    )
    .unwrap();
    fs::write(root.join("src/lib.rs"), "pub fn f() {}\n").unwrap();
    let mut client = Client::spawn_with_args(
        titand(),
        &[
            "--no-recovery",
            "--no-watch",
            "--simulate-memory-pressure",
            "critical",
        ],
    )
    .unwrap();
    client
        .call("session.hello", json!({ "protocolVersion": 1 }))
        .unwrap();
    client
        .call(
            "workspace.open",
            json!({ "path": root.display().to_string() }),
        )
        .unwrap();
    client
        .call(
            "index.status",
            json!({ "waitForIdle": true, "timeoutMs": 10000 }),
        )
        .unwrap();
    std::thread::sleep(Duration::from_millis(1_500));
    let summary = client.call("graph.summary", Value::Null).unwrap();
    assert_eq!(summary["state"], "paused", "{summary}");
    assert!(
        summary["paused"]
            .as_str()
            .unwrap()
            .contains("memory pressure")
    );
    let metrics = client.call("system.metrics", Value::Null).unwrap();
    assert_eq!(metrics["resources"]["pressure"], "critical");
    assert!(metrics["resources"]["sheds"].as_u64().unwrap() >= 1);
    let id = client
        .call("document.open", json!({ "path": "src/lib.rs" }))
        .unwrap()["documentId"]
        .as_u64()
        .unwrap();
    let edit = client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": 0, "edits": [
                { "range": { "start": { "line": 0, "character": 0 }, "end": { "line": 0, "character": 0 } },
                  "text": "// still editing\n" } ] }),
        )
        .unwrap();
    assert_eq!(edit["revision"], 1);
    let memory = client.call("system.memory", Value::Null).unwrap();
    assert_eq!(memory["resources"]["pressure"], "critical");
    assert!(
        memory["estimates"]["documents"]["textBytes"]
            .as_u64()
            .unwrap()
            > 0
    );
    assert!(client.shutdown().unwrap().success());
}
