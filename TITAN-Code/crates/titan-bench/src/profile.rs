//! Profiling scenarios (system level, real processes):
//!
//! * **memory phases** — `system.memory` of a `titand` built with the `alloc-stats` feature
//!   (live Rust heap) after each phase of a session, with and without rust-analyzer;
//!   rust-analyzer's own memory is read from `/proc` separately.
//! * **cold start** — a fresh `titand` + rust-analyzer per configuration: time to ready, to
//!   the first diagnostics, the first definition and hover (cold), then warm requests;
//!   plus the first query after a burst of edits. Measured with cache priming off and on
//!   (`titand`'s default since Phase 4).

use std::fs;
use std::path::Path;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_server::client::Client;

use super::load::{FILE_BYTES, WORKSPACE_FILES, workspace};

fn start(server: &Path, args: &[String]) -> Option<Client> {
    let mut all = vec!["--no-recovery".to_string(), "--no-watch".to_string()];
    all.extend(args.iter().cloned());
    let mut client = Client::spawn_with_args(server, &all).ok()?;
    client
        .call("session.hello", json!({ "protocolVersion": 1 }))
        .ok()?;
    Some(client)
}

fn memory(client: &mut Client, phase: &str, t0: Instant) -> Value {
    let mut m = client
        .call("system.memory", Value::Null)
        .unwrap_or_default();
    m["phase"] = json!(phase);
    m["atMs"] = json!(t0.elapsed().as_millis() as u64);
    m
}

fn wait_idle(client: &mut Client) {
    let _ = client.call(
        "index.status",
        json!({ "waitForIdle": true, "timeoutMs": 120000 }),
    );
}

