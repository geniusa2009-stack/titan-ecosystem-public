//! Failure-injection tests against `titan-fake-lsp`.

mod common;

use std::path::Path;
use std::time::{Duration, Instant};

use common::{Events, process_alive};
use serde_json::{Value, json};
use titan_lsp::{LspCommand, LspError, LspEvent, ServerState, Supervisor, SupervisorConfig};

const T: Duration = Duration::from_secs(20);

fn config(args: &[&str]) -> SupervisorConfig {
    let command = LspCommand::new(env!("CARGO_BIN_EXE_titan-fake-lsp")).args(args.iter().copied());
    let mut config = SupervisorConfig::new(command, env!("CARGO_MANIFEST_DIR"));
    config.init_timeout = Duration::from_secs(10);
    config.request_timeout = Duration::from_secs(10);
    config.shutdown_timeout = Duration::from_secs(5);
    config.backoff_initial = Duration::from_millis(20);
    config.backoff_max = Duration::from_millis(200);
    config.max_restarts = 3;
    config.restart_window = Duration::from_secs(60);
    config
}

fn start(config: SupervisorConfig) -> (Supervisor, Events) {
    let (events, callback) = Events::new();
    (Supervisor::start(config, callback), events)
}

fn states(events: &Events, generation: u64) -> Vec<ServerState> {
    events
        .snapshot()
        .into_iter()
        .filter_map(|e| match e {
            LspEvent::State {
                generation: g,
                state,
            } if g == generation => Some(state),
            _ => None,
        })
        .collect()
}

fn did_open(uri: &str, version: i64) -> Value {
    json!({ "textDocument": { "uri": uri, "languageId": "rust", "version": version, "text": "fn main() {}\n" } })
}

#[test]
fn initialize_handshake_sends_client_info_and_captures_capabilities() {
    let mut config = config(&[]);
    config.initialization_options = json!({ "fake": true });
    let (sup, events) = start(config);
    events.wait_running(1, T);
    assert_eq!(
        states(&events, 1),
        vec![
            ServerState::Starting,
            ServerState::Initializing,
            ServerState::Running { quiescent: None }
        ]
    );
    assert_eq!(sup.generation(), 1);
    assert!(sup.pid().is_some());
    assert_eq!(sup.position_encoding(), "utf-16");
    assert_eq!(
        sup.server_info().expect("serverInfo")["name"],
        "titan-fake-lsp"
    );
    let caps = sup.server_capabilities().expect("capabilities captured");
    for key in [
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
        "workspaceSymbolProvider",
    ] {
        assert_eq!(caps[key], json!(true), "{key} in {caps}");
    }
    assert_eq!(
        caps["completionProvider"]["triggerCharacters"],
        json!([":", "."])
    );

    let ticket = sup.request("fake/state", json!({}), None).expect("request");
    let (generation, result) = events.response(ticket, T);
    assert_eq!(generation, 1);
    let init = result.expect("fake/state answer")["initializeParams"].clone();
    let root_uri = titan_lsp::path_to_uri(Path::new(env!("CARGO_MANIFEST_DIR")));
    assert_eq!(init["processId"], json!(std::process::id()));
    assert_eq!(init["clientInfo"]["name"], "titand");
    assert_eq!(init["rootUri"], json!(root_uri));
    assert_eq!(init["workspaceFolders"][0]["uri"], json!(root_uri));
    assert_eq!(init["workspaceFolders"][0]["name"], "titan-lsp");
    assert_eq!(init["initializationOptions"], json!({ "fake": true }));
    let c = &init["capabilities"];
    assert_eq!(c["general"]["positionEncodings"], json!(["utf-16"]));
    assert_eq!(c["textDocument"]["synchronization"]["didSave"], true);
    assert_eq!(
        c["textDocument"]["publishDiagnostics"]["relatedInformation"],
        true
    );
    assert_eq!(
        c["textDocument"]["publishDiagnostics"]["versionSupport"],
        true
    );
    assert!(c["textDocument"]["definition"].is_object());
    assert!(c["textDocument"]["references"].is_object());
    assert!(c["textDocument"]["completion"].is_object());
    assert_eq!(
        c["textDocument"]["hover"]["contentFormat"],
        json!(["markdown", "plaintext"])
    );
    assert_eq!(
        c["textDocument"]["documentSymbol"]["hierarchicalDocumentSymbolSupport"],
        true
    );
    assert!(c["workspace"]["symbol"].is_object());
    assert_eq!(
        c["workspace"]["didChangeWatchedFiles"]["dynamicRegistration"],
        false
    );
    assert_eq!(c["window"]["workDoneProgress"], true);
    assert_eq!(c["experimental"]["serverStatusNotification"], true);

    let report = sup.shutdown();
    assert!(report.shutdown_acknowledged);
    assert!(!report.forced_kill);
    assert_eq!(report.metrics.starts, 1);
    assert_eq!(report.metrics.crashes, 0);
}

