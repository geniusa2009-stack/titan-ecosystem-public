//! System scenarios against a real `titand` (and optionally a real rust-analyzer):
//! editing latency while slower services are busy, file-watcher latency, cancellation
//! latency, protocol throughput, memory and CPU of each process.
//!
//! Latencies are client-observed round trips over pipes (request written → response read).
//! "During X" scenarios only keep samples from rounds in which X was verifiably still
//! running when the round ended, so idle samples cannot dilute them.

use std::fs;
use std::path::Path;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_server::client::Client;

use super::{Scenario, rust_source};

pub(crate) const WORKSPACE_FILES: usize = 150;
pub(crate) const FILE_BYTES: usize = 60 * 1024;

/// `VmRSS` / `VmHWM` (KiB) and CPU seconds (utime + stime, assuming the usual 100 Hz
/// `CLK_TCK`) of a process, from `/proc` (Linux only).
pub(crate) fn process_stats(pid: u32) -> Value {
    let status = fs::read_to_string(format!("/proc/{pid}/status")).ok();
    let field = |name: &str| -> Option<u64> {
        status
            .as_deref()?
            .lines()
            .find(|l| l.starts_with(name))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|v| v.parse().ok())
    };
    let cpu = fs::read_to_string(format!("/proc/{pid}/stat"))
        .ok()
        .and_then(|stat| {
            let after = stat.rsplit_once(')')?.1;
            let fields: Vec<&str> = after.split_whitespace().collect();
            let utime: u64 = fields.get(11)?.parse().ok()?;
            let stime: u64 = fields.get(12)?.parse().ok()?;
            Some((utime + stime) as f64 / 100.0)
        });
    json!({ "pid": pid, "rssKib": field("VmRSS:"), "peakRssKib": field("VmHWM:"), "cpuSeconds": cpu })
}

pub(crate) fn workspace(dir: &Path, cargo: bool) {
    let src = dir.join("src");
    let _ = fs::create_dir_all(&src);
    let body = rust_source(FILE_BYTES);
    let mut lib = String::new();
    for i in 0..WORKSPACE_FILES {
        let _ = fs::write(src.join(format!("m{i}.rs")), &body);
        lib.push_str(&format!("pub mod m{i};\n"));
    }
    lib.push_str("pub mod big;\n");
    let _ = fs::write(src.join("big.rs"), rust_source(1024 * 1024));
    if cargo {
        let _ = fs::write(src.join("lib.rs"), lib);
        let _ = fs::write(
            dir.join("Cargo.toml"),
            "[package]\nname = \"loadfixture\"\nversion = \"0.1.0\"\nedition = \"2021\"\n",
        );
    }
}

fn start(server: &Path, extra: &[String]) -> Option<Client> {
    let mut args = vec!["--no-recovery".to_string()];
    args.extend(extra.iter().cloned());
    let mut client = Client::spawn_with_args(server, &args).ok()?;
    client
        .call(
            "session.hello",
            json!({ "protocolVersion": 1, "capabilities": { "fileEvents": true } }),
        )
        .ok()?;
    Some(client)
}

fn open_ws(client: &mut Client, dir: &Path) -> Option<()> {
    client
        .call(
            "workspace.open",
            json!({ "path": dir.display().to_string() }),
        )
        .ok()
        .map(|_| ())
}

fn open_doc(client: &mut Client, path: &str) -> Option<u64> {
    client.call("document.open", json!({ "path": path })).ok()?["documentId"].as_u64()
}

fn edit_params(id: u64, revision: u64) -> Value {
    json!({ "documentId": id, "expectedRevision": revision, "edits": [
        { "range": { "start": { "line": 3, "character": 4 },
                     "end": { "line": 3, "character": 4 } }, "text": "x" } ] })
}

fn wait_idle(client: &mut Client) {
    let _ = client.call(
        "index.status",
        json!({ "waitForIdle": true, "timeoutMs": 120000 }),
    );
}

