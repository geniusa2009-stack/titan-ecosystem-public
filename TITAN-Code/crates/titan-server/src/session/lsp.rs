//! Language-server integration: process supervision (via `titan-lsp`), document
//! synchronisation and semantic diagnostics.
//!
//! * The supervisor runs on its own threads; its events arrive here as [`Event::Lsp`].
//!   Sending to the server only queues bytes, so editing never waits for it.
//! * Documents are synchronised with the **buffer** (never the disk) of every open Rust
//!   document. The LSP `version` of a document is its TITAN revision, so every semantic
//!   answer can be tied to the exact revision it describes.
//! * A new server generation (first start or restart after a crash) has no documents open;
//!   [`Session::ensure_lsp_synced`] (run after every event and on every tick) sends
//!   `didOpen` for each open Rust document with its current text.
//! * Diagnostics carry the version the server computed them for; they are reported as
//!   current only when that version is the document's current revision.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;

use serde_json::{Value, json};
use titan_lsp::{LspCommand, LspEvent, ServerState, Supervisor, SupervisorConfig, path_to_uri};
use titan_protocol::messages::{
    Diagnostic, PositionEncoding, SemanticDiagnosticsParams, notifications,
};
use titan_text::{Change, TextBuffer, TextSnapshot};

use super::intel::{Query, lsp_range_to_client};
use super::{Session, wire_changes};
use crate::{Event, LspOptions};

/// LSP `languageId` for Rust documents.
const LANGUAGE_ID: &str = "rust";

pub(super) struct SyncedDoc {
    pub generation: u64,
    pub version: u64,
}

pub(super) struct LspPending {
    pub id: Option<Value>,
    pub key: Option<String>,
    pub query: Query,
    pub doc_id: Option<u64>,
    pub revision: Option<u64>,
    pub generation: u64,
    pub started: Instant,
    /// Syntax-level evidence computed when the request arrived, sent with the answer.
    pub syntax: Vec<titan_protocol::messages::Evidence>,
}

/// Last diagnostics the server published for a document.
pub(super) struct StoredDiagnostics {
    pub version: Option<u64>,
    pub generation: u64,
    pub received_at_revision: u64,
    pub items: Vec<Value>,
}

/// Milestones of the current server generation, in ms since its start.
#[derive(Clone, Debug, Default)]
pub(super) struct ColdStart {
    pub started: Option<Instant>,
    pub initialized_ms: Option<u64>,
    pub quiescent_ms: Option<u64>,
    pub first_diagnostics_ms: Option<u64>,
    /// method → (ms since start when answered, request latency ms).
    pub first_answers: std::collections::BTreeMap<&'static str, (u64, u64)>,
}

impl ColdStart {
    pub fn since(&self) -> Option<u64> {
        self.started
            .map(|t| u64::try_from(t.elapsed().as_millis()).unwrap_or(u64::MAX))
    }

    pub fn json(&self) -> Value {
        json!({
            "initializedMs": self.initialized_ms,
            "readyMs": self.quiescent_ms,
            "firstDiagnosticsMs": self.first_diagnostics_ms,
            "firstAnswers": self.first_answers.iter().map(|(m, (at, latency))| {
                (m.to_string(), json!({ "atMs": at, "latencyMs": latency }))
            }).collect::<serde_json::Map<_, _>>(),
        })
    }
}

#[derive(Default)]
pub(super) struct Semantic {
    pub supervisor: Option<Supervisor>,
    pub name: Option<String>,
    pub state: Option<ServerState>,
    pub generation: u64,
    pub synced: HashMap<u64, SyncedDoc>,
    /// Documents whose synchronisation broke (queue full): close and reopen.
    pub resync: Vec<u64>,
    pub pending: HashMap<u64, LspPending>,
    pub by_key: HashMap<String, u64>,
    pub diagnostics: HashMap<u64, StoredDiagnostics>,
    pub crashes: u64,
    pub last_crash: Option<String>,
    pub started_at: Option<Instant>,
    pub cold: ColdStart,
    /// Multi-request jobs (impact analysis, rename) in flight.
    pub jobs: HashMap<u64, super::impact::Job>,
    pub next_job: u64,
}

impl Semantic {
    pub fn quiescent(&self) -> Option<bool> {
        match &self.state {
            Some(ServerState::Running { quiescent }) => *quiescent,
            _ => None,
        }
    }

    pub fn running(&self) -> bool {
        self.state.as_ref().is_some_and(ServerState::is_running)
    }

