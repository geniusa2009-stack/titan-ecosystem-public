//! Shared helpers: real git repositories with a tiny dependency-free Cargo crate.
#![allow(dead_code)]

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use tempfile::TempDir;
use titan_verify::{
    Check, FileOp, LogLimits, Timeouts, TrustPolicy, VerificationPlan, VerifyConfig, Workspace,
};

pub const CARGO_TOML: &str = "[package]
name = \"demo\"
version = \"0.1.0\"
edition = \"2021\"

[dependencies]

[workspace]
";

pub const LIB_OK: &str = "pub fn add(a: i32, b: i32) -> i32 {
    a + b
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adds() {
        assert_eq!(add(2, 2), 4);
    }
}
";

/// A git repository in a temporary directory plus a scratch directory for run directories.
pub struct Repo {
    _dir: TempDir,
    pub path: PathBuf,
    pub scratch: TempDir,
    /// Files outside the repository that tests may write (markers, pid files).
    pub outside: TempDir,
}

fn git(dir: &Path, args: &[&str]) -> String {
    let out = Command::new("git")
        .args(args)
        .current_dir(dir)
        .output()
        .expect("run git");
    assert!(
        out.status.success(),
        "git {args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout).into_owned()
}

impl Repo {
    /// `git init` + local identity + one commit with `files`.
    pub fn new(files: &[(&str, &str)]) -> Self {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = fs::canonicalize(dir.path()).expect("canonicalize");
        git(&path, &["init", "-q"]);
        git(&path, &["config", "user.name", "TITAN Verify Test"]);
        git(
            &path,
            &["config", "user.email", "verify-test@example.invalid"],
        );
        git(&path, &["config", "commit.gpgsign", "false"]);
        let repo = Self {
            _dir: dir,
            path,
            scratch: tempfile::tempdir().expect("scratch"),
            outside: tempfile::tempdir().expect("outside"),
        };
        repo.commit(files, "base");
        repo
    }

    pub fn commit(&self, files: &[(&str, &str)], message: &str) {
        for (rel, content) in files {
            let path = self.path.join(rel);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).expect("mkdir");
            }
            fs::write(&path, content).expect("write fixture");
        }
        git(&self.path, &["add", "-A"]);
        git(&self.path, &["commit", "-q", "-m", message]);
    }

    pub fn git(&self, args: &[&str]) -> String {
        git(&self.path, args)
    }

    /// Number of worktrees git knows about (1 = only the main one).
    pub fn worktree_count(&self) -> usize {
        self.git(&["worktree", "list", "--porcelain"])
            .lines()
            .filter(|l| l.starts_with("worktree "))
            .count()
    }

    /// True when no run directory is left in the scratch directory.
    pub fn scratch_is_empty(&self) -> bool {
        fs::read_dir(self.scratch.path())
            .map(|mut d| d.next().is_none())
            .unwrap_or(false)
    }

    pub fn workspace(&self) -> Workspace {
        self.workspace_with_logs(LogLimits::default())
    }

    pub fn workspace_with_logs(&self, log_limits: LogLimits) -> Workspace {
        let config = VerifyConfig {
            scratch_dir: Some(self.scratch.path().to_path_buf()),
            log_limits,
            ..VerifyConfig::default()
        };
        Workspace::open(&self.path, config).expect("open workspace")
    }

    /// A path outside the repository, as a Rust string literal.
    pub fn outside_literal(&self, name: &str) -> (PathBuf, String) {
        let path = fs::canonicalize(self.outside.path())
            .expect("canonicalize")
            .join(name);
        let literal = format!("{:?}", path.to_string_lossy());
        (path, literal)
    }
}

pub fn plan(checks: Vec<Check>, candidate: Vec<FileOp>) -> VerificationPlan {
    VerificationPlan {
        base_rev: "HEAD".to_string(),
        candidate,
        checks,
        policy: TrustPolicy::default(),
        timeouts: Timeouts {
            check_ms: 300_000,
            tool_ms: 60_000,
        },
    }
}

pub fn write(path: &str, content: &str) -> FileOp {
    FileOp::Write {
        path: path.to_string(),
        content: content.to_string(),
    }
}

pub fn cargo_test() -> Check {
    Check::CargoTest {
        package: None,
        test_filter: None,
    }
}

/// Writes a line straight to file descriptor 2, bypassing libtest's output capture, so
/// measurements show up in CI logs.
pub fn report_line(line: &str) {
    let _ = writeln!(std::io::stderr().lock(), "{line}");
}
