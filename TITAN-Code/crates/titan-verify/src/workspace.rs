//! Repository access, worktree lifecycle and the verification run.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::check::Check;
use crate::env::HostEnv;
use crate::error::VerifyError;
use crate::log::{BoundedLog, LogLimits};
use crate::patch::{self, AppliedChange, FileOp};
use crate::process::{self, Exit, ProcessOutcome, ProcessSpec};
use crate::report::{self, CheckRun, CheckStatus, EnvironmentInfo, Side, VerificationReport};
use crate::trust::{self, TrustInspection, TrustPolicy};

/// Longest accepted revision expression, in bytes.
const MAX_REV_LEN: usize = 256;
/// Largest stdout collected from git or `cargo metadata`.
const MAX_TOOL_STDOUT: usize = 64 * 1024 * 1024;
/// Log limits for tool invocations (git, metadata, versions).
const TOOL_LOG: LogLimits = LogLimits {
    head_bytes: 4 * 1024,
    tail_bytes: 4 * 1024,
};

/// Static configuration of a [`Workspace`].
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct VerifyConfig {
    /// Absolute path of `git`; `None` = first `git` on `PATH`.
    pub git: Option<PathBuf>,
    /// Absolute path of `cargo`; `None` = first `cargo` on `PATH`.
    pub cargo: Option<PathBuf>,
    /// Absolute path of `rustc`, used only for `rustc --version` in the report;
    /// `None` = first `rustc` on `PATH` (the report says `None` when there is none).
    pub rustc: Option<PathBuf>,
    /// Directory for per-run temporary directories; `None` = the system temp directory.
    pub scratch_dir: Option<PathBuf>,
    /// `RUSTUP_TOOLCHAIN` for every check (overrides the inherited value). Pinning the
    /// toolchain is required when the repository has a `rust-toolchain(.toml)` file.
    pub rustup_toolchain: Option<String>,
    /// Bytes of output kept per check (head and tail).
    pub log_limits: LogLimits,
}

/// Wall-clock limits.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Timeouts {
    /// Per check (one cargo invocation, including compilation), in milliseconds. Also used
    /// for `cargo metadata`.
    pub check_ms: u64,
    /// Per git invocation and version query, in milliseconds.
    pub tool_ms: u64,
}

impl Default for Timeouts {
    fn default() -> Self {
        Self {
            check_ms: 15 * 60 * 1000,
            tool_ms: 60 * 1000,
        }
    }
}

/// What to verify.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VerificationPlan {
    /// The base commit (any git revision expression naming a commit, e.g. `HEAD`, a SHA, a
    /// branch). Uncommitted changes of the repository are *not* part of the baseline; to
    /// verify them, include them in `candidate`.
    pub base_rev: String,
    /// The candidate change, applied in order on top of the base commit.
    pub candidate: Vec<FileOp>,
    /// Checks to run on both sides, sequentially, in this order.
    pub checks: Vec<Check>,
    /// What project code may execute.
    pub policy: TrustPolicy,
    /// Wall-clock limits.
    pub timeouts: Timeouts,
}

/// A git repository the pipeline verifies changes against.
#[derive(Debug, Clone)]
pub struct Workspace {
    repo: PathBuf,
    config: VerifyConfig,
    host: HostEnv,
    git: PathBuf,
    cargo: PathBuf,
    rustc: Option<PathBuf>,
}

