//! The serializable verification report and the baseline/candidate comparison.

use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};

use crate::check::Check;
use crate::libtest::TestSummary;
use crate::log::BoundedLog;
use crate::patch::AppliedChange;
use crate::trust::{TrustInspection, TrustPolicy};

/// Which worktree a check ran in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Side {
    /// The base commit as checked out.
    Baseline,
    /// The base commit with the candidate patch applied.
    Candidate,
}

/// Outcome of one check on one side.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum CheckStatus {
    /// The command exited with status 0.
    Passed,
    /// The command exited with a non-zero status (or was killed by a signal it did not get
    /// from the pipeline).
    Failed,
    /// The pipeline refused to run the check (trust policy, configuration files, incomplete
    /// inspection). Never counted as passed.
    NotRun {
        /// Why it did not run.
        reason: String,
    },
    /// The wall-clock timeout expired; the process group was killed.
    TimedOut,
    /// The command could not be started.
    Error {
        /// What went wrong.
        message: String,
    },
}

impl CheckStatus {
    /// True only for [`CheckStatus::Passed`].
    #[must_use]
    pub fn is_passed(&self) -> bool {
        matches!(self, Self::Passed)
    }
}

/// One check executed (or refused) on one side.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CheckRun {
    /// The check.
    pub check: Check,
    /// Where it ran.
    pub side: Side,
    /// The outcome.
    pub status: CheckStatus,
    /// The argument vector (program first); empty when the check did not run.
    pub command: Vec<String>,
    /// Exit code when the process exited normally.
    pub exit_code: Option<i32>,
    /// Wall-clock duration in milliseconds (0 when not run).
    pub duration_ms: u64,
    /// Head and tail of combined stdout and stderr.
    pub log: BoundedLog,
    /// libtest counts for `cargo test` (`None` when no test binary reported a result).
    pub tests: Option<TestSummary>,
}

/// How the candidate compares to the baseline for one check.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Verdict {
    /// Same outcome (and, for failing test runs, the same failing tests).
    Same,
    /// The candidate passes where the baseline failed or timed out, or fails fewer tests and
    /// no new ones.
    Improved,
    /// The candidate fails or times out where the baseline passed, or fails a test the
    /// baseline did not.
    Regressed,
    /// At least one side did not run, errored, or both timed out / they failed differently
    /// (e.g. one timed out, one failed): no claim is made.
    Incomparable,
}

/// The comparison for one planned check.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Comparison {
    /// The check.
    pub check: Check,
    /// Baseline outcome.
    pub baseline: CheckStatus,
    /// Candidate outcome.
    pub candidate: CheckStatus,
    /// The verdict.
    pub verdict: Verdict,
    /// Tests failing in the candidate but not in the baseline.
    pub newly_failing: Vec<String>,
    /// Tests failing in the baseline but not in the candidate.
    pub newly_passing: Vec<String>,
}

/// Tool versions and platform the checks ran with.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct EnvironmentInfo {
    /// `git --version`.
    pub git_version: Option<String>,
    /// `cargo --version` (run outside the worktrees with the check environment).
    pub cargo_version: Option<String>,
    /// `rustc --version` (same).
    pub rustc_version: Option<String>,
    /// `std::env::consts::OS` / `ARCH`.
    pub os: String,
    /// `RUSTUP_TOOLCHAIN` passed to checks, if any.
    pub rustup_toolchain: Option<String>,
}

/// The result of [`crate::Workspace::run`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VerificationReport {
    /// The revision as requested.
    pub base_rev: String,
    /// The commit it resolved to.
    pub base_commit: String,
    /// [`crate::patch_id`] of the candidate operations.
    pub candidate_patch_id: String,
    /// What applying the candidate did (with BLAKE3 of every written file).
    pub candidate_changes: Vec<AppliedChange>,
    /// The trust policy in force.
    pub policy: TrustPolicy,
    /// Inspection of the baseline worktree.
    pub baseline_inspection: TrustInspection,
    /// Inspection of the candidate worktree.
    pub candidate_inspection: TrustInspection,
    /// Every check on both sides: all baseline runs, then all candidate runs, in plan order.
    pub runs: Vec<CheckRun>,
    /// One comparison per planned check, in plan order.
    pub comparison: Vec<Comparison>,
    /// Tool versions and platform.
    pub environment: EnvironmentInfo,
    /// Problems removing the worktrees (empty when cleanup succeeded).
    pub cleanup_warnings: Vec<String>,
}

impl VerificationReport {
    /// The run of `check_index` (plan order) on `side`.
    #[must_use]
    pub fn run_of(&self, check_index: usize, side: Side) -> Option<&CheckRun> {
        self.runs.iter().filter(|r| r.side == side).nth(check_index)
    }
}