/// Edit round trips: idle, during a full indexing scan, during a search/cancellation storm.
fn editing_under_load(server: &Path, ops: usize, out: &mut Vec<Value>, info: &mut Value) {
    let Ok(dir) = tempfile::tempdir() else { return };
    workspace(dir.path(), false);
    let Some(mut client) = start(server, &["--no-watch".to_string()]) else {
        return;
    };
    let dataset = format!("ws_{WORKSPACE_FILES}x{}kb", FILE_BYTES / 1024);
    let bytes = WORKSPACE_FILES * FILE_BYTES + 1024 * 1024;
    if open_ws(&mut client, dir.path()).is_none() {
        return;
    }
    wait_idle(&mut client);
    let Some(id) = open_doc(&mut client, "src/big.rs") else {
        return;
    };
    wait_idle(&mut client);

    // Idle (background services quiet).
    let mut s = Scenario::default();
    let mut revision = 0;
    for _ in 0..ops {
        if s.run(|| {
            client
                .call("document.applyEdits", edit_params(id, revision))
                .ok()
        })
        .is_some()
        {
            revision += 1;
        }
    }
    out.push(s.summary("edit_idle", &dataset, bytes));

    // During a full scan: reopening the workspace resets the index, so every file is read,
    // hashed and parsed again on the worker pool while we edit.
    let mut s = Scenario::default();
    let mut rounds_kept = 0;
    let rounds = (ops / 50).clamp(4, 20);
    for _ in 0..rounds {
        let _ = client.call(
            "document.close",
            json!({ "documentId": id, "discard": true }),
        );
        if open_ws(&mut client, dir.path()).is_none() {
            break;
        }
        let Some(doc) = open_doc(&mut client, "src/big.rs") else {
            break;
        };
        let mut round = Scenario::default();
        let mut rev = 0;
        for _ in 0..50 {
            if round
                .run(|| {
                    client
                        .call("document.applyEdits", edit_params(doc, rev))
                        .ok()
                })
                .is_some()
            {
                rev += 1;
            }
        }
        let still_scanning = client
            .call("index.status", Value::Null)
            .ok()
            .is_some_and(|st| st["scanning"] == true);
        if still_scanning {
            rounds_kept += 1;
            s.absorb(round);
        }
        let _ = client.call(
            "document.close",
            json!({ "documentId": doc, "discard": true }),
        );
        wait_idle(&mut client);
    }
    let mut summary = s.summary("edit_during_scan", &dataset, bytes);
    summary["roundsWithScanActive"] = json!(rounds_kept);
    summary["rounds"] = json!(rounds);
    out.push(summary);

    // Search + cancellation storm: 16 workspace searches in flight, half cancelled, while
    // editing; plus cancellation latency on its own.
    let Some(doc) = open_doc(&mut client, "src/big.rs") else {
        return;
    };
    let mut s = Scenario::default();
    let mut rev = 0;
    let max_depth_before = client
        .call("system.metrics", Value::Null)
        .ok()
        .and_then(|m| m["tasks"]["maxQueueDepth"].as_u64());
    for round in 0..(ops / 40).clamp(3, 10) {
        let searches: Vec<u64> = (0..16)
            .filter_map(|k| {
                client
                    .send_request(
                        "workspace.search",
                        json!({ "query": format!("function_{}", round * 16 + k), "maxResults": 10000 }),
                    )
                    .ok()
            })
            .collect();
        for (k, sid) in searches.iter().enumerate() {
            if k % 2 == 0 {
                let _ = client.send_notification("$/cancelRequest", json!({ "id": sid }));
            }
        }
        for _ in 0..20 {
            if s.run(|| {
                client
                    .call("document.applyEdits", edit_params(doc, rev))
                    .ok()
            })
            .is_some()
            {
                rev += 1;
            }
        }
        for sid in searches {
            let _ = client.wait_response(sid);
        }
    }
    out.push(s.summary("edit_during_search_storm", &dataset, bytes));
    let metrics = client
        .call("system.metrics", Value::Null)
        .unwrap_or_default();
    info["searchStorm"] = json!({
        "maxQueueDepthBefore": max_depth_before,
        "maxQueueDepthAfter": metrics["tasks"]["maxQueueDepth"],
        "cancelled": metrics["requests"]["cancelled"],
        "busyRejections": metrics["requests"]["busyRejections"],
    });

    let mut s = Scenario::default();
    for i in 0..(ops / 20).clamp(10, 50) {
        let Ok(sid) = client.send_request(
            "workspace.search",
            json!({ "query": format!("function_{i}"), "maxResults": 10000 }),
        ) else {
            break;
        };
        std::thread::sleep(Duration::from_millis(2));
        let _ = s.run(|| {
            let _ = client.send_notification("$/cancelRequest", json!({ "id": sid }));
            client.wait_response(sid).ok()
        });
    }
    out.push(s.summary("cancel_search_latency", &dataset, bytes));

    // Pipelined throughput: 1000 requests written before any response is read.
    let n = 1000;
    let start = Instant::now();
    let ids: Vec<u64> = (0..n)
        .filter_map(|_| client.send_request("system.health", Value::Null).ok())
        .collect();
    let answered = ids
        .into_iter()
        .filter(|id| client.wait_response(*id).is_ok())
        .count();
    let secs = start.elapsed().as_secs_f64();
    info["pipelinedHealthThroughput"] = json!({
        "requests": n, "answered": answered, "seconds": secs,
        "requestsPerSecond": answered as f64 / secs.max(1e-9),
    });

    // Syntax index query latency (for comparison with semantic queries below).
    let mut s = Scenario::default();
    for i in 0..(ops / 4).max(20) {
        let _ = s.run(|| {
            client
                .call(
                    "index.symbols",
                    json!({ "query": format!("function_{}", i % 500) }),
                )
                .ok()
        });
    }
    out.push(s.summary("syntax_symbol_query", &dataset, bytes));

    // Sustained editing: memory of titand before and after many edits (bounded history).
    let before = process_stats(client.pid());
    for _ in 0..(ops * 5) {
        if client
            .call("document.applyEdits", edit_params(doc, rev))
            .is_ok()
        {
            rev += 1;
        }
    }
    let after = process_stats(client.pid());
    info["sustainedEditing"] =
        json!({ "edits": ops * 5, "titandBefore": before, "titandAfter": after });
    let _ = client.call(
        "document.close",
        json!({ "documentId": doc, "discard": true }),
    );
    info["titandAfterEditingScenarios"] = process_stats(client.pid());
    let _ = client.shutdown();
}