#[test]
fn negotiated_position_encoding_is_recorded() {
    let (sup, events) = start(config(&["--position-encoding", "utf-32"]));
    events.wait_running(1, T);
    assert_eq!(sup.position_encoding(), "utf-32");
    sup.shutdown();
}

#[test]
fn server_status_quiescence_is_reflected_in_state() {
    let (sup, events) = start(config(&["--server-status"]));
    events.wait_state(T, "running, quiescent", |g, s| {
        g == 1
            && *s
                == ServerState::Running {
                    quiescent: Some(true),
                }
    });
    let running: Vec<ServerState> = states(&events, 1)
        .into_iter()
        .filter(ServerState::is_running)
        .collect();
    assert_eq!(
        running,
        vec![
            ServerState::Running { quiescent: None },
            ServerState::Running {
                quiescent: Some(false)
            },
            ServerState::Running {
                quiescent: Some(true)
            },
        ]
    );
    assert_eq!(
        sup.state(),
        ServerState::Running {
            quiescent: Some(true)
        }
    );
    assert_eq!(
        events.count(|e| matches!(e, LspEvent::Notification { method, .. } if method == "experimental/serverStatus")),
        2
    );
    sup.shutdown();
}

#[test]
fn server_to_client_requests_are_answered() {
    let mut config = config(&["--server-requests"]);
    config.workspace_configuration = json!({ "fake": { "answer": 42, "nested": "deep" } });
    let (sup, events) = start(config);
    let (_, params) = events.wait_notification(0, T, "fake/clientResponses", |_, _| true);
    let mut responses = params["responses"].as_array().expect("array").clone();
    responses.sort_by_key(|r| r["id"].as_str().unwrap_or("").to_string());
    assert_eq!(responses.len(), 4);
    for response in &responses {
        assert_eq!(response["jsonrpc"], "2.0");
    }
    assert_eq!(responses[0]["id"], "srv-0");
    assert_eq!(
        responses[0]["result"],
        json!([{ "answer": 42, "nested": "deep" }, "deep", null])
    );
    assert_eq!(responses[1]["id"], "srv-1");
    assert_eq!(responses[1]["result"], Value::Null);
    assert!(responses[1].get("result").is_some());
    assert_eq!(responses[2]["id"], "srv-2");
    assert_eq!(responses[2]["result"], Value::Null);
    assert!(responses[2].get("result").is_some());
    assert_eq!(responses[3]["id"], "srv-3");
    assert_eq!(responses[3]["error"]["code"], json!(-32601));
    assert_eq!(sup.metrics().server_requests, 4);
    sup.shutdown();
}

#[test]
fn out_of_order_responses_are_matched_to_their_tickets() {
    let (sup, events) = start(config(&["--reverse-order"]));
    events.wait_running(1, T);
    let tickets: Vec<u64> = (0..5)
        .map(|n| {
            sup.request("textDocument/hover", json!({ "n": n }), None)
                .expect("request")
        })
        .collect();
    sup.notify("fake/flush", Value::Null).expect("flush");
    for (n, ticket) in tickets.iter().enumerate() {
        let (_, result) = events.response(*ticket, T);
        let result = result.expect("echo");
        assert_eq!(result["id"], json!(ticket));
        assert_eq!(result["params"]["n"], json!(n));
        assert_eq!(events.responses_for(*ticket), 1);
    }
    let order: Vec<u64> = events
        .snapshot()
        .into_iter()
        .filter_map(|e| match e {
            LspEvent::Response { ticket, .. } => Some(ticket),
            _ => None,
        })
        .collect();
    let mut reversed = tickets.clone();
    reversed.reverse();
    assert_eq!(order, reversed, "the fake answered newest first");
    let metrics = sup.metrics();
    assert_eq!(metrics.requests_sent, 5);
    assert_eq!(metrics.requests_completed, 5);
    sup.shutdown();
}

