//! Verified Changes service: propose → preview → approve → apply → verify → restore, with an
//! evidence record per proposal (`titan-changes` + `titan-verify`).
//!
//! * Proposals are validated against what the editor sees: open documents through their
//!   buffers (expected revision + hash), other files on disk (expected hash).
//! * Applying always needs an explicit `changes.approve` first, and re-validates: a
//!   proposal computed for an older revision or disk version is rejected, nothing written.
//! * Disk files are changed by a journaled transaction (staged originals, conflict-checked
//!   atomic replacement, rollback on failure, crash recovery on the next `workspace.open`);
//!   open documents get the edits as one undoable transaction each, never saved implicitly.
//!   Multi-file application is not atomic; the journal makes it recoverable.
//! * `changes.verify` runs explicitly allowed checks (cargo check/test/...) on base and
//!   candidate in isolated git worktrees, under a trust policy that refuses build scripts and
//!   proc macros by default; results are recorded in the evidence as passed / failed /
//!   not run / timed out. A passing check is evidence for that check only.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use serde_json::{Value, json};
use titan_changes::{
    BaseSnapshot, ChangeAnnotation, CheckRecord, CheckStatus as EvidenceStatus, Current,
    EvidenceRecord, FileChange, Proposal, ProposalSource, ProposalStatus, ViewError, WorkspaceView,
    resolve_confined,
};
use titan_doc::{Limits, read_disk};
use titan_protocol::rpc::codes;
use titan_tasks::Priority;
use titan_text::{ByteOffset, TextEdit};

use super::{Pending, Reply, ServerError, Session, TaskResult, rpc_error, to_json, to_json_rpc};

const MAX_PROPOSALS: usize = 200;

/// What `titand` keeps per proposal.
pub(super) struct Stored {
    pub proposal: Proposal,
    pub evidence: Option<EvidenceRecord>,
    /// Open documents edited by the apply: (document id, revision after the edit).
    pub buffer_revisions: Vec<(u64, u64)>,
    /// Graph identity to carry over when a rename proposal is applied.
    pub renamed_symbol: Option<(titan_index::graph::NodeKey, String)>,
}

#[derive(Default)]
pub(super) struct ChangesState {
    pub proposals: BTreeMap<String, Stored>,
    pub recovered: Option<Value>,
}

/// The editor's view of the workspace for validation and application.
pub(super) struct SessionView<'a> {
    pub session: &'a Session,
    pub root: PathBuf,
    pub limits: Limits,
}