    pub fn state_name(&self) -> &'static str {
        match &self.state {
            None if self.name.is_none() => "disabled",
            None => "stopped",
            Some(ServerState::Starting | ServerState::Initializing) => "starting",
            Some(ServerState::Running {
                quiescent: Some(true),
            }) => "ready",
            Some(ServerState::Running { .. }) => "indexing",
            Some(ServerState::Restarting { .. }) => "restarting",
            Some(ServerState::ShuttingDown) => "shuttingDown",
            Some(ServerState::Failed { .. }) => "failed",
            Some(ServerState::Stopped) => "stopped",
        }
    }

    pub fn state_reason(&self) -> Option<String> {
        match &self.state {
            Some(ServerState::Failed { reason }) => Some(reason.clone()),
            Some(ServerState::Restarting { attempt, retry_in }) => Some(format!(
                "restarting after crash {attempt} in {} ms",
                retry_in.as_millis()
            )),
            _ => self.last_crash.clone(),
        }
    }
}

pub(super) fn uri_to_path(uri: &str) -> Option<PathBuf> {
    let rest = uri.strip_prefix("file://")?;
    let bytes = rest.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    let text = String::from_utf8(out).ok()?;
    #[cfg(windows)]
    let text = text.trim_start_matches('/').to_string();
    Some(PathBuf::from(text))
}

/// rust-analyzer settings used by `titand` unless overridden: semantic analysis without
/// running `cargo check`, build scripts or proc-macro expansion (fast, no code execution).
pub(crate) fn default_rust_analyzer_settings() -> Value {
    json!({
        "checkOnSave": false,
        "cargo": { "buildScripts": { "enable": false } },
        "procMacro": { "enable": false },
        // Measured (benchmarks/results/2026-11-10-phase4-*): priming moves the first
        // definition from ~6 s (lazy analysis) to ~25 ms, at the price of a later "ready".
        "cachePriming": { "enable": true },
        // Type mismatches and similar checks are "experimental" native diagnostics.
        "diagnostics": { "experimental": { "enable": true } },
    })
}

pub(crate) const LIGHT_MODE_LIMITATION: &str = "rust-analyzer runs without cargo check, build scripts and proc-macro expansion: \
     macro-generated items and compiler-only diagnostics are missing";

impl Session {
    pub(super) fn start_language_server(&mut self, root: &Path) {
        self.stop_language_server(false);
        let Some(options) = self.options.lsp.clone() else {
            return;
        };
        let LspOptions {
            program,
            args,
            settings,
            request_timeout,
            name,
            ..
        } = options;
        let mut config = SupervisorConfig::new(LspCommand::new(program).args(args).cwd(root), root);
        config.initialization_options = settings.clone();
        config.workspace_configuration = json!({ "rust-analyzer": settings });
        config.request_timeout = request_timeout;
        let events = self.events.clone();
        let supervisor = Supervisor::start(
            config,
            Box::new(move |event| {
                let _ = events.send(Event::Lsp(Box::new(event)));
            }),
        );
        self.semantic.name = Some(name);
        self.semantic.state = Some(supervisor.state());
        self.semantic.generation = supervisor.generation();
        self.semantic.supervisor = Some(supervisor);
        self.semantic.started_at = Some(Instant::now());
        self.semantic.cold = ColdStart {
            started: Some(Instant::now()),
            ..ColdStart::default()
        };
    }

    /// Stop the server. `wait`: block until it exited (process shutdown); otherwise the
    /// shutdown runs on a helper thread so the event loop does not wait for it.
    pub(super) fn stop_language_server(&mut self, wait: bool) {
        let pending: Vec<Value> = self
            .semantic
            .pending
            .drain()
            .filter_map(|(_, p)| p.id)
            .collect();
        for id in pending {
            self.respond(
                id,
                Err(super::rpc_error(
                    titan_protocol::rpc::codes::REQUEST_CANCELLED,
                    "the language server was stopped",
                )),
            );
        }
        let job_ids: Vec<Value> = self
            .semantic
            .jobs
            .drain()
            .filter_map(|(_, job)| job.request_id())
            .collect();
        for id in job_ids {
            self.respond(
                id,
                Err(super::rpc_error(
                    titan_protocol::rpc::codes::REQUEST_CANCELLED,
                    "the language server was stopped",
                )),
            );
        }
        let name = self.semantic.name.clone();
        let old = std::mem::take(&mut self.semantic);
        self.semantic.name = name.filter(|_| self.options.lsp.is_some());
        if let Some(supervisor) = old.supervisor {
            if wait {
                let report = supervisor.shutdown();
                if report.forced_kill {
                    eprintln!("titand: the language server had to be killed at shutdown");
                }
            } else {
                let _ = std::thread::Builder::new()
                    .name("lsp-shutdown".to_string())
                    .spawn(move || supervisor.shutdown());
            }
        }
    }