#[test]
fn responses_with_unknown_ids_are_counted_and_ignored() {
    let (sup, events) = start(config(&[]));
    events.wait_running(1, T);
    let ticket = sup
        .request("fake/sendUnknownResponses", json!({}), None)
        .expect("request");
    let (_, result) = events.response(ticket, T);
    assert_eq!(result.expect("echo")["id"], json!(ticket));
    let metrics = sup.metrics();
    assert_eq!(metrics.unknown_responses, 2);
    assert_eq!(metrics.crashes, 0);
    assert!(sup.state().is_running());
    sup.shutdown();
}

#[test]
fn request_timeout_fires_and_sends_cancel_request() {
    let mut config = config(&["--hang-on", "textDocument/hover"]);
    config.request_timeout = Duration::from_millis(300);
    let (sup, events) = start(config);
    events.wait_running(1, T);

    let started = Instant::now();
    let explicit = sup
        .request(
            "textDocument/hover",
            json!({}),
            Some(Duration::from_millis(150)),
        )
        .expect("request");
    let (_, result) = events.response(explicit, T);
    assert_eq!(result, Err(LspError::Timeout));
    assert!(started.elapsed() >= Duration::from_millis(150));
    events.wait_notification(0, T, "fake/cancelled", |_, p| p["id"] == json!(explicit));

    let started = Instant::now();
    let defaulted = sup
        .request("textDocument/hover", json!({}), None)
        .expect("request");
    let (_, result) = events.response(defaulted, T);
    assert_eq!(result, Err(LspError::Timeout));
    assert!(started.elapsed() >= Duration::from_millis(300));
    events.wait_notification(0, T, "fake/cancelled", |_, p| p["id"] == json!(defaulted));

    assert_eq!(events.responses_for(explicit), 1);
    assert_eq!(events.responses_for(defaulted), 1);
    let metrics = sup.metrics();
    assert_eq!(metrics.requests_timed_out, 2);
    assert!(sup.state().is_running(), "a timeout is not a crash");
    sup.shutdown();
}

#[test]
fn explicit_cancel_completes_once_and_drops_the_late_answer() {
    let (sup, events) = start(config(&[
        "--hang-on",
        "textDocument/hover",
        "--answer-after-cancel",
    ]));
    events.wait_running(1, T);
    let ticket = sup
        .request("textDocument/hover", json!({}), None)
        .expect("request");
    assert!(sup.cancel(ticket));
    assert!(
        !sup.cancel(ticket),
        "a completed ticket cannot be cancelled again"
    );
    let (generation, result) = events.response(ticket, T);
    assert_eq!(generation, 1);
    assert_eq!(result, Err(LspError::Cancelled));
    // The fake answers the cancelled request and then acknowledges the cancellation.
    events.wait_notification(0, T, "fake/cancelled", |_, p| p["id"] == json!(ticket));
    let metrics = sup.metrics();
    assert_eq!(metrics.late_responses, 1);
    assert_eq!(metrics.requests_cancelled, 1);
    assert_eq!(
        events.responses_for(ticket),
        1,
        "the late answer was dropped"
    );
    assert!(!sup.cancel(999_999));
    sup.shutdown();
}