impl Workspace {
    /// Opens the repository containing `path` and resolves `git` and `cargo`.
    ///
    /// The parent environment is read once here (only the forwarded variables).
    ///
    /// # Errors
    /// [`VerifyError::ToolNotFound`], [`VerifyError::NotARepository`],
    /// [`VerifyError::InvalidArgument`] for an invalid `rustup_toolchain`.
    pub fn open(path: impl AsRef<Path>, config: VerifyConfig) -> Result<Self, VerifyError> {
        let path = path.as_ref();
        let mut host = HostEnv::capture();
        if let Some(toolchain) = &config.rustup_toolchain {
            validate_toolchain(toolchain)?;
            host.set("RUSTUP_TOOLCHAIN", toolchain.into());
        }
        let git = host.resolve("git", config.git.as_deref())?;
        let cargo = host.resolve("cargo", config.cargo.as_deref())?;
        let rustc = host.resolve("rustc", config.rustc.as_deref()).ok();
        let dir = std::fs::canonicalize(path)
            .map_err(|e| VerifyError::io(format!("cannot open {}", path.display()), e))?;
        let mut workspace = Self {
            repo: dir.clone(),
            config,
            host,
            git,
            cargo,
            rustc,
        };
        let out = workspace
            .git_in(&dir, &["rev-parse", "--show-toplevel"], 60_000)
            .map_err(|e| VerifyError::NotARepository {
                path: dir.clone(),
                message: e.to_string(),
            })?;
        if !out.success() {
            return Err(VerifyError::NotARepository {
                path: dir,
                message: log_excerpt(&out.log),
            });
        }
        let top = String::from_utf8_lossy(&out.stdout).trim().to_string();
        workspace.repo = std::fs::canonicalize(&top)
            .map_err(|e| VerifyError::io(format!("cannot open repository root {top}"), e))?;
        Ok(workspace)
    }

    /// The repository's top-level directory (canonical).
    #[must_use]
    pub fn repo_root(&self) -> &Path {
        &self.repo
    }

    /// Resolves `rev` to a full commit id.
    ///
    /// # Errors
    /// [`VerifyError::InvalidArgument`] for a malformed revision,
    /// [`VerifyError::RevisionNotFound`] when it names no commit.
    pub fn resolve_commit(&self, rev: &str) -> Result<String, VerifyError> {
        validate_rev(rev)?;
        let expr = format!("{rev}^{{commit}}");
        let out = self
            .git_in(
                &self.repo,
                &[
                    "rev-parse",
                    "--verify",
                    "--quiet",
                    "--end-of-options",
                    &expr,
                ],
                60_000,
            )
            .map_err(|e| VerifyError::Git {
                action: "rev-parse",
                message: e.to_string(),
            })?;
        let sha = String::from_utf8_lossy(&out.stdout).trim().to_string();
        let is_hex =
            (sha.len() == 40 || sha.len() == 64) && sha.bytes().all(|b| b.is_ascii_hexdigit());
        if !out.success() || !is_hex {
            return Err(VerifyError::RevisionNotFound {
                rev: rev.to_string(),
                message: log_excerpt(&out.log),
            });
        }
        Ok(sha)
    }

    /// Creates a private run directory with two detached worktrees of `base_rev`:
    /// `<run>/base` and `<run>/candidate`. They are removed by
    /// [`PreparedWorktrees::cleanup`] or on drop, also when preparation fails half-way.
    ///
    /// # Errors
    /// Revision and git errors; [`VerifyError::Io`] when the run directory cannot be made.
    pub fn prepare(&self, base_rev: &str) -> Result<PreparedWorktrees, VerifyError> {
        self.prepare_with_timeout(base_rev, Timeouts::default().tool_ms)
    }

    fn prepare_with_timeout(
        &self,
        base_rev: &str,
        tool_ms: u64,
    ) -> Result<PreparedWorktrees, VerifyError> {
        let commit = self.resolve_commit(base_rev)?;
        let scratch = self
            .config
            .scratch_dir
            .clone()
            .unwrap_or_else(std::env::temp_dir);
        let dir = tempfile::Builder::new()
            .prefix("titan-verify-")
            .tempdir_in(&scratch)
            .map_err(|e| {
                VerifyError::io(
                    format!("cannot create a run directory in {}", scratch.display()),
                    e,
                )
            })?;
        let root = std::fs::canonicalize(dir.path())
            .map_err(|e| VerifyError::io("cannot resolve the run directory", e))?;
        let mut prepared = PreparedWorktrees {
            workspace: self.clone(),
            run_dir: Some(dir),
            base: root.join("base"),
            candidate: root.join("candidate"),
            root,
            base_commit: commit,
            added: Vec::new(),
            tool_ms,
        };
        for path in [prepared.base.clone(), prepared.candidate.clone()] {
            let path_arg = path.as_os_str().to_owned();
            let out = self
                .git_os(
                    &self.repo,
                    &[
                        "worktree".into(),
                        "add".into(),
                        "--detach".into(),
                        path_arg,
                        prepared.base_commit.clone().into(),
                    ],
                    tool_ms,
                )
                .map_err(|e| VerifyError::Git {
                    action: "worktree add",
                    message: e.to_string(),
                })?;
            if !out.success() {
                return Err(VerifyError::Git {
                    action: "worktree add",
                    message: format!("{}: {}", out.describe(), log_excerpt(&out.log)),
                });
            }
            prepared.added.push(path);
        }
        Ok(prepared)
    }