    /// `intel.server`: explicit lifecycle control of the language server.
    pub(super) fn intel_server(&mut self, params: Value) -> Result<Value, super::ServerError> {
        let action = params["action"].as_str().unwrap_or_default().to_string();
        let root = self.workspace()?.root().to_path_buf();
        if self.options.lsp.is_none() {
            return Err(super::rpc_error(
                titan_protocol::rpc::codes::SEMANTIC_UNAVAILABLE,
                "no language server is configured",
            ));
        }
        match action.as_str() {
            "stop" => self.stop_language_server(false),
            "start" if self.semantic.supervisor.is_none() => self.start_language_server(&root),
            "start" => {}
            "restart" => self.start_language_server(&root),
            other => {
                return Err(super::rpc_error(
                    titan_protocol::rpc::codes::INVALID_PARAMS,
                    format!("unknown action {other:?}; use stop, start or restart"),
                ));
            }
        }
        Ok(json!({ "action": action, "state": self.semantic.state_name() }))
    }

    pub(crate) fn on_lsp_event(&mut self, event: LspEvent) {
        match event {
            LspEvent::State { generation, state } => {
                let old_state = self.semantic.state_name();
                if generation != self.semantic.generation {
                    // A fresh process: nothing is open in it; its cold start is measured anew.
                    self.semantic.synced.clear();
                    self.semantic.resync.clear();
                    self.semantic.cold = ColdStart {
                        started: Some(Instant::now()),
                        ..ColdStart::default()
                    };
                }
                if let ServerState::Running { quiescent } = &state {
                    let at = self.semantic.cold.since();
                    self.semantic
                        .cold
                        .initialized_ms
                        .get_or_insert(at.unwrap_or(0));
                    if *quiescent == Some(true) {
                        self.semantic
                            .cold
                            .quiescent_ms
                            .get_or_insert(at.unwrap_or(0));
                    }
                }
                self.semantic.generation = generation;
                self.semantic.state = Some(state);
                if old_state != self.semantic.state_name() {
                    self.push_intel_state();
                }
            }
            LspEvent::Notification {
                generation,
                method,
                params,
            } => {
                if method == "textDocument/publishDiagnostics" {
                    if generation == self.semantic.generation {
                        let at = self.semantic.cold.since().unwrap_or(0);
                        self.semantic.cold.first_diagnostics_ms.get_or_insert(at);
                    }
                    self.on_publish_diagnostics(generation, &params);
                }
            }
            LspEvent::Response {
                ticket,
                generation,
                result,
            } => self.on_lsp_response(ticket, generation, result),
            LspEvent::Crashed {
                generation,
                reason,
                stderr_tail,
            } => {
                self.semantic.crashes += 1;
                let tail: String = stderr_tail
                    .lines()
                    .rev()
                    .take(5)
                    .collect::<Vec<_>>()
                    .into_iter()
                    .rev()
                    .collect::<Vec<_>>()
                    .join(" | ");
                eprintln!("titand: language server generation {generation} crashed: {reason}");
                self.semantic.last_crash = Some(if tail.is_empty() {
                    reason
                } else {
                    format!("{reason} (stderr: {tail})")
                });
                self.semantic.synced.clear();
                self.push_intel_state();
            }
        }
        self.ensure_lsp_synced();
    }

    fn push_intel_state(&mut self) {
        if !self.caps.semantic_notifications {
            return;
        }
        let params = json!({
            "server": self.semantic.name,
            "state": self.semantic.state_name(),
            "generation": self.semantic.generation,
            "quiescent": self.semantic.quiescent(),
            "crashes": self.semantic.crashes,
            "reason": self.semantic.state_reason(),
        });
        self.push_notification(notifications::INTEL_STATE, params);
    }

