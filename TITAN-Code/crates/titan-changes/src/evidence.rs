//! Evidence records: what was changed, how it was validated, which checks ran and what
//! they reported, who approved, and how to restore.
//!
//! A record states facts that were observed. A check status says what a command reported
//! when it ran (for example "the test suite exited with 0"); it is evidence about that run,
//! not a guarantee that the change is correct. A check that was not run is recorded as
//! [`CheckStatus::NotRun`] with its reason, never left out or reported as passed.
//!
//! Records are stored as one JSON file per proposal, `changes/evidence/<id>.json`, written
//! atomically (temporary file + `fsync` + rename + directory `fsync`).

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::apply::{ApplyReport, ChangeError, Outcome, changes_dir, io_ctx};
use crate::model::{Approval, BaseSnapshot, Proposal, ProposalId, ProposalSource};
use crate::util::{unix_ms, write_atomic};
use crate::validate::{Problem, Target, ValidationReport};

/// Version of the evidence JSON layout.
pub const EVIDENCE_FORMAT_VERSION: u32 = 1;

/// Name of the evidence directory under `changes/`.
pub const EVIDENCE_DIR: &str = "evidence";

/// One file affected by a change.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AffectedFile {
    pub path: String,
    /// BLAKE3 (hex) before; `None` when the file did not exist.
    pub before_hash: Option<String>,
    /// BLAKE3 (hex) after; `None` when the file was removed.
    pub after_hash: Option<String>,
    pub target: Target,
}

/// Summary of the validation that preceded the change.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidationSummary {
    pub ok: bool,
    pub problems: Vec<Problem>,
    pub files_previewed: usize,
    /// Annotation ids that required confirmation.
    pub needs_confirmation: Vec<String>,
}

impl From<&ValidationReport> for ValidationSummary {
    fn from(report: &ValidationReport) -> Self {
        Self {
            ok: report.ok,
            problems: report.problems.clone(),
            files_previewed: report.preview.len(),
            needs_confirmation: report
                .needs_confirmation
                .iter()
                .map(|c| c.annotation_id.clone())
                .collect(),
        }
    }
}

/// What a check reported.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum CheckStatus {
    /// The check ran and reported success.
    Passed,
    /// The check ran and reported failure.
    Failed,
    /// The check was not run.
    #[serde(rename_all = "camelCase")]
    NotRun { reason: String },
    /// The check was stopped after its time limit.
    TimedOut,
    /// The check could not be run or its result could not be read.
    #[serde(rename_all = "camelCase")]
    Error { message: String },
}

/// One check (build, test, lint, ...) and what it reported.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CheckRecord {
    pub name: String,
    pub status: CheckStatus,
    pub exit_code: Option<i32>,
    pub duration_ms: Option<u64>,
    /// A bounded excerpt of the output (the caller chooses the bound).
    #[serde(default)]
    pub log_excerpt: String,
    #[serde(default)]
    pub details: serde_json::Value,
}

impl CheckRecord {
    /// A check that was not run, with the reason.
    pub fn not_run(name: impl Into<String>, reason: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            status: CheckStatus::NotRun {
                reason: reason.into(),
            },
            exit_code: None,
            duration_ms: None,
            log_excerpt: String::new(),
            details: serde_json::Value::Null,
        }
    }
}

/// Where the original content is kept and whether [`crate::restore`] can use it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RestoreInfo {
    pub txn_id: Option<String>,
    pub txn_dir: Option<PathBuf>,
    /// True when the change was applied and its transaction directory existed when the
    /// record was made. [`crate::restore`] still checks every file before touching it.
    pub available: bool,
}

/// The evidence record of one proposal.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EvidenceRecord {
    pub format_version: u32,
    pub proposal_id: ProposalId,
    pub patch_id: String,
    pub source: ProposalSource,
    pub base: BaseSnapshot,
    pub recorded_unix_ms: u64,
    pub affected: Vec<AffectedFile>,
    pub validation: ValidationSummary,
    pub checks: Vec<CheckRecord>,
    pub approval: Option<Approval>,
    pub outcome: Outcome,
    pub restore: RestoreInfo,
    #[serde(default)]
    pub notes: Vec<String>,
}