#[test]
fn crash_fails_pending_requests_then_restarts_with_a_new_generation() {
    let (sup, events) = start(config(&[
        "--crash-on",
        "textDocument/hover",
        "--hang-on",
        "textDocument/definition",
    ]));
    events.wait_running(1, T);
    let first_pid = sup.pid().expect("pid");
    let uri = "file:///fake/lib.rs";
    assert_eq!(sup.notify("textDocument/didOpen", did_open(uri, 1)), Ok(1));
    events.wait_notification(0, T, "textDocument/publishDiagnostics", |g, p| {
        g == 1 && p["uri"] == uri && p["version"] == 1
    });

    let hanging = sup
        .request("textDocument/definition", json!({}), None)
        .expect("request");
    let crashing = sup
        .request("textDocument/hover", json!({}), None)
        .expect("request");
    for ticket in [hanging, crashing] {
        let (generation, result) = events.response(ticket, T);
        assert_eq!(generation, 1);
        match result {
            Err(LspError::ConnectionLost(reason)) => {
                assert!(reason.contains("exit status: 3"), "{reason}");
            }
            other => panic!("expected ConnectionLost, got {other:?}"),
        }
    }
    match events.wait_for(T, "crash event", |e| matches!(e, LspEvent::Crashed { .. })) {
        LspEvent::Crashed {
            generation,
            stderr_tail,
            ..
        } => {
            assert_eq!(generation, 1);
            assert!(
                stderr_tail.contains("crash-on textDocument/hover"),
                "{stderr_tail}"
            );
        }
        other => panic!("{other:?}"),
    }
    events.wait_state(T, "restarting", |g, s| {
        g == 1
            && *s
                == ServerState::Restarting {
                    attempt: 1,
                    retry_in: Duration::from_millis(20),
                }
    });
    events.wait_running(2, T);
    assert_eq!(sup.generation(), 2);
    let second_pid = sup.pid().expect("pid");
    assert_ne!(first_pid, second_pid);
    assert!(!process_alive(first_pid), "crashed process was reaped");

    // The new process knows nothing about the documents until the caller resyncs.
    let ticket = sup.request("fake/state", json!({}), None).expect("request");
    let (generation, result) = events.response(ticket, T);
    assert_eq!(generation, 2);
    assert_eq!(result.expect("state")["openDocuments"], json!({}));
    let mark = events.mark();
    assert_eq!(sup.notify("textDocument/didOpen", did_open(uri, 1)), Ok(2));
    events.wait_notification(mark, T, "textDocument/publishDiagnostics", |g, p| {
        g == 2 && p["uri"] == uri && p["version"] == 1
    });

    let ticket = sup
        .request(
            "textDocument/completion",
            json!({ "after": "restart" }),
            None,
        )
        .expect("request");
    let (generation, result) = events.response(ticket, T);
    assert_eq!(generation, 2);
    assert_eq!(result.expect("echo")["params"]["after"], "restart");

    let metrics = sup.metrics();
    assert_eq!(metrics.starts, 2);
    assert_eq!(metrics.restarts, 1);
    assert_eq!(metrics.crashes, 1);
    assert_eq!(metrics.requests_failed, 2);
    let report = sup.shutdown();
    assert!(!report.forced_kill);
    assert!(!process_alive(second_pid));
}

#[test]
fn crash_loop_gives_up_after_max_restarts() {
    let mut config = config(&["--crash-after-initialize"]);
    config.max_restarts = 2;
    config.backoff_initial = Duration::from_millis(10);
    config.backoff_max = Duration::from_secs(1);
    let (sup, events) = start(config);
    let (_, state) = events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    match state {
        ServerState::Failed { reason } => {
            assert!(reason.contains("3 times"), "{reason}");
            assert!(reason.contains("exit status: 101"), "{reason}");
        }
        other => panic!("{other:?}"),
    }
    let restarting: Vec<ServerState> = events
        .snapshot()
        .into_iter()
        .filter_map(|e| match e {
            LspEvent::State { state, .. } if matches!(state, ServerState::Restarting { .. }) => {
                Some(state)
            }
            _ => None,
        })
        .collect();
    assert_eq!(
        restarting,
        vec![
            ServerState::Restarting {
                attempt: 1,
                retry_in: Duration::from_millis(10)
            },
            ServerState::Restarting {
                attempt: 2,
                retry_in: Duration::from_millis(20)
            },
        ]
    );
    let crashed: Vec<String> = events
        .snapshot()
        .into_iter()
        .filter_map(|e| match e {
            LspEvent::Crashed { stderr_tail, .. } => Some(stderr_tail),
            _ => None,
        })
        .collect();
    assert_eq!(crashed.len(), 3);
    assert!(crashed.iter().all(|t| t.contains("crash after initialize")));

    // No further attempts once failed.
    std::thread::sleep(Duration::from_millis(500));
    let metrics = sup.metrics();
    assert_eq!(metrics.starts, 3);
    assert_eq!(metrics.restarts, 2);
    assert_eq!(metrics.crashes, 3);
    assert_eq!(sup.generation(), 3);
    assert!(matches!(sup.state(), ServerState::Failed { .. }));
    assert_eq!(sup.pid(), None);
    assert_eq!(
        sup.request("textDocument/hover", json!({}), None),
        Err(LspError::NotRunning)
    );
    let report = sup.shutdown();
    assert!(!report.forced_kill);
}