    /// Runs `plan`: prepares both worktrees, applies the candidate, inspects both sides,
    /// runs every check on the baseline and then on the candidate (one process at a time),
    /// compares, and removes the worktrees.
    ///
    /// # Errors
    /// Plan validation, git and patch errors. Check failures are not errors; they are in
    /// the report.
    pub fn run(&self, plan: &VerificationPlan) -> Result<VerificationReport, VerifyError> {
        validate_plan(plan)?;
        let prepared = self.prepare_with_timeout(&plan.base_rev, plan.timeouts.tool_ms)?;
        let changes = prepared.apply_candidate(&plan.candidate)?;
        let environment = self.environment_info(&prepared.root, plan);

        let mut runs = Vec::with_capacity(plan.checks.len() * 2);
        let mut inspections = Vec::with_capacity(2);
        for (side, worktree) in [
            (Side::Baseline, prepared.base.clone()),
            (Side::Candidate, prepared.candidate.clone()),
        ] {
            let target_dir = prepared.root.join(match side {
                Side::Baseline => "target-baseline",
                Side::Candidate => "target-candidate",
            });
            let inspection = self.inspect(&worktree, &target_dir, plan);
            for check in &plan.checks {
                runs.push(self.run_check(check, side, &worktree, &target_dir, plan, &inspection));
            }
            inspections.push(inspection);
        }
        let comparison = (0..plan.checks.len())
            .filter_map(|i| {
                let base = runs.iter().filter(|r| r.side == Side::Baseline).nth(i)?;
                let cand = runs.iter().filter(|r| r.side == Side::Candidate).nth(i)?;
                Some(report::compare(base, cand))
            })
            .collect();
        let base_commit = prepared.base_commit.clone();
        let cleanup_warnings = prepared.cleanup();
        let candidate_inspection = inspections.pop().unwrap_or_default();
        let baseline_inspection = inspections.pop().unwrap_or_default();
        Ok(VerificationReport {
            base_rev: plan.base_rev.clone(),
            base_commit,
            candidate_patch_id: patch::patch_id(&plan.candidate),
            candidate_changes: changes,
            policy: plan.policy,
            baseline_inspection,
            candidate_inspection,
            runs,
            comparison,
            environment,
            cleanup_warnings,
        })
    }

    fn toolchain_pinned(&self) -> bool {
        self.host.get("RUSTUP_TOOLCHAIN").is_some()
    }