/// Compares the two runs of one check.
#[must_use]
pub fn compare(baseline: &CheckRun, candidate: &CheckRun) -> Comparison {
    use CheckStatus as S;
    let (base_failing, cand_failing) = failing_sets(baseline, candidate);
    let newly_failing: Vec<String> = cand_failing.difference(&base_failing).cloned().collect();
    let newly_passing: Vec<String> = base_failing.difference(&cand_failing).cloned().collect();
    let verdict = match (&baseline.status, &candidate.status) {
        (S::Passed, S::Passed) => Verdict::Same,
        (S::Failed | S::TimedOut, S::Passed) => Verdict::Improved,
        (S::Passed, S::Failed | S::TimedOut) => Verdict::Regressed,
        (S::Failed, S::Failed) => {
            compare_failures(baseline, candidate, &newly_failing, &newly_passing)
        }
        _ => Verdict::Incomparable,
    };
    Comparison {
        check: candidate.check.clone(),
        baseline: baseline.status.clone(),
        candidate: candidate.status.clone(),
        verdict,
        newly_failing,
        newly_passing,
    }
}

fn failing_sets(baseline: &CheckRun, candidate: &CheckRun) -> (BTreeSet<String>, BTreeSet<String>) {
    let set = |run: &CheckRun| -> BTreeSet<String> {
        run.tests
            .as_ref()
            .map(|t| t.failing_names.iter().cloned().collect())
            .unwrap_or_default()
    };
    (set(baseline), set(candidate))
}

fn compare_failures(
    baseline: &CheckRun,
    candidate: &CheckRun,
    newly_failing: &[String],
    newly_passing: &[String],
) -> Verdict {
    let (Some(base), Some(cand)) = (&baseline.tests, &candidate.tests) else {
        // No test results on at least one side (e.g. a build failure): same coarse outcome.
        return if baseline.tests.is_none() && candidate.tests.is_none() {
            Verdict::Same
        } else {
            Verdict::Incomparable
        };
    };
    if base.failing_names_truncated || cand.failing_names_truncated {
        return match cand.failed.cmp(&base.failed) {
            std::cmp::Ordering::Less => Verdict::Improved,
            std::cmp::Ordering::Equal => Verdict::Same,
            std::cmp::Ordering::Greater => Verdict::Regressed,
        };
    }
    if !newly_failing.is_empty() {
        Verdict::Regressed
    } else if !newly_passing.is_empty() {
        Verdict::Improved
    } else {
        Verdict::Same
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(status: CheckStatus, failing: &[&str]) -> CheckRun {
        CheckRun {
            check: Check::CargoTest {
                package: None,
                test_filter: None,
            },
            side: Side::Baseline,
            status,
            command: Vec::new(),
            exit_code: None,
            duration_ms: 0,
            log: BoundedLog::default(),
            tests: Some(TestSummary {
                binaries: 1,
                failed: failing.len() as u64,
                failing_names: failing.iter().map(|s| (*s).to_string()).collect(),
                ..TestSummary::default()
            }),
        }
    }

    #[test]
    fn verdicts() {
        use CheckStatus as S;
        let v = |b: CheckStatus, bf: &[&str], c: CheckStatus, cf: &[&str]| {
            compare(&run(b, bf), &run(c, cf)).verdict
        };
        assert_eq!(v(S::Passed, &[], S::Passed, &[]), Verdict::Same);
        assert_eq!(v(S::Failed, &["a"], S::Passed, &[]), Verdict::Improved);
        assert_eq!(v(S::TimedOut, &[], S::Passed, &[]), Verdict::Improved);
        assert_eq!(v(S::Passed, &[], S::Failed, &["a"]), Verdict::Regressed);
        assert_eq!(v(S::Passed, &[], S::TimedOut, &[]), Verdict::Regressed);
        assert_eq!(
            v(S::Failed, &["a", "b"], S::Failed, &["a"]),
            Verdict::Improved
        );
        assert_eq!(
            v(S::Failed, &["a"], S::Failed, &["a", "b"]),
            Verdict::Regressed
        );
        assert_eq!(v(S::Failed, &["a"], S::Failed, &["b"]), Verdict::Regressed);
        assert_eq!(v(S::Failed, &["a"], S::Failed, &["a"]), Verdict::Same);
        assert_eq!(v(S::TimedOut, &[], S::TimedOut, &[]), Verdict::Incomparable);
        let not_run = S::NotRun {
            reason: "policy".into(),
        };
        assert_eq!(v(not_run, &[], S::Passed, &[]), Verdict::Incomparable);
        let error = S::Error {
            message: "spawn".into(),
        };
        assert_eq!(v(S::Passed, &[], error, &[]), Verdict::Incomparable);
        let cmp = compare(&run(S::Failed, &["a", "b"]), &run(S::Failed, &["b", "c"]));
        assert_eq!(cmp.newly_failing, ["c"]);
        assert_eq!(cmp.newly_passing, ["a"]);
    }
}