    /// `didOpen` every open Rust document that the current server generation does not have
    /// yet (and close/reopen documents whose synchronisation broke).
    pub(super) fn ensure_lsp_synced(&mut self) {
        if !self.semantic.running() {
            return;
        }
        let generation = self.semantic.generation;
        for doc_id in std::mem::take(&mut self.semantic.resync) {
            if let (Some(sup), Some(od)) = (&self.semantic.supervisor, self.documents.get(&doc_id))
            {
                let uri = path_to_uri(od.doc.path());
                let _ = sup.notify(
                    "textDocument/didClose",
                    json!({ "textDocument": { "uri": uri } }),
                );
            }
            self.semantic.synced.remove(&doc_id);
        }
        let missing: Vec<u64> = self
            .documents
            .iter()
            .filter(|(id, od)| {
                od.is_rust
                    && self
                        .semantic
                        .synced
                        .get(id)
                        .is_none_or(|s| s.generation != generation)
            })
            .map(|(id, _)| *id)
            .collect();
        for doc_id in missing {
            let Some(od) = self.documents.get(&doc_id) else {
                continue;
            };
            let Some(sup) = &self.semantic.supervisor else {
                return;
            };
            let version = od.doc.text().revision();
            let params = json!({ "textDocument": {
                "uri": path_to_uri(od.doc.path()),
                "languageId": LANGUAGE_ID,
                "version": version,
                "text": od.doc.text().buffer().text(),
            }});
            if sup.notify("textDocument/didOpen", params).is_ok() {
                self.semantic.synced.insert(
                    doc_id,
                    SyncedDoc {
                        generation,
                        version,
                    },
                );
            }
        }
    }

    /// Forward a committed change (`didChange`, incremental, UTF-16).
    pub(super) fn lsp_did_change(&mut self, doc_id: u64, before: &TextSnapshot, change: &Change) {
        let generation = self.semantic.generation;
        let Some(synced) = self.semantic.synced.get_mut(&doc_id) else {
            return;
        };
        if synced.generation != generation {
            return;
        }
        let (Some(sup), Some(od)) = (&self.semantic.supervisor, self.documents.get(&doc_id)) else {
            return;
        };
        let incremental = sup
            .server_capabilities()
            .map(|caps| sync_kind(&caps) == 2)
            .unwrap_or(false)
            && sup.position_encoding() == "utf-16";
        let content_changes = if incremental {
            match wire_changes(PositionEncoding::Utf16, before.buffer(), &change.edits) {
                // Edits are sorted and do not overlap; applying them last-first keeps every
                // range valid for the sequential application LSP specifies.
                Ok(edits) => edits
                    .into_iter()
                    .rev()
                    .map(|e| json!({ "range": e.range, "text": e.text }))
                    .collect(),
                Err(_) => vec![json!({ "text": od.doc.text().buffer().text() })],
            }
        } else {
            vec![json!({ "text": od.doc.text().buffer().text() })]
        };
        let params = json!({
            "textDocument": { "uri": path_to_uri(od.doc.path()), "version": change.revision },
            "contentChanges": content_changes,
        });
        match sup.notify("textDocument/didChange", params) {
            Ok(_) => synced.version = change.revision,
            Err(_) => self.semantic.resync.push(doc_id),
        }
    }

    pub(super) fn lsp_did_save(&mut self, doc_id: u64) {
        self.lsp_document_notification(doc_id, "textDocument/didSave");
    }

    pub(super) fn lsp_did_close(&mut self, doc_id: u64) {
        if self.semantic.synced.contains_key(&doc_id) {
            self.lsp_document_notification(doc_id, "textDocument/didClose");
        }
        self.semantic.synced.remove(&doc_id);
        self.semantic.diagnostics.remove(&doc_id);
    }

    fn lsp_document_notification(&mut self, doc_id: u64, method: &str) {
        let generation = self.semantic.generation;
        if self
            .semantic
            .synced
            .get(&doc_id)
            .is_none_or(|s| s.generation != generation)
        {
            return;
        }
        if let (Some(sup), Some(od)) = (&self.semantic.supervisor, self.documents.get(&doc_id)) {
            let _ = sup.notify(
                method,
                json!({ "textDocument": { "uri": path_to_uri(od.doc.path()) } }),
            );
        }
    }

    /// `workspace/didChangeWatchedFiles` for files that are not open (open documents are
    /// synchronised from their buffers).
    pub(super) fn lsp_watched_files(&mut self, changes: &[(PathBuf, u8)]) {
        if !self.semantic.running() || changes.is_empty() {
            return;
        }
        let Some(sup) = &self.semantic.supervisor else {
            return;
        };
        let changes: Vec<Value> = changes
            .iter()
            .filter(|(path, _)| !self.by_path.contains_key(path))
            .map(|(path, kind)| json!({ "uri": path_to_uri(path), "type": kind }))
            .collect();
        if !changes.is_empty() {
            let _ = sup.notify(
                "workspace/didChangeWatchedFiles",
                json!({ "changes": changes }),
            );
        }
    }

    fn doc_for_uri(&self, uri: &str) -> Option<u64> {
        let path = uri_to_path(uri)?;
        self.by_path.get(&path).copied()
    }