/// Watcher latency (external write → `workspace/filesChanged`) and search right after an
/// external change.
fn watcher_scenarios(server: &Path, ops: usize, out: &mut Vec<Value>, info: &mut Value) {
    let Ok(dir) = tempfile::tempdir() else { return };
    let root = dir
        .path()
        .canonicalize()
        .unwrap_or_else(|_| dir.path().to_path_buf());
    let _ = fs::create_dir_all(root.join("w"));
    let Some(mut client) = start(server, &[]) else {
        return;
    };
    if open_ws(&mut client, &root).is_none() {
        return;
    }
    let mut s = Scenario::default();
    let mut missed = 0;
    for i in 0..(ops / 20).clamp(10, 40) {
        let name = format!("w/f{i}.txt");
        let _ = client.take_notifications();
        let _ = s.run(|| {
            let _ = fs::write(root.join(&name), format!("v{i}"));
            let deadline = Instant::now() + Duration::from_secs(5);
            loop {
                let _ = client.call("system.health", Value::Null);
                let seen = client.take_notifications().into_iter().any(|n| {
                    n["method"] == "workspace/filesChanged"
                        && n["params"]["changes"]
                            .as_array()
                            .is_some_and(|c| c.iter().any(|c| c["path"] == name.as_str()))
                });
                if seen {
                    return true;
                }
                if Instant::now() > deadline {
                    missed += 1;
                    return false;
                }
            }
        });
        std::thread::sleep(Duration::from_millis(100));
    }
    let mut summary = s.summary("watcher_event_latency", "single_file_write", 0);
    summary["missed"] = json!(missed);
    out.push(summary);

    let mut s = Scenario::default();
    let mut found = 0;
    let rounds = (ops / 20).clamp(10, 40);
    for i in 0..rounds {
        let token = format!("needle_{i}_{}", Instant::now().elapsed().as_nanos());
        let _ = fs::write(root.join("w/search.txt"), format!("x {token} y\n"));
        let hit = s.run(|| {
            client
                .call("workspace.search", json!({ "query": token }))
                .ok()
                .and_then(|r| r["matches"].as_array().map(|m| !m.is_empty()))
                .unwrap_or(false)
        });
        if hit {
            found += 1;
        }
    }
    let mut summary = s.summary("search_after_external_change", "single_file_write", 0);
    summary["foundImmediately"] = json!(found);
    summary["rounds"] = json!(rounds);
    out.push(summary);
    let metrics = client
        .call("system.metrics", Value::Null)
        .unwrap_or_default();
    info["watcher"] = metrics["watch"].clone();
    let _ = client.shutdown();
}

