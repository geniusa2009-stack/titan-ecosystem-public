//! TITAN Code controlled verification pipeline.
//!
//! Runs explicitly allowed checks against a candidate change and against its baseline, each
//! in its own detached git worktree, and reports structured, serializable results
//! ([`VerificationReport`]): per-check status, exit code, duration, bounded log, parsed
//! libtest counts, and a baseline/candidate [`Verdict`] per check.
//!
//! # Safety model (see ADR 0009)
//!
//! * **No arbitrary commands.** A [`Check`] is a variant of a fixed catalogue that maps to
//!   one fixed `cargo` argument vector; the only free arguments (package name, test filter)
//!   are validated against `[A-Za-z0-9_:-]+`. Programs are `git` and `cargo` (plus
//!   `rustc --version` for the report), resolved from explicit absolute paths or absolute
//!   `PATH` entries. No shell is ever involved.
//! * **Trust policy.** Before any check that compiles code, the worktree's manifests and
//!   the resolved dependency set (`cargo metadata --offline`) are inspected for build
//!   scripts and proc macros; unless [`TrustPolicy`] allows them, the check is recorded as
//!   [`CheckStatus::NotRun`] with the reason. Cargo configuration files and unpinned
//!   toolchain files block every check, `cargo fmt --check` included.
//! * **Environment.** Children start from an empty environment plus an allow list (see
//!   `env.rs`); cargo runs `--offline` unless the policy allows the network.
//! * **Resource limits.** Per-check wall-clock timeout; the check's whole process group is
//!   killed on timeout; output is kept as head + tail with the total size.
//!
//! This is **not a sandbox**: a check that runs (notably `cargo test`, and any allowed build
//! script or proc macro) executes candidate code with the caller's privileges, file-system
//! access and (process-level) network access. The boundary is: what may run (catalogue +
//! trust policy), where (a private worktree), with which environment, and for how long.
//!
//! # Baseline
//!
//! The baseline is a commit. Uncommitted changes in the repository are not part of it; to
//! verify them, express them as part of the candidate ([`FileOp`]s).

#![warn(missing_docs)]

mod check;
mod env;
mod error;
mod libtest;
mod log;
mod patch;
mod process;
mod report;
mod trust;
mod workspace;

pub use check::{Check, MAX_ARGUMENT_LEN, validate_argument};
pub use error::VerifyError;
pub use libtest::{MAX_FAILING_NAMES, TestSummary};
pub use log::{BoundedLog, LogLimits};
pub use patch::{AppliedChange, FileOp, MAX_PATH_LEN, patch_id, validate_relative_path};
pub use report::{
    CheckRun, CheckStatus, Comparison, EnvironmentInfo, Side, Verdict, VerificationReport, compare,
};
pub use trust::{TrustInspection, TrustPolicy};
pub use workspace::{PreparedWorktrees, Timeouts, VerificationPlan, VerifyConfig, Workspace};