fn wait_graph(client: &mut Client) {
    let deadline = Instant::now() + Duration::from_secs(120);
    while Instant::now() < deadline {
        let s = client
            .call("graph.summary", Value::Null)
            .unwrap_or_default();
        if s["state"] == "ready" && s["stale"] == false {
            return;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

fn semantic_state(client: &mut Client) -> Value {
    client
        .call("intel.status", Value::Null)
        .map(|s| s["semantic"].clone())
        .unwrap_or_default()
}

fn wait_ready(client: &mut Client, secs: u64) -> bool {
    let deadline = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < deadline {
        if semantic_state(client)["state"] == "ready" {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

fn edit(client: &mut Client, id: u64, rev: u64) -> bool {
    client
        .call(
            "document.applyEdits",
            json!({ "documentId": id, "expectedRevision": rev, "edits": [
                { "range": { "start": { "line": 3, "character": 4 },
                             "end": { "line": 3, "character": 4 } }, "text": "x" } ] }),
        )
        .is_ok()
}

/// Memory after each phase of a realistic session.
fn memory_phases(server: &Path, ra: Option<&Path>) -> Value {
    let Ok(dir) = tempfile::tempdir() else {
        return Value::Null;
    };
    workspace(dir.path(), true);
    let mut args = Vec::new();
    if let Some(ra) = ra {
        args.push("--rust-analyzer".to_string());
        args.push(ra.display().to_string());
    }
    let Some(mut client) = start(server, &args) else {
        return Value::Null;
    };
    let t0 = Instant::now();
    let mut phases = vec![memory(&mut client, "started", t0)];
    if client
        .call(
            "workspace.open",
            json!({ "path": dir.path().display().to_string() }),
        )
        .is_err()
    {
        return Value::Null;
    }
    wait_idle(&mut client);
    phases.push(memory(&mut client, "workspaceIndexed", t0));
    wait_graph(&mut client);
    phases.push(memory(&mut client, "graphBuilt", t0));
    let Some(id) = client
        .call("document.open", json!({ "path": "src/big.rs" }))
        .ok()
        .and_then(|i| i["documentId"].as_u64())
    else {
        return Value::Null;
    };
    wait_idle(&mut client);
    phases.push(memory(&mut client, "bigDocumentOpen", t0));
    if ra.is_some() {
        let ready = wait_ready(&mut client, 300);
        let mut m = memory(&mut client, "languageServerReady", t0);
        m["ready"] = json!(ready);
        phases.push(m);
    }
    let mut rev = 0;
    for _ in 0..2_500 {
        if edit(&mut client, id, rev) {
            rev += 1;
        }
    }
    wait_idle(&mut client);
    phases.push(memory(&mut client, "after2500Edits", t0));
    wait_graph(&mut client);
    phases.push(memory(&mut client, "after2500EditsGraphSettled", t0));
    let _ = client.call("workspace.close", json!({ "discard": true }));
    std::thread::sleep(Duration::from_millis(1_500));
    phases.push(memory(&mut client, "workspaceClosed", t0));
    let _ = client.shutdown();
    json!({ "withLanguageServer": ra.is_some(), "phases": phases })
}

/// Line and column of `function_1` in the caller appended to `m0.rs`.
fn caller_position(dir: &Path) -> (usize, usize) {
    let path = dir.join("src/m0.rs");
    let mut text = fs::read_to_string(&path).unwrap_or_default();
    let line = text.lines().count();
    let call = "pub fn caller() -> u64 { crate::m1::function_1(2) }\n";
    text.push_str(call);
    let _ = fs::write(&path, text);
    (line, call.find("function_1").unwrap_or(0) + 2)
}

fn cold_start(server: &Path, ra: &Path, priming: bool) -> Value {
    let Ok(dir) = tempfile::tempdir() else {
        return Value::Null;
    };
    workspace(dir.path(), true);
    let (line, character) = caller_position(dir.path());
    let args = vec![
        "--rust-analyzer".to_string(),
        ra.display().to_string(),
        "--lsp-settings".to_string(),
        json!({
            "checkOnSave": false,
            "cargo": { "buildScripts": { "enable": false } },
            "procMacro": { "enable": false },
            "cachePriming": { "enable": priming },
            "diagnostics": { "experimental": { "enable": true } },
        })
        .to_string(),
    ];
    let t0 = Instant::now();
    let Some(mut client) = start(server, &args) else {
        return Value::Null;
    };
    let spawned_ms = t0.elapsed().as_millis() as u64;
    if client
        .call(
            "workspace.open",
            json!({ "path": dir.path().display().to_string() }),
        )
        .is_err()
    {
        return Value::Null;
    }
    let Some(id) = client
        .call("document.open", json!({ "path": "src/m0.rs" }))
        .ok()
        .and_then(|i| i["documentId"].as_u64())
    else {
        return Value::Null;
    };
    let ready = wait_ready(&mut client, 300);
    let ready_ms = t0.elapsed().as_millis() as u64;
    let position = json!({ "documentId": id, "expectedRevision": 0,
                           "position": { "line": line, "character": character } });
    let timed = |client: &mut Client, method: &str| -> (u64, String, usize) {
        let t = Instant::now();
        let r = client.call(method, position.clone()).unwrap_or_default();
        let sem = r["results"]
            .as_array()
            .and_then(|e| e.iter().find(|e| e["kind"] == "semantic").cloned())
            .unwrap_or_default();
        (
            t.elapsed().as_micros() as u64,
            sem["status"].as_str().unwrap_or_default().to_string(),
            sem["items"].as_array().map_or(0, Vec::len),
        )
    };
    let first_definition = timed(&mut client, "intel.definition");
    let first_hover = timed(&mut client, "intel.hover");
    let mut warm = Vec::new();
    for _ in 0..20 {
        warm.push(timed(&mut client, "intel.definition").0);
    }
    warm.sort_unstable();
    // A burst of edits elsewhere in the file, then the first query.
    let mut rev = 0;
    for _ in 0..500 {
        if edit(&mut client, id, rev) {
            rev += 1;
        }
    }
    let position_after = json!({ "documentId": id, "expectedRevision": rev,
                                 "position": { "line": line, "character": character } });
    let t = Instant::now();
    let after_burst = client
        .call("intel.definition", position_after)
        .unwrap_or_default();
    let after_burst_us = t.elapsed().as_micros() as u64;
    let after_burst_status = after_burst["results"]
        .as_array()
        .and_then(|e| e.iter().find(|e| e["kind"] == "semantic").cloned())
        .map(|s| s["status"].clone())
        .unwrap_or_default();
    let cold = semantic_state(&mut client)["coldStart"].clone();
    let _ = client.shutdown();
    let pick = |q: f64| {
        warm.get(((warm.len().saturating_sub(1)) as f64 * q) as usize)
            .copied()
    };
    json!({
        "cachePriming": priming,
        "dataset": format!("cargo_ws_{WORKSPACE_FILES}x{}kb", FILE_BYTES / 1024),
        "titandSpawnedMs": spawned_ms,
        "readyMs": ready_ms,
        "ready": ready,
        "serverTimeline": cold,
        "firstDefinition": { "us": first_definition.0, "status": first_definition.1, "items": first_definition.2 },
        "firstHover": { "us": first_hover.0, "status": first_hover.1, "items": first_hover.2 },
        "warmDefinitionUs": { "median": pick(0.5), "p95": pick(0.95), "max": warm.last() },
        "definitionAfter500Edits": { "us": after_burst_us, "status": after_burst_status },
    })
}

fn ms(t: Instant) -> f64 {
    t.elapsed().as_secs_f64() * 1e3
}

/// Project Twin and Verified Changes costs on the 150-file workspace (+ rust-analyzer for
/// impact and rename when available).
fn twin_scenarios(server: &Path, ra: Option<&Path>) -> Value {
    let Ok(dir) = tempfile::tempdir() else {
        return Value::Null;
    };
    let Ok(state) = tempfile::tempdir() else {
        return Value::Null;
    };
    workspace(dir.path(), true);
    let (line, character) = caller_position(dir.path());
    let mut args = vec![
        "--state-dir".to_string(),
        state.path().display().to_string(),
    ];
    if let Some(ra) = ra {
        args.push("--rust-analyzer".to_string());
        args.push(ra.display().to_string());
    }
    let Some(mut client) = start(server, &args) else {
        return Value::Null;
    };
    let t = Instant::now();
    if client
        .call(
            "workspace.open",
            json!({ "path": dir.path().display().to_string() }),
        )
        .is_err()
    {
        return Value::Null;
    }
    wait_idle(&mut client);
    wait_graph(&mut client);
    let open_to_graph_ms = ms(t);
    let summary = client
        .call("graph.summary", Value::Null)
        .unwrap_or_default();
    let Some(id) = client
        .call("document.open", json!({ "path": "src/m0.rs" }))
        .ok()
        .and_then(|i| i["documentId"].as_u64())
    else {
        return Value::Null;
    };
    wait_idle(&mut client);
    // Local edits: every published parse updates the graph in place.
    let mut rev = 0;
    for _ in 0..200 {
        if edit(&mut client, id, rev) {
            rev += 1;
        }
        let _ = client.call(
            "index.status",
            json!({ "waitForIdle": true, "timeoutMs": 10000 }),
        );
    }
    let after_local = client
        .call("graph.summary", Value::Null)
        .unwrap_or_default();
    // A structural edit (new module declaration): time until a rebuilt graph is published.
    let generation = after_local["generation"].as_u64().unwrap_or(0);
    let t = Instant::now();
    let structural = client.call(
        "document.applyEdits",
        json!({ "documentId": id, "expectedRevision": rev, "edits": [
            { "range": { "start": { "line": 0, "character": 0 }, "end": { "line": 0, "character": 0 } },
              "text": "pub mod generated_module {}\n" } ] }),
    );
    rev += 1;
    let mut rebuild_ms = None;
    if structural.is_ok() {
        let deadline = Instant::now() + Duration::from_secs(60);
        while Instant::now() < deadline {
            let s = client
                .call("graph.summary", Value::Null)
                .unwrap_or_default();
            if s["generation"].as_u64().unwrap_or(0) > generation && s["stale"] == false {
                rebuild_ms = Some(ms(t));
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    // Multi-file change: one edit in each of 50 files on disk.
    let changes: Vec<Value> = (1..=50)
        .filter_map(|i| {
            let path = format!("src/m{i}.rs");
            let bytes = fs::read(dir.path().join(&path)).ok()?;
            Some(json!({
                "kind": "edit", "path": path,
                "expected": { "kind": "disk", "hash": blake3_hex(&bytes), "len": bytes.len() },
                "edits": [{ "start": 0, "end": 0, "text": "// changed by the benchmark\n" }],
            }))
        })
        .collect();
    let t = Instant::now();
    let proposed = client
        .call("changes.propose", json!({ "changes": changes }))
        .unwrap_or_default();
    let propose_ms = ms(t);
    let pid = proposed["proposal"]["id"].clone();
    let _ = client.call("changes.approve", json!({ "proposalId": pid }));
    let t = Instant::now();
    let applied = client
        .call("changes.apply", json!({ "proposalId": pid }))
        .unwrap_or_default();
    let apply_ms = ms(t);
    let t = Instant::now();
    let restored = client
        .call("changes.restore", json!({ "proposalId": pid }))
        .unwrap_or_default();
    let restore_ms = ms(t);

    let mut semantic = Value::Null;
    if ra.is_some() && wait_ready(&mut client, 300) {
        let position = json!({ "documentId": id, "expectedRevision": rev,
                               "position": { "line": line + 1, "character": character } });
        // (+1: the structural edit above inserted one line at the top.)
        let mut impact_us = Vec::new();
        let mut last = Value::Null;
        for _ in 0..10 {
            let t = Instant::now();
            last = client
                .call("twin.impact", position.clone())
                .unwrap_or_default();
            impact_us.push(t.elapsed().as_micros() as u64);
        }
        let t = Instant::now();
        let rename = client
            .call(
                "refactor.rename",
                json!({ "documentId": id, "expectedRevision": rev,
                        "position": { "line": line + 1, "character": character }, "newName": "function_one" }),
            )
            .unwrap_or_default();
        let rename_ms = ms(t);
        semantic = json!({
            "impactUs": impact_us,
            "impactCallers": last["known"]["callers"]["items"].as_array().map_or(0, Vec::len),
            "impactReferences": last["known"]["references"]["items"].as_array().map_or(0, Vec::len),
            "impactAffectedFiles": last["affectedFiles"].as_array().map_or(0, Vec::len),
            "renamePreviewMs": rename_ms,
            "renameFiles": rename["proposal"]["changes"].as_array().map_or(0, Vec::len),
            "renameStatus": rename["status"],
        });
    }
    let memory = client
        .call("system.memory", Value::Null)
        .unwrap_or_default();
    let _ = client.shutdown();
    json!({
        "openToGraphReadyMs": open_to_graph_ms,
        "graph": {
            "nodes": summary["nodes"],
            "edges": summary["edges"],
            "fullBuildMs": summary["cache"]["lastBuildMs"],
            "estimatedBytes": memory["estimates"]["graph"]["estimatedBytes"],
            "localUpdates": after_local["cache"]["localUpdates"],
            "localUpdateUs": after_local["cache"]["localUpdateUs"],
            "structuralRebuildMs": rebuild_ms,
            "reusedFactsOnRebuild": client_reused(&after_local),
        },
        "changes50Files": {
            "proposeAndPreviewMs": propose_ms,
            "validated": proposed["status"],
            "applyMs": apply_ms,
            "outcome": applied["outcome"],
            "restoreMs": restore_ms,
            "restored": restored["restored"],
        },
        "semantic": semantic,
    })
}

fn client_reused(summary: &Value) -> Value {
    summary["cache"]["reusedLastBuild"].clone()
}

fn blake3_hex(bytes: &[u8]) -> String {
    titan_doc::ContentHash::of(bytes).to_hex()
}

pub(crate) fn run(alloc_server: Option<&Path>, server: &Path, ra: Option<&Path>) -> Value {
    let mut out = json!({});
    let profiled = alloc_server.unwrap_or(server);
    eprintln!("titan-bench: memory phases ({})", profiled.display());
    out["memoryWithoutLanguageServer"] = memory_phases(profiled, None);
    if let Some(ra) = ra {
        out["memoryWithLanguageServer"] = memory_phases(profiled, Some(ra));
        eprintln!("titan-bench: cold start, cache priming off");
        out["coldStartPrimingOff"] = cold_start(server, ra, false);
        eprintln!("titan-bench: cold start, cache priming on");
        out["coldStartPrimingOn"] = cold_start(server, ra, true);
    }
    eprintln!("titan-bench: project twin and verified changes");
    out["twin"] = twin_scenarios(server, ra);
    out["heapMeasured"] = json!(alloc_server.is_some());
    out
}