#[test]
fn missing_executable_fails_quickly_without_retrying() {
    let command = LspCommand::new("/nonexistent/titan-no-such-language-server");
    let mut config = SupervisorConfig::new(command, env!("CARGO_MANIFEST_DIR"));
    config.backoff_initial = Duration::from_millis(10);
    let started = Instant::now();
    let (sup, events) = start(config);
    let (_, state) = events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    assert!(started.elapsed() < Duration::from_secs(5));
    match state {
        ServerState::Failed { reason } => {
            assert!(
                reason.contains("/nonexistent/titan-no-such-language-server"),
                "{reason}"
            );
        }
        other => panic!("{other:?}"),
    }
    std::thread::sleep(Duration::from_millis(200));
    let metrics = sup.metrics();
    assert_eq!(metrics.starts, 1);
    assert_eq!(metrics.spawn_failures, 1);
    assert_eq!(metrics.crashes, 0);
    assert_eq!(
        sup.request("textDocument/hover", json!({}), None),
        Err(LspError::NotRunning)
    );
    assert_eq!(
        sup.notify("textDocument/didOpen", json!({})),
        Err(LspError::NotRunning)
    );
    assert_eq!(events.count(|e| matches!(e, LspEvent::Crashed { .. })), 0);
    let started = Instant::now();
    sup.shutdown();
    assert!(started.elapsed() < Duration::from_secs(2));
}

#[test]
fn garbage_output_loses_the_connection_and_the_client_survives() {
    let mut config = config(&["--garbage-after-initialize"]);
    config.max_restarts = 1;
    let (sup, events) = start(config);
    events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    let reasons: Vec<(u64, String)> = events
        .snapshot()
        .into_iter()
        .filter_map(|e| match e {
            LspEvent::Crashed {
                generation, reason, ..
            } => Some((generation, reason)),
            _ => None,
        })
        .collect();
    assert_eq!(reasons.len(), 2, "{reasons:?}");
    assert_eq!(reasons[0].0, 1);
    assert_eq!(reasons[1].0, 2);
    for (_, reason) in &reasons {
        assert!(reason.contains("malformed JSON"), "{reason}");
        assert!(
            reason.contains("killed"),
            "the fake stays alive and must be killed: {reason}"
        );
    }
    let metrics = sup.metrics();
    assert_eq!(metrics.crashes, 2);
    assert_eq!(metrics.restarts, 1);
    assert_eq!(sup.pid(), None);
    sup.shutdown();
}

#[test]
fn malformed_header_loses_the_connection() {
    let mut config = config(&["--bad-header-after-initialize"]);
    config.max_restarts = 0;
    let (sup, events) = start(config);
    events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    match events.wait_for(T, "crash", |e| matches!(e, LspEvent::Crashed { .. })) {
        LspEvent::Crashed { reason, .. } => {
            assert!(reason.contains("framing error"), "{reason}");
        }
        other => panic!("{other:?}"),
    }
    sup.shutdown();
}

#[test]
fn oversize_message_loses_the_connection() {
    let mut config = config(&["--oversize-response", "200000"]);
    config.max_message_bytes = 64 * 1024;
    config.max_restarts = 0;
    let (sup, events) = start(config);
    events.wait_running(1, T);
    let pid = sup.pid().expect("pid");
    let ticket = sup
        .request("textDocument/hover", json!({}), None)
        .expect("request");
    match events.response(ticket, T).1 {
        Err(LspError::ConnectionLost(reason)) => {
            assert!(reason.contains("above the 65536-byte limit"), "{reason}");
        }
        other => panic!("expected ConnectionLost, got {other:?}"),
    }
    events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    assert!(!process_alive(pid));
    assert_eq!(sup.metrics().crashes, 1);
    sup.shutdown();
}

#[test]
fn graceful_shutdown_waits_for_exit_and_fails_unanswered_requests() {
    let (sup, events) = start(config(&[
        "--exit-on-shutdown-delay-ms",
        "100",
        "--hang-on",
        "textDocument/hover",
    ]));
    events.wait_running(1, T);
    let pid = sup.pid().expect("pid");
    let ticket = sup
        .request("textDocument/hover", json!({}), None)
        .expect("request");
    let started = Instant::now();
    let report = sup.shutdown();
    let elapsed = started.elapsed();
    assert!(report.shutdown_acknowledged);
    assert!(!report.forced_kill);
    assert_eq!(report.metrics.forced_kills, 0);
    assert!(elapsed >= Duration::from_millis(100), "{elapsed:?}");
    assert!(elapsed < Duration::from_secs(5), "{elapsed:?}");
    assert!(!process_alive(pid), "server process {pid} still exists");
    assert_eq!(events.response(ticket, T).1, Err(LspError::Stopped));
    let tail = states(&events, 1);
    assert_eq!(
        &tail[tail.len() - 2..],
        &[ServerState::ShuttingDown, ServerState::Stopped]
    );
}