impl WorkspaceView for SessionView<'_> {
    fn root(&self) -> &Path {
        &self.root
    }

    fn resolve(&self, rel: &Path) -> Result<PathBuf, ViewError> {
        resolve_confined(&self.root, rel)
    }

    fn current(&self, abs: &Path) -> Result<Current, ViewError> {
        if let Some(&id) = self.session.by_path.get(abs) {
            if let Some(od) = self.session.documents.get(&id) {
                return Ok(Current::OpenBuffer {
                    revision: od.doc.text().revision(),
                    text: od.doc.text().buffer().text(),
                });
            }
        }
        match read_disk(abs, &self.limits) {
            Ok(s) if !s.state.exists => Ok(Current::Missing),
            Ok(s) => Ok(Current::Disk {
                hash: s.state.hash,
                bytes: s.bytes,
            }),
            Err(e) => Err(ViewError::Unreadable {
                path: abs.to_path_buf(),
                reason: e.to_string(),
            }),
        }
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ProposeParams {
    #[serde(default)]
    source: Option<ProposalSource>,
    changes: Vec<FileChange>,
    #[serde(default)]
    preconditions: Vec<String>,
    #[serde(default)]
    expected_result: Option<String>,
    #[serde(default)]
    annotations: Vec<ChangeAnnotation>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct IdParams {
    proposal_id: String,
    #[serde(default)]
    by: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct VerifyParams {
    proposal_id: String,
    checks: Vec<titan_verify::Check>,
    #[serde(default)]
    policy: titan_verify::TrustPolicy,
    #[serde(default)]
    timeout_ms: Option<u64>,
}

pub(super) fn unix_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// `git rev-parse HEAD` without running git: read `.git/HEAD` and the ref it names.
fn git_head(root: &Path) -> Option<String> {
    let git = root.join(".git");
    let head = std::fs::read_to_string(git.join("HEAD")).ok()?;
    let head = head.trim();
    match head.strip_prefix("ref: ") {
        Some(r) => std::fs::read_to_string(git.join(r))
            .ok()
            .map(|s| s.trim().to_string())
            .or_else(|| {
                let packed = std::fs::read_to_string(git.join("packed-refs")).ok()?;
                packed
                    .lines()
                    .find(|l| l.ends_with(r))
                    .and_then(|l| l.split_whitespace().next())
                    .map(str::to_string)
            }),
        None => Some(head.to_string()),
    }
}

fn status_name(status: &ProposalStatus) -> &'static str {
    match status {
        ProposalStatus::Proposed => "proposed",
        ProposalStatus::Validated => "validated",
        ProposalStatus::Rejected { .. } => "rejected",
        ProposalStatus::Approved { .. } => "approved",
        ProposalStatus::Applied => "applied",
        ProposalStatus::PartiallyApplied => "partiallyApplied",
        ProposalStatus::RolledBack => "rolledBack",
        ProposalStatus::Failed { .. } => "failed",
        ProposalStatus::Superseded => "superseded",
    }
}

impl Session {
    pub(super) fn view(&self) -> Result<SessionView<'_>, ServerError> {
        let root = self.workspace()?.root().to_path_buf();
        Ok(SessionView {
            session: self,
            root,
            limits: self.options.limits,
        })
    }

    fn state_dir(&self) -> Result<PathBuf, ServerError> {
        self.options.state_dir.clone().ok_or_else(|| {
            rpc_error(
                codes::RECOVERY_UNAVAILABLE,
                "Verified Changes needs a state directory (start titand with --state-dir)",
            )
        })
    }

    /// Finish interrupted transactions of earlier sessions (on `workspace.open`).
    pub(super) fn recover_changes(&mut self) {
        let Some(state) = self.options.state_dir.clone() else {
            return;
        };
        match titan_changes::recover(&state) {
            Ok(report) => self.changes.recovered = serde_json::to_value(&report).ok(),
            Err(e) => eprintln!("titand: could not recover change transactions: {e}"),
        }
    }

    /// Store a proposal after validating it; returns proposal + validation (with diffs).
    pub(super) fn store_proposal(
        &mut self,
        mut proposal: Proposal,
        renamed_symbol: Option<(titan_index::graph::NodeKey, String)>,
    ) -> Result<Value, ServerError> {
        let report = titan_changes::validate(&proposal, &self.view()?);
        proposal.status = if report.ok {
            ProposalStatus::Validated
        } else {
            ProposalStatus::Rejected {
                problems: report.problems.clone(),
            }
        };
        if self.changes.proposals.len() >= MAX_PROPOSALS {
            if let Some(oldest) = self.changes.proposals.keys().next().cloned() {
                self.changes.proposals.remove(&oldest);
            }
        }
        let id = proposal.id.as_str().to_string();
        let value = json!({
            "proposal": proposal,
            "status": status_name(&proposal.status),
            "validation": report,
        });
        self.changes.proposals.insert(
            id,
            Stored {
                proposal,
                evidence: None,
                buffer_revisions: Vec::new(),
                renamed_symbol,
            },
        );
        Ok(value)
    }

    pub(super) fn changes_propose(&mut self, params: Value) -> Result<Value, ServerError> {
        let p: ProposeParams = super::parse(params)?;
        let root = self.workspace()?.root().to_path_buf();
        let mut proposal = Proposal::new(
            p.source.unwrap_or(ProposalSource {
                kind: "developer".to_string(),
                detail: Value::Null,
            }),
            BaseSnapshot {
                git_head: git_head(&root),
                workspace_root: root,
            },
            p.changes,
        );
        proposal.preconditions = p.preconditions;
        proposal.expected_result = p.expected_result;
        proposal.annotations = p.annotations;
        self.store_proposal(proposal, None)
    }

    fn stored(&self, id: &str) -> Result<&Stored, ServerError> {
        self.changes
            .proposals
            .get(id)
            .ok_or_else(|| rpc_error(codes::INVALID_PARAMS, format!("unknown proposal {id:?}")))
    }

    pub(super) fn changes_preview(&self, params: Value) -> Result<Value, ServerError> {
        let p: IdParams = super::parse(params)?;
        let stored = self.stored(&p.proposal_id)?;
        let report = titan_changes::validate(&stored.proposal, &self.view()?);
        to_json(json!({
            "proposalId": p.proposal_id,
            "status": status_name(&stored.proposal.status),
            "validation": report,
            "note": "read-only: nothing was written",
        }))
    }

    pub(super) fn changes_get(&self, params: Value) -> Result<Value, ServerError> {
        let p: IdParams = super::parse(params)?;
        let stored = self.stored(&p.proposal_id)?;
        Ok(json!({
            "proposal": stored.proposal,
            "status": status_name(&stored.proposal.status),
            "evidence": stored.evidence,
        }))
    }

    pub(super) fn changes_list(&self) -> Value {
        json!({
            "proposals": self.changes.proposals.values().map(|s| json!({
                "proposalId": s.proposal.id.as_str(),
                "source": s.proposal.source.kind,
                "status": status_name(&s.proposal.status),
                "changes": s.proposal.changes.len(),
                "patchId": s.proposal.patch_id(),
            })).collect::<Vec<_>>(),
            "recoveredAtOpen": self.changes.recovered,
        })
    }

    pub(super) fn changes_approve(&mut self, params: Value) -> Result<Value, ServerError> {
        let p: IdParams = super::parse(params)?;
        let by = p.by.unwrap_or_else(|| "client".to_string());
        let stored = self
            .changes
            .proposals
            .get_mut(&p.proposal_id)
            .ok_or_else(|| rpc_error(codes::INVALID_PARAMS, "unknown proposal"))?;
        match stored.proposal.status {
            ProposalStatus::Validated | ProposalStatus::Proposed => {}
            ref other => {
                return Err(rpc_error(
                    codes::INVALID_PARAMS,
                    format!("a {} proposal cannot be approved", status_name(other)),
                ));
            }
        }
        stored.proposal.status = ProposalStatus::Approved {
            by,
            at_unix_ms: unix_ms(),
        };
        Ok(json!({ "proposalId": p.proposal_id, "status": "approved" }))
    }

    pub(super) fn changes_apply(&mut self, params: Value) -> Result<Value, ServerError> {
        let p: IdParams = super::parse(params)?;
        let state = self.state_dir()?;
        let proposal = self.stored(&p.proposal_id)?.proposal.clone();
        if proposal.approval().is_none() {
            return Err(rpc_error(
                codes::INVALID_PARAMS,
                "apply needs an explicit changes.approve first",
            ));
        }
        let result = titan_changes::apply(&proposal, &self.view()?, &state);
        let report = match result {
            Ok(report) => report,
            Err(titan_changes::ApplyError::Invalid(validation)) => {
                // Stale or invalid now: reject, write nothing.
                if let Some(s) = self.changes.proposals.get_mut(&p.proposal_id) {
                    s.proposal.status = ProposalStatus::Rejected {
                        problems: validation.problems.clone(),
                    };
                }
                return Err(super::ServerError::Rpc(
                    titan_protocol::rpc::RpcError::new(
                        codes::STALE_REVISION,
                        "the proposal no longer matches the workspace; nothing was written",
                    )
                    .with_data(json!({ "validation": validation })),
                ));
            }
            Err(e) => return Err(rpc_error(codes::IO_ERROR, e.to_string())),
        };
        // Buffer edits: one undoable transaction per open document, at its expected revision.
        let mut buffer_revisions = Vec::new();
        let mut buffer_failures = Vec::new();
        for edits in &report.buffer_edits {
            let Some(&doc_id) = self.by_path.get(&edits.abs_path) else {
                buffer_failures.push(format!("{} is no longer open", edits.path));
                continue;
            };
            let Some(od) = self.documents.get_mut(&doc_id) else {
                continue;
            };
            let before = od.doc.text().snapshot();
            let text_edits: Vec<TextEdit> = edits
                .edits
                .iter()
                .map(|e| TextEdit::replace(ByteOffset(e.start), ByteOffset(e.end), e.text.clone()))
                .collect();
            match od.doc.text_mut().apply(edits.expected_revision, text_edits) {
                Ok(change) => {
                    buffer_revisions.push((doc_id, change.revision));
                    self.after_change(doc_id, &before, &change);
                }
                Err(e) => buffer_failures.push(format!("{}: {e}", edits.path)),
            }
        }
        // Disk files that changed: re-index, invalidate the graph.
        for affected in &report.affected {
            if let Ok(abs) = resolve_confined(self.workspace()?.root(), Path::new(&affected.path)) {
                if !self.by_path.contains_key(&abs) && super::is_rust_file(&abs) {
                    self.reindex_from_disk(abs);
                }
            }
        }
        self.mark_graph_stale();
        let mut evidence = EvidenceRecord::from_apply(&proposal, &report);
        evidence.approval = proposal.approval();
        evidence.notes.extend(
            buffer_failures
                .iter()
                .map(|f| format!("buffer edit not applied: {f}")),
        );
        // Checks run before the apply (changes.verify) belong to this evidence.
        let earlier: Vec<CheckRecord> = self
            .changes
            .proposals
            .get(&p.proposal_id)
            .and_then(|s| s.evidence.as_ref())
            .map(|e| e.checks.clone())
            .unwrap_or_default();
        if earlier.is_empty() {
            evidence.checks.push(CheckRecord::not_run(
                "verification",
                "no checks requested yet (use changes.verify)",
            ));
        } else {
            evidence
                .notes
                .push("checks were run on the candidate before it was applied".to_string());
            evidence.checks.extend(earlier);
        }
        let evidence_path = titan_changes::write_evidence(&state, &evidence)
            .map_err(|e| rpc_error(codes::IO_ERROR, e.to_string()))?;
        let renamed = self
            .changes
            .proposals
            .get(&p.proposal_id)
            .and_then(|s| s.renamed_symbol.clone());
        if let (Some((key, new_name)), titan_changes::Outcome::Applied) = (renamed, report.outcome)
        {
            if let Some(graph) = self.graph.graph.as_mut() {
                graph.rename_symbol(&key, &new_name);
            }
        }
        if let Some(s) = self.changes.proposals.get_mut(&p.proposal_id) {
            s.proposal.status = report.status();
            s.evidence = Some(evidence.clone());
            s.buffer_revisions = buffer_revisions;
        }
        Ok(json!({
            "proposalId": p.proposal_id,
            "outcome": report.outcome,
            "report": report,
            "bufferFailures": buffer_failures,
            "evidence": evidence,
            "evidencePath": evidence_path,
        }))
    }

    pub(super) fn changes_restore(&mut self, params: Value) -> Result<Value, ServerError> {
        let p: IdParams = super::parse(params)?;
        let state = self.state_dir()?;
        let stored = self.stored(&p.proposal_id)?;
        let evidence = stored
            .evidence
            .clone()
            .ok_or_else(|| rpc_error(codes::INVALID_PARAMS, "the proposal was not applied"))?;
        let buffers = stored.buffer_revisions.clone();
        // Buffers first: undo only if nothing changed since the apply.
        let mut buffer_refused = Vec::new();
        for (doc_id, revision) in &buffers {
            let current = self
                .documents
                .get(doc_id)
                .map(|od| od.doc.text().revision());
            if current != Some(*revision) {
                buffer_refused.push(
                    json!({ "documentId": doc_id, "detail": "edited since the apply; not undone" }),
                );
            }
        }
        if !buffer_refused.is_empty() {
            return Ok(json!({ "restored": false, "bufferRefused": buffer_refused, "disk": null }));
        }
        let disk = titan_changes::restore(&state, &evidence)
            .map_err(|e| rpc_error(codes::IO_ERROR, e.to_string()))?;
        if disk.refused.is_empty() {
            for (doc_id, revision) in buffers {
                let Some(od) = self.documents.get_mut(&doc_id) else {
                    continue;
                };
                let before = od.doc.text().snapshot();
                if let Ok(Some(change)) = od.doc.text_mut().undo(revision) {
                    self.after_change(doc_id, &before, &change);
                }
            }
            if let Some(s) = self.changes.proposals.get_mut(&p.proposal_id) {
                s.proposal.status = ProposalStatus::RolledBack;
                if let Some(ev) = s.evidence.as_mut() {
                    ev.notes.push(format!("restored at {}", unix_ms()));
                    let _ = titan_changes::write_evidence(&state, ev);
                }
            }
            self.mark_graph_stale();
        }
        Ok(
            json!({ "restored": disk.restored && disk.refused.is_empty(), "disk": disk, "bufferRefused": [] }),
        )
    }

    /// The proposal's resulting content of every touched file, as verification file ops.
    fn candidate_ops(&self, proposal: &Proposal) -> Result<Vec<titan_verify::FileOp>, ServerError> {
        let view = self.view()?;
        let mut ops = Vec::new();
        for change in &proposal.changes {
            match change {
                FileChange::Edit { path, edits, .. } => {
                    let abs = view
                        .resolve(Path::new(path))
                        .map_err(|e| rpc_error(codes::INVALID_PARAMS, e.to_string()))?;
                    let mut text = match view.current(&abs) {
                        Ok(Current::OpenBuffer { text, .. }) => text,
                        Ok(Current::Disk { bytes, .. }) => {
                            String::from_utf8(bytes).map_err(|_| {
                                rpc_error(
                                    codes::UNSUPPORTED_CONTENT,
                                    format!("{path} is not UTF-8"),
                                )
                            })?
                        }
                        _ => return Err(rpc_error(codes::IO_ERROR, format!("{path} is missing"))),
                    };
                    let mut sorted = edits.clone();
                    sorted.sort_by_key(|e| e.start);
                    for e in sorted.iter().rev() {
                        if e.end > text.len()
                            || !text.is_char_boundary(e.start)
                            || !text.is_char_boundary(e.end)
                        {
                            return Err(rpc_error(
                                codes::INVALID_POSITION,
                                format!("edit out of range in {path}"),
                            ));
                        }
                        text.replace_range(e.start..e.end, &e.text);
                    }
                    ops.push(titan_verify::FileOp::Write {
                        path: path.clone(),
                        content: text,
                    });
                }
                FileChange::Create { path, content, .. } => ops.push(titan_verify::FileOp::Write {
                    path: path.clone(),
                    content: content.clone(),
                }),
                FileChange::Delete { path, .. } => {
                    ops.push(titan_verify::FileOp::Delete { path: path.clone() })
                }
                FileChange::Rename { from, to, .. } => ops.push(titan_verify::FileOp::Rename {
                    from: from.clone(),
                    to: to.clone(),
                }),
            }
        }
        Ok(ops)
    }

    /// Run allowed checks on base (git HEAD) and candidate (HEAD + the proposal) in
    /// isolated worktrees, in the background; record the results in the evidence.
    pub(super) fn changes_verify(&mut self, params: Value, id: Option<Value>) -> Reply {
        let p: VerifyParams = match super::parse(params) {
            Ok(p) => p,
            Err(e) => return Reply::Now(Err(e)),
        };
        let ops = match self
            .stored(&p.proposal_id)
            .and_then(|stored| self.candidate_ops(&stored.proposal))
        {
            Ok(ops) => ops,
            Err(e) => return Reply::Now(Err(e)),
        };
        let root = match self.workspace() {
            Ok(ws) => ws.root().to_path_buf(),
            Err(e) => return Reply::Now(Err(e)),
        };
        let proposal_id = p.proposal_id.clone();
        let mut timeouts = titan_verify::Timeouts::default();
        if let Some(ms) = p.timeout_ms {
            timeouts.check_ms = ms;
        }
        let plan = titan_verify::VerificationPlan {
            base_rev: "HEAD".to_string(),
            candidate: ops,
            checks: p.checks,
            policy: p.policy,
            timeouts,
        };
        let toolchain = std::env::var("RUSTUP_TOOLCHAIN").ok();
        let submitted = self
            .pool
            .submit("changes.verify", Priority::Low, None, move |_| {
                let config = titan_verify::VerifyConfig {
                    rustup_toolchain: toolchain,
                    ..titan_verify::VerifyConfig::default()
                };
                let result =
                    titan_verify::Workspace::open(&root, config).and_then(|ws| ws.run(&plan));
                TaskResult::Response(match result {
                    Ok(report) => {
                        to_json_rpc(json!({ "proposalId": proposal_id, "report": report }))
                    }
                    Err(e) => Ok(json!({ "proposalId": proposal_id, "error": e.to_string() })),
                })
            });
        match submitted {
            Ok((task, _)) => {
                self.pending.insert(
                    task,
                    Pending::Verify {
                        id,
                        proposal_id: p.proposal_id,
                    },
                );
                Reply::Deferred
            }
            Err(e) => Reply::Now(Err(rpc_error(codes::SERVER_BUSY, e.to_string()))),
        }
    }

    /// Record verification results in the evidence and answer the request.
    pub(super) fn on_verified(
        &mut self,
        id: Option<Value>,
        proposal_id: String,
        result: Result<Value, titan_protocol::rpc::RpcError>,
    ) {
        let mut response = result;
        if let Ok(value) = &mut response {
            let records = verification_records(value);
            if let Some(s) = self.changes.proposals.get_mut(&proposal_id) {
                let evidence = s.evidence.get_or_insert_with(|| EvidenceRecord {
                    format_version: titan_changes::EVIDENCE_FORMAT_VERSION,
                    proposal_id: s.proposal.id.clone(),
                    patch_id: s.proposal.patch_id(),
                    source: s.proposal.source.clone(),
                    base: s.proposal.base.clone(),
                    recorded_unix_ms: unix_ms(),
                    affected: Vec::new(),
                    validation: titan_changes::ValidationSummary {
                        ok: true,
                        problems: Vec::new(),
                        files_previewed: 0,
                        needs_confirmation: Vec::new(),
                    },
                    checks: Vec::new(),
                    approval: s.proposal.approval(),
                    outcome: titan_changes::Outcome::Failed,
                    restore: titan_changes::RestoreInfo {
                        txn_id: None,
                        txn_dir: None,
                        available: false,
                    },
                    notes: vec!["verified before apply: not applied".to_string()],
                });
                evidence.checks.retain(|c| c.name != "verification");
                evidence.checks.extend(records);
                if let Some(state) = &self.options.state_dir {
                    let _ = titan_changes::write_evidence(state, evidence);
                }
                value["evidence"] = serde_json::to_value(&*evidence).unwrap_or_default();
            }
        }
        if let Some(id) = id {
            self.respond(id, response.map_err(super::ServerError::Rpc));
        }
    }
}

/// Evidence check records from a `titan-verify` report (candidate side, with the baseline
/// verdict); every check is recorded, including those not run.
fn verification_records(value: &Value) -> Vec<CheckRecord> {
    if let Some(error) = value["error"].as_str() {
        return vec![CheckRecord {
            name: "verification".to_string(),
            status: EvidenceStatus::Error {
                message: error.to_string(),
            },
            exit_code: None,
            duration_ms: None,
            log_excerpt: String::new(),
            details: Value::Null,
        }];
    }
    let report = &value["report"];
    let comparisons = report["comparison"].as_array().cloned().unwrap_or_default();
    report["runs"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|r| r["side"] == "candidate")
        .enumerate()
        .map(|(i, run)| {
            let status = match run["status"]["kind"].as_str() {
                Some("passed") => EvidenceStatus::Passed,
                Some("failed") => EvidenceStatus::Failed,
                Some("timed_out") => EvidenceStatus::TimedOut,
                Some("not_run") => EvidenceStatus::NotRun {
                    reason: run["status"]["reason"]
                        .as_str()
                        .unwrap_or_default()
                        .to_string(),
                },
                _ => EvidenceStatus::Error {
                    message: run["status"].to_string(),
                },
            };
            let tail = run["log"]["tail"].as_str().unwrap_or_default();
            let excerpt: String = tail
                .chars()
                .rev()
                .take(2_000)
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
                .collect();
            CheckRecord {
                name: run["check"]["kind"].as_str().unwrap_or("check").to_string(),
                status,
                exit_code: run["exit_code"]
                    .as_i64()
                    .and_then(|c| i32::try_from(c).ok()),
                duration_ms: run["duration_ms"].as_u64(),
                log_excerpt: excerpt,
                details: json!({
                    "tests": run["tests"],
                    "comparison": comparisons.get(i),
                    "baseCommit": report["base_commit"],
                }),
            }
        })
        .collect()
}