    fn inspect(
        &self,
        worktree: &Path,
        target_dir: &Path,
        plan: &VerificationPlan,
    ) -> TrustInspection {
        let mut inspection = TrustInspection::default();
        let cargo_home = self
            .host
            .cargo_home()
            .map(|h| std::fs::canonicalize(&h).unwrap_or(h));
        let (config, toolchain) = trust::find_config_files(worktree, cargo_home.as_deref());
        inspection.execution_config = config;
        inspection.toolchain_files = toolchain;
        let manifests = trust::walk_manifests(worktree, &mut inspection.problems);
        inspection.manifests = manifests.keys().cloned().collect();
        // Until the dependency set is known, every manifest in the tree counts.
        for (path, facts) in &manifests {
            let name = facts.package.as_deref().unwrap_or("<virtual>");
            if let Some(how) = &facts.build_script {
                inspection
                    .build_scripts
                    .push(format!("`{name}` ({path}: {how})"));
            }
            if facts.proc_macro {
                inspection
                    .proc_macros
                    .push(format!("`{name}` ({path}: [lib] proc-macro = true)"));
            }
        }
        let wants_compile = plan.checks.iter().any(Check::compiles_code);
        let config_ok = inspection.execution_config.is_empty()
            && (inspection.toolchain_files.is_empty() || self.toolchain_pinned());
        if !wants_compile || !config_ok {
            // Either nothing will be compiled, or every check is refused because of the
            // configuration files; `cargo metadata` would read them too, so it is not run.
            return inspection;
        }
        let mut args: Vec<OsString> =
            vec!["metadata".into(), "--format-version".into(), "1".into()];
        if !plan.policy.allow_network {
            args.push("--offline".into());
        }
        let env = self.host.for_cargo(target_dir, !plan.policy.allow_network);
        let spec = ProcessSpec {
            program: &self.cargo,
            args: &args,
            cwd: worktree,
            env: &env,
            timeout: Duration::from_millis(plan.timeouts.check_ms),
            log_limits: TOOL_LOG,
            parse_tests: false,
            stdout_limit: MAX_TOOL_STDOUT,
        };
        match process::run(&spec) {
            Ok(out) if out.success() && !out.stdout_overflow => {
                inspection.build_scripts.clear();
                inspection.proc_macros.clear();
                trust::apply_metadata(&mut inspection, &out.stdout, worktree, &manifests);
            }
            Ok(out) => inspection.problems.push(format!(
                "cargo metadata did not determine the dependency set ({}{}): {}",
                out.describe(),
                if out.stdout_overflow {
                    ", output too large"
                } else {
                    ""
                },
                log_excerpt(&out.log)
            )),
            Err(e) => inspection
                .problems
                .push(format!("cargo metadata could not be started: {e}")),
        }
        inspection
    }

    fn run_check(
        &self,
        check: &Check,
        side: Side,
        worktree: &Path,
        target_dir: &Path,
        plan: &VerificationPlan,
        inspection: &TrustInspection,
    ) -> CheckRun {
        let mut run = CheckRun {
            check: check.clone(),
            side,
            status: CheckStatus::Passed,
            command: Vec::new(),
            exit_code: None,
            duration_ms: 0,
            log: BoundedLog::default(),
            tests: None,
        };
        if let Some(reason) =
            inspection.blocking_reason(check, &plan.policy, self.toolchain_pinned())
        {
            run.log = BoundedLog::note(&format!("[titan-verify: not run: {reason}]\n"));
            run.status = CheckStatus::NotRun { reason };
            return run;
        }
        let offline = !plan.policy.allow_network;
        let args = check.cargo_args(offline);
        run.command = std::iter::once(self.cargo.as_os_str())
            .chain(args.iter().map(OsString::as_os_str))
            .map(|a| a.to_string_lossy().into_owned())
            .collect();
        let env = self.host.for_cargo(target_dir, offline);
        let spec = ProcessSpec {
            program: &self.cargo,
            args: &args,
            cwd: worktree,
            env: &env,
            timeout: Duration::from_millis(plan.timeouts.check_ms),
            log_limits: self.config.log_limits,
            parse_tests: matches!(check, Check::CargoTest { .. }),
            stdout_limit: 0,
        };
        match process::run(&spec) {
            Ok(out) => {
                run.status = match out.exit {
                    Exit::TimedOut => CheckStatus::TimedOut,
                    Exit::Exited { code: Some(0), .. } => CheckStatus::Passed,
                    Exit::Exited { .. } => CheckStatus::Failed,
                };
                run.exit_code = out.code();
                run.duration_ms = u64::try_from(out.duration.as_millis()).unwrap_or(u64::MAX);
                run.log = out.log;
                run.tests = out.tests;
            }
            Err(e) => {
                run.status = CheckStatus::Error {
                    message: format!("cannot start {}: {e}", self.cargo.display()),
                };
            }
        }
        run
    }