#[test]
fn shutdown_kills_a_server_that_ignores_it() {
    let mut config = config(&["--ignore-shutdown"]);
    config.shutdown_timeout = Duration::from_millis(300);
    let (sup, events) = start(config);
    events.wait_running(1, T);
    let pid = sup.pid().expect("pid");
    let started = Instant::now();
    let report = sup.shutdown();
    let elapsed = started.elapsed();
    assert!(!report.shutdown_acknowledged);
    assert!(report.forced_kill);
    assert_eq!(report.metrics.forced_kills, 1);
    assert!(elapsed >= Duration::from_millis(300), "{elapsed:?}");
    assert!(elapsed < Duration::from_secs(5), "{elapsed:?}");
    assert!(!process_alive(pid), "server process {pid} still exists");
}

#[test]
fn dropping_the_supervisor_kills_the_server() {
    let (sup, events) = start(config(&["--ignore-shutdown"]));
    events.wait_running(1, T);
    let pid = sup.pid().expect("pid");
    assert!(process_alive(pid));
    drop(sup);
    assert!(!process_alive(pid), "server process {pid} still exists");
}

#[test]
fn callback_panics_are_contained() {
    let (events, record) = Events::new();
    let callback: titan_lsp::EventCallback = Box::new(move |event| {
        let panic_now = matches!(
            &event,
            LspEvent::State {
                state: ServerState::Initializing,
                ..
            }
        );
        record(event);
        assert!(!panic_now, "deliberate panic in the event callback");
    });
    let sup = Supervisor::start(config(&[]), callback);
    events.wait_running(1, T);
    assert_eq!(sup.metrics().callback_panics, 1);
    let ticket = sup.request("fake/echo", json!({}), None).expect("request");
    assert!(events.response(ticket, T).1.is_ok());
    sup.shutdown();
}

#[test]
fn initialize_timeout_counts_as_a_crash() {
    let mut config = config(&["--hang-on", "initialize"]);
    config.init_timeout = Duration::from_millis(300);
    config.max_restarts = 0;
    let (sup, events) = start(config);
    match events.wait_for(T, "crash", |e| matches!(e, LspEvent::Crashed { .. })) {
        LspEvent::Crashed { reason, .. } => {
            assert!(
                reason.contains("initialize was not answered within 300ms"),
                "{reason}"
            );
            assert!(reason.contains("killed"), "{reason}");
        }
        other => panic!("{other:?}"),
    }
    events.wait_state(T, "failed", |_, s| matches!(s, ServerState::Failed { .. }));
    assert!(!states(&events, 1).iter().any(ServerState::is_running));
    assert_eq!(sup.server_capabilities(), None);
    sup.shutdown();
}

#[test]
fn slow_answer_after_timeout_is_dropped() {
    let (sup, events) = start(config(&["--delay-ms", "400"]));
    events.wait_running(1, T);
    let ticket = sup
        .request(
            "textDocument/hover",
            json!({}),
            Some(Duration::from_millis(100)),
        )
        .expect("request");
    assert_eq!(events.response(ticket, T).1, Err(LspError::Timeout));
    // The fake sleeps, answers late, then reads the $/cancelRequest and acknowledges it.
    events.wait_notification(0, T, "fake/cancelled", |_, p| p["id"] == json!(ticket));
    let metrics = sup.metrics();
    assert_eq!(metrics.requests_timed_out, 1);
    assert_eq!(metrics.late_responses, 1);
    assert_eq!(events.responses_for(ticket), 1);
    // A request with enough time still succeeds.
    let ticket = sup
        .request(
            "textDocument/hover",
            json!({ "ok": true }),
            Some(Duration::from_secs(10)),
        )
        .expect("request");
    assert_eq!(
        events.response(ticket, T).1.expect("echo")["params"]["ok"],
        true
    );
    sup.shutdown();
}