/// Editing while rust-analyzer loads and analyses the workspace; semantic query latency;
/// restart latency; memory of both processes.
fn lsp_scenarios(server: &Path, ra: &Path, ops: usize, out: &mut Vec<Value>, info: &mut Value) {
    let Ok(dir) = tempfile::tempdir() else { return };
    workspace(dir.path(), true);
    let args = vec![
        "--no-watch".to_string(),
        "--rust-analyzer".to_string(),
        ra.display().to_string(),
    ];
    let Some(mut client) = start(server, &args) else {
        return;
    };
    let dataset = format!("cargo_ws_{WORKSPACE_FILES}x{}kb", FILE_BYTES / 1024);
    let bytes = WORKSPACE_FILES * FILE_BYTES + 1024 * 1024;
    let started = Instant::now();
    if open_ws(&mut client, dir.path()).is_none() {
        return;
    }
    let Some(doc) = open_doc(&mut client, "src/big.rs") else {
        return;
    };
    let state = |client: &mut Client| -> Value {
        client
            .call("intel.status", Value::Null)
            .map(|s| s["semantic"].clone())
            .unwrap_or_default()
    };
    // Edit while rust-analyzer is starting/indexing; keep only batches that ended before
    // it reported ready.
    let mut s = Scenario::default();
    let mut rev = 0;
    let deadline = Instant::now() + Duration::from_secs(300);
    let mut indexing_batches = 0;
    loop {
        let mut batch = Scenario::default();
        for _ in 0..20 {
            if batch
                .run(|| {
                    client
                        .call("document.applyEdits", edit_params(doc, rev))
                        .ok()
                })
                .is_some()
            {
                rev += 1;
            }
        }
        let st = state(&mut client);
        if st["state"] == "ready" || Instant::now() > deadline {
            info["rustAnalyzerReadyMs"] = json!(started.elapsed().as_millis() as u64);
            break;
        }
        indexing_batches += 1;
        s.absorb(batch);
    }
    let mut summary = s.summary("edit_during_lsp_indexing", &dataset, bytes);
    summary["batches"] = json!(indexing_batches);
    out.push(summary);

    // Ready: every edit triggers didChange + analysis + diagnostics in rust-analyzer.
    let mut s = Scenario::default();
    for _ in 0..ops {
        if s.run(|| {
            client
                .call("document.applyEdits", edit_params(doc, rev))
                .ok()
        })
        .is_some()
        {
            rev += 1;
        }
    }
    out.push(s.summary("edit_with_lsp_active", &dataset, bytes));

    // Semantic query latency (definition of a function call inside big.rs).
    let mut s = Scenario::default();
    let mut current = 0;
    for _ in 0..(ops / 10).max(20) {
        let r = s.run(|| {
            client
                .call(
                    "intel.definition",
                    json!({ "documentId": doc, "expectedRevision": rev,
                            "position": { "line": 1, "character": 10 } }),
                )
                .ok()
        });
        if r.is_some_and(|r| {
            r["results"].as_array().is_some_and(|e| {
                e.iter()
                    .any(|e| e["kind"] == "semantic" && e["status"] == "current")
            })
        }) {
            current += 1;
        }
    }
    let mut summary = s.summary("semantic_definition_query", &dataset, bytes);
    summary["currentAnswers"] = json!(current);
    out.push(summary);

    let metrics = client
        .call("system.metrics", Value::Null)
        .unwrap_or_default();
    let ra_pid = metrics["lsp"]["pid"]
        .as_u64()
        .and_then(|p| u32::try_from(p).ok());
    info["processesWithLsp"] = json!({
        "titand": process_stats(client.pid()),
        "rustAnalyzer": ra_pid.map(process_stats),
    });

    // Restart latency: kill -9 rust-analyzer, time until it is ready again.
    if let Some(pid) = ra_pid {
        let killed = std::process::Command::new("kill")
            .args(["-9", &pid.to_string()])
            .status()
            .is_ok_and(|s| s.success());
        let t = Instant::now();
        let deadline = t + Duration::from_secs(300);
        let mut ready_ms = None;
        let mut edit_ok = true;
        while killed && Instant::now() < deadline {
            edit_ok &= client
                .call("document.applyEdits", edit_params(doc, rev))
                .is_ok();
            rev += 1;
            let st = state(&mut client);
            if st["state"] == "ready" && st["generation"].as_u64().unwrap_or(0) >= 2 {
                ready_ms = Some(t.elapsed().as_millis() as u64);
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        info["lspRestart"] =
            json!({ "killed": killed, "readyAgainMs": ready_ms, "editsKeptWorking": edit_ok });
    }
    let t = Instant::now();
    let _ = client.shutdown();
    info["shutdownWithLspMs"] = json!(t.elapsed().as_millis() as u64);
}

pub(crate) fn run(
    server: &Path,
    rust_analyzer: Option<&Path>,
    ops: usize,
    out: &mut Vec<Value>,
) -> Value {
    let mut info = json!({});
    eprintln!("titan-bench: editing under load");
    editing_under_load(server, ops, out, &mut info);
    eprintln!("titan-bench: watcher");
    watcher_scenarios(server, ops, out, &mut info);
    if let Some(ra) = rust_analyzer {
        eprintln!("titan-bench: rust-analyzer via {}", ra.display());
        lsp_scenarios(server, ra, ops, out, &mut info);
    }
    info
}