    fn on_publish_diagnostics(&mut self, generation: u64, params: &Value) {
        if generation != self.semantic.generation {
            return;
        }
        let Some(doc_id) = params["uri"].as_str().and_then(|u| self.doc_for_uri(u)) else {
            return;
        };
        let Some(od) = self.documents.get(&doc_id) else {
            return;
        };
        let current_revision = od.doc.text().revision();
        let version = params["version"].as_u64();
        let items = params["diagnostics"]
            .as_array()
            .cloned()
            .unwrap_or_default();
        self.semantic.diagnostics.insert(
            doc_id,
            StoredDiagnostics {
                version,
                generation,
                received_at_revision: current_revision,
                items,
            },
        );
        if self.caps.semantic_notifications {
            if let Some(params) = self.semantic_diagnostics_params(doc_id) {
                self.push_notification(notifications::SEMANTIC_DIAGNOSTICS, params);
            }
        }
    }

    /// The stored semantic diagnostics of a document in the client's position encoding.
    pub(super) fn semantic_diagnostics_params(
        &self,
        doc_id: u64,
    ) -> Option<SemanticDiagnosticsParams> {
        let stored = self.semantic.diagnostics.get(&doc_id)?;
        let od = self.documents.get(&doc_id)?;
        let current_revision = od.doc.text().revision();
        let current = stored.version == Some(current_revision)
            && stored.generation == self.semantic.generation;
        let mut cache = HashMap::new();
        let diagnostics = stored
            .items
            .iter()
            .filter_map(|d| {
                let range = lsp_range_to_client(self, od.doc.path(), &d["range"], &mut cache)?;
                Some(Diagnostic {
                    range,
                    severity: match d["severity"].as_u64() {
                        Some(1) => "error",
                        Some(2) => "warning",
                        Some(3) => "information",
                        Some(4) => "hint",
                        _ => "error",
                    }
                    .to_string(),
                    message: d["message"].as_str().unwrap_or_default().to_string(),
                })
            })
            .collect();
        Some(SemanticDiagnosticsParams {
            document_id: doc_id,
            source: self.semantic.name.clone().unwrap_or_default(),
            revision: stored.version,
            received_at_revision: stored.received_at_revision,
            current,
            generation: stored.generation,
            diagnostics,
        })
    }

    pub(super) fn lsp_metrics(&self) -> Value {
        let metrics = self.semantic.supervisor.as_ref().map(Supervisor::metrics);
        json!({
            "server": self.semantic.name,
            "state": self.semantic.state_name(),
            "generation": self.semantic.generation,
            "quiescent": self.semantic.quiescent(),
            "pid": self.semantic.supervisor.as_ref().and_then(Supervisor::pid),
            "syncedDocuments": self.semantic.synced.len(),
            "pendingRequests": self.semantic.pending.len(),
            "crashes": self.semantic.crashes,
            "lastCrash": self.semantic.last_crash,
            "uptimeMs": self.semantic.started_at.map(|t| t.elapsed().as_millis() as u64),
            "coldStart": self.semantic.cold.json(),
            "process": metrics.map(|m| json!({
                "starts": m.starts,
                "restarts": m.restarts,
                "crashes": m.crashes,
                "spawnFailures": m.spawn_failures,
                "requestsSent": m.requests_sent,
                "requestsCompleted": m.requests_completed,
                "requestsTimedOut": m.requests_timed_out,
                "requestsCancelled": m.requests_cancelled,
                "requestsFailed": m.requests_failed,
                "lateResponses": m.late_responses,
                "notificationsIn": m.notifications_in,
                "notificationsOut": m.notifications_out,
                "bytesIn": m.bytes_in,
                "bytesOut": m.bytes_out,
                "forcedKills": m.forced_kills,
                "lastError": m.last_error,
            })),
        })
    }
}

/// `textDocumentSync` kind from server capabilities (0 none, 1 full, 2 incremental).
fn sync_kind(caps: &Value) -> u64 {
    match &caps["textDocumentSync"] {
        Value::Number(n) => n.as_u64().unwrap_or(1),
        Value::Object(o) => o.get("change").and_then(Value::as_u64).unwrap_or(1),
        _ => 1,
    }
}

/// The text of `buffer` as a fresh buffer (helper for position conversion caches).
pub(super) fn buffer_for(path: &Path, limit: u64) -> Option<TextBuffer> {
    let bytes = std::fs::read(path).ok()?;
    if bytes.len() as u64 > limit {
        return None;
    }
    let text = String::from_utf8(bytes).ok()?;
    Some(TextBuffer::new(
        text.strip_prefix('\u{feff}').unwrap_or(&text),
    ))
}