impl EvidenceRecord {
    /// A record of `report` for `proposal`, without checks (add them to `checks`).
    pub fn from_apply(proposal: &Proposal, report: &ApplyReport) -> Self {
        let mut notes = Vec::new();
        if let Some(failure) = &report.failure {
            notes.push(format!("failure: {failure}"));
        }
        for conflict in &report.rollback_conflicts {
            notes.push(format!(
                "not restored: {} ({}); original copy {:?}, proposed copy {:?}",
                conflict.path, conflict.detail, conflict.original_copy, conflict.proposed_copy
            ));
        }
        if !report.buffer_edits.is_empty() {
            notes.push(format!(
                "{} buffer edit set(s) were handed to the caller to apply in memory",
                report.buffer_edits.len()
            ));
        }
        Self {
            format_version: EVIDENCE_FORMAT_VERSION,
            proposal_id: proposal.id.clone(),
            patch_id: report.patch_id.clone(),
            source: proposal.source.clone(),
            base: proposal.base.clone(),
            recorded_unix_ms: unix_ms(),
            affected: report.affected.clone(),
            validation: ValidationSummary::from(&report.validation),
            checks: Vec::new(),
            approval: proposal.approval(),
            outcome: report.outcome,
            restore: RestoreInfo {
                txn_id: Some(report.txn_id.clone()),
                txn_dir: Some(report.txn_dir.clone()),
                available: report.outcome == Outcome::Applied && report.txn_dir.is_dir(),
            },
            notes,
        }
    }
}

fn evidence_dir(state_dir: &Path) -> PathBuf {
    changes_dir(state_dir).join(EVIDENCE_DIR)
}

fn evidence_path(state_dir: &Path, id: &ProposalId) -> PathBuf {
    evidence_dir(state_dir).join(format!("{}.json", id.as_str()))
}

/// Store `record` as `state_dir/changes/evidence/<proposal id>.json` (atomically,
/// replacing an earlier record of the same proposal). Returns the file path.
pub fn write_evidence(state_dir: &Path, record: &EvidenceRecord) -> Result<PathBuf, ChangeError> {
    let dir = evidence_dir(state_dir);
    fs::create_dir_all(&dir).map_err(io_ctx(format!("creating {}", dir.display())))?;
    let path = evidence_path(state_dir, &record.proposal_id);
    let json = serde_json::to_vec_pretty(record).map_err(|e| ChangeError::Evidence {
        path: path.clone(),
        reason: e.to_string(),
    })?;
    write_atomic(&path, &json).map_err(io_ctx(format!("writing {}", path.display())))?;
    Ok(path)
}

fn parse_evidence(path: &Path) -> Result<EvidenceRecord, ChangeError> {
    let bytes = fs::read(path).map_err(io_ctx(format!("reading {}", path.display())))?;
    serde_json::from_slice(&bytes).map_err(|e| ChangeError::Evidence {
        path: path.to_path_buf(),
        reason: e.to_string(),
    })
}

/// Load the evidence record of proposal `id`.
pub fn load_evidence(state_dir: &Path, id: &ProposalId) -> Result<EvidenceRecord, ChangeError> {
    let path = evidence_path(state_dir, id);
    let record = parse_evidence(&path)?;
    if &record.proposal_id != id {
        return Err(ChangeError::Evidence {
            path,
            reason: format!("the file holds proposal {}", record.proposal_id),
        });
    }
    Ok(record)
}

/// Every readable record, plus the files that could not be read.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EvidenceListing {
    /// Sorted by `recorded_unix_ms`, then proposal id.
    pub records: Vec<EvidenceRecord>,
    pub unreadable: Vec<(PathBuf, String)>,
}

/// List all evidence records under `state_dir`.
pub fn list_evidence(state_dir: &Path) -> Result<EvidenceListing, ChangeError> {
    let dir = evidence_dir(state_dir);
    let mut listing = EvidenceListing::default();
    let entries = match fs::read_dir(&dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(listing),
        Err(e) => return Err(io_ctx(format!("reading {}", dir.display()))(e)),
    };
    let mut paths: Vec<PathBuf> = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "json"))
        .collect();
    paths.sort();
    for path in paths {
        match parse_evidence(&path) {
            Ok(record) => listing.records.push(record),
            Err(e) => listing.unreadable.push((path, e.to_string())),
        }
    }
    listing.records.sort_by(|a, b| {
        a.recorded_unix_ms
            .cmp(&b.recorded_unix_ms)
            .then_with(|| a.proposal_id.cmp(&b.proposal_id))
    });
    Ok(listing)
}