    fn environment_info(&self, run_root: &Path, plan: &VerificationPlan) -> EnvironmentInfo {
        let env = self
            .host
            .for_cargo(&run_root.join("target-env"), !plan.policy.allow_network);
        let version = |program: &Path, env: &[(OsString, OsString)]| -> Option<String> {
            let args = [OsString::from("--version")];
            let spec = ProcessSpec {
                program,
                args: &args,
                cwd: run_root,
                env,
                timeout: Duration::from_millis(plan.timeouts.tool_ms),
                log_limits: TOOL_LOG,
                parse_tests: false,
                stdout_limit: 64 * 1024,
            };
            let out = process::run(&spec).ok()?;
            out.success()
                .then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
        };
        EnvironmentInfo {
            git_version: version(&self.git, &self.host.for_git()),
            cargo_version: version(&self.cargo, &env),
            rustc_version: self.rustc.as_deref().and_then(|r| version(r, &env)),
            os: format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            rustup_toolchain: self
                .host
                .get("RUSTUP_TOOLCHAIN")
                .map(|t| t.to_string_lossy().into_owned()),
        }
    }

    fn git_in(
        &self,
        cwd: &Path,
        args: &[&str],
        timeout_ms: u64,
    ) -> std::io::Result<ProcessOutcome> {
        let args: Vec<OsString> = args.iter().map(OsString::from).collect();
        self.git_os(cwd, &args, timeout_ms)
    }

    /// One controlled git invocation: hooks and fsmonitor disabled, scrubbed environment.
    fn git_os(
        &self,
        cwd: &Path,
        args: &[OsString],
        timeout_ms: u64,
    ) -> std::io::Result<ProcessOutcome> {
        let mut argv: Vec<OsString> = vec![
            "-c".into(),
            "core.hooksPath=/dev/null".into(),
            "-c".into(),
            "core.fsmonitor=false".into(),
        ];
        argv.extend(args.iter().cloned());
        let env = self.host.for_git();
        process::run(&ProcessSpec {
            program: &self.git,
            args: &argv,
            cwd,
            env: &env,
            timeout: Duration::from_millis(timeout_ms.max(1)),
            log_limits: TOOL_LOG,
            parse_tests: false,
            stdout_limit: MAX_TOOL_STDOUT,
        })
    }
}

/// Two worktrees of one base commit in a private run directory. Dropping it removes them.
#[derive(Debug)]
pub struct PreparedWorktrees {
    workspace: Workspace,
    run_dir: Option<tempfile::TempDir>,
    root: PathBuf,
    base: PathBuf,
    candidate: PathBuf,
    base_commit: String,
    added: Vec<PathBuf>,
    tool_ms: u64,
}

impl PreparedWorktrees {
    /// The resolved base commit.
    #[must_use]
    pub fn base_commit(&self) -> &str {
        &self.base_commit
    }

    /// The run directory (canonical) holding both worktrees and the target directories.
    #[must_use]
    pub fn run_dir(&self) -> &Path {
        &self.root
    }

    /// The baseline worktree.
    #[must_use]
    pub fn base_path(&self) -> &Path {
        &self.base
    }

    /// The candidate worktree.
    #[must_use]
    pub fn candidate_path(&self) -> &Path {
        &self.candidate
    }

    /// Applies `ops` to the candidate worktree.
    ///
    /// # Errors
    /// [`VerifyError::InvalidPath`] (including symbolic-link escapes), [`VerifyError::Io`].
    pub fn apply_candidate(&self, ops: &[FileOp]) -> Result<Vec<AppliedChange>, VerifyError> {
        patch::apply(&self.candidate, ops)
    }

    /// Removes both worktrees (`git worktree remove --force`), deletes the run directory and
    /// prunes worktree metadata. Returns warnings for anything that failed.
    pub fn cleanup(mut self) -> Vec<String> {
        self.cleanup_inner()
    }

    fn cleanup_inner(&mut self) -> Vec<String> {
        let mut warnings = Vec::new();
        for path in std::mem::take(&mut self.added) {
            let args = [
                "worktree".into(),
                "remove".into(),
                "--force".into(),
                path.as_os_str().to_owned(),
            ];
            match self
                .workspace
                .git_os(&self.workspace.repo, &args, self.tool_ms)
            {
                Ok(out) if out.success() => {}
                Ok(out) => warnings.push(format!(
                    "git worktree remove {}: {}: {}",
                    path.display(),
                    out.describe(),
                    log_excerpt(&out.log)
                )),
                Err(e) => warnings.push(format!("git worktree remove {}: {e}", path.display())),
            }
        }
        if let Some(dir) = self.run_dir.take() {
            if let Err(e) = dir.close() {
                warnings.push(format!("cannot delete the run directory: {e}"));
            }
            match self
                .workspace
                .git_in(&self.workspace.repo, &["worktree", "prune"], self.tool_ms)
            {
                Ok(out) if out.success() => {}
                Ok(out) => warnings.push(format!("git worktree prune: {}", out.describe())),
                Err(e) => warnings.push(format!("git worktree prune: {e}")),
            }
        }
        warnings
    }
}

impl Drop for PreparedWorktrees {
    fn drop(&mut self) {
        let _ = self.cleanup_inner();
    }
}

fn validate_plan(plan: &VerificationPlan) -> Result<(), VerifyError> {
    validate_rev(&plan.base_rev)?;
    for check in &plan.checks {
        check.validate()?;
    }
    for op in &plan.candidate {
        op.validate()?;
    }
    for (what, value) in [
        ("check timeout", plan.timeouts.check_ms),
        ("tool timeout", plan.timeouts.tool_ms),
    ] {
        if value == 0 {
            return Err(VerifyError::InvalidArgument {
                what,
                value: value.to_string(),
                reason: "must be at least 1 ms".to_string(),
            });
        }
    }
    Ok(())
}

/// A revision expression: 1..=256 bytes of `[A-Za-z0-9_./~^@{}-]`, not starting with `-`.
fn validate_rev(rev: &str) -> Result<(), VerifyError> {
    let reject = |reason: &str| VerifyError::InvalidArgument {
        what: "revision",
        value: rev.to_string(),
        reason: reason.to_string(),
    };
    if rev.is_empty() || rev.len() > MAX_REV_LEN {
        return Err(reject("must be 1 to 256 bytes"));
    }
    if rev.starts_with('-') {
        return Err(reject("must not start with '-'"));
    }
    if !rev
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || "_./~^@{}-".contains(c))
    {
        return Err(reject("only [A-Za-z0-9_./~^@{}-] is allowed"));
    }
    Ok(())
}

fn validate_toolchain(toolchain: &str) -> Result<(), VerifyError> {
    let ok = !toolchain.is_empty()
        && toolchain.len() <= 128
        && !toolchain.starts_with('-')
        && toolchain
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "._+-".contains(c));
    if ok {
        Ok(())
    } else {
        Err(VerifyError::InvalidArgument {
            what: "rustup toolchain",
            value: toolchain.to_string(),
            reason: "only [A-Za-z0-9._+-] (1 to 128 bytes, no leading '-') is allowed".to_string(),
        })
    }
}

/// The last ~2 KiB of a tool's output, for error messages.
fn log_excerpt(log: &BoundedLog) -> String {
    let text = if log.truncated { &log.tail } else { &log.head };
    let text = text.trim();
    let start = text
        .char_indices()
        .map(|(i, _)| i)
        .find(|&i| text.len() - i <= 2048)
        .unwrap_or(0);
    text[start..].to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn revisions_are_validated() {
        for good in [
            "HEAD", "HEAD~1", "main", "feat/x", "v1.2.3", "a1b2c3", "HEAD@{1}", "x^2",
        ] {
            assert!(validate_rev(good).is_ok(), "{good}");
        }
        for bad in [
            "",
            "--upload-pack=x",
            "-x",
            "a b",
            "a;b",
            "$(id)",
            "a\nb",
            "a:b",
        ] {
            assert!(validate_rev(bad).is_err(), "{bad:?}");
        }
        assert!(validate_toolchain("1.91.0").is_ok());
        assert!(validate_toolchain("stable-x86_64-unknown-linux-gnu").is_ok());
        assert!(validate_toolchain("../x").is_err());
        assert!(validate_toolchain("-x").is_err());
    }
}
