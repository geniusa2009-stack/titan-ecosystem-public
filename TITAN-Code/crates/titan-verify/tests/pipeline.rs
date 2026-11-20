//! End-to-end tests: real git repositories, real `git worktree`, real nested `cargo`.
//!
//! Every run uses its own run directory with per-side `CARGO_TARGET_DIR`s, so the nested
//! cargo invocations never contend for the outer build's lock.

mod common;

use std::time::{Duration, Instant};

use common::{CARGO_TOML, LIB_OK, Repo, cargo_test, plan, report_line, write};
use titan_verify::{
    Check, CheckStatus, FileOp, LogLimits, Side, TrustPolicy, Verdict, VerifyError,
};

const LIB_BROKEN_TEST: &str = "pub fn add(a: i32, b: i32) -> i32 {
    a - b
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adds() {
        println!(\"{}\", \"noise \".repeat(40_000));
        assert_eq!(add(2, 2), 4);
    }

    #[test]
    fn other() {
        assert_eq!(add(0, 0), 0);
    }

    #[test]
    #[ignore]
    fn slow() {}
}
";

const INTEGRATION_TEST: &str = "#[test]
fn it_works() {
    assert_eq!(demo::add(1, 0), 1);
}
";

#[test]
fn candidate_fixing_a_failing_test_is_improved() {
    let repo = Repo::new(&[
        ("Cargo.toml", CARGO_TOML),
        ("src/lib.rs", LIB_BROKEN_TEST),
        ("tests/it.rs", INTEGRATION_TEST),
    ]);
    let limits = LogLimits {
        head_bytes: 8 * 1024,
        tail_bytes: 8 * 1024,
    };
    let workspace = repo.workspace_with_logs(limits);
    let fixed = LIB_BROKEN_TEST.replace("a - b", "a + b");
    let filtered = Check::CargoTest {
        package: Some("demo".to_string()),
        test_filter: Some("other".to_string()),
    };
    let plan = plan(
        vec![cargo_test(), filtered],
        vec![write("src/lib.rs", &fixed)],
    );
    let started = Instant::now();
    let report = workspace.run(&plan).expect("run");
    let total = started.elapsed();

    let base = report.run_of(0, Side::Baseline).expect("baseline run");
    let cand = report.run_of(0, Side::Candidate).expect("candidate run");
    assert_eq!(base.status, CheckStatus::Failed, "{:?}", base.log);
    assert_eq!(cand.status, CheckStatus::Passed, "{:?}", cand.log);
    assert_eq!(base.exit_code, Some(101));
    assert_eq!(cand.exit_code, Some(0));

    // Unit tests + integration test + doc tests: three binaries, summed.
    let bt = base.tests.as_ref().expect("baseline tests parsed");
    assert_eq!(bt.binaries, 3, "{bt:?}");
    assert_eq!((bt.passed, bt.failed, bt.ignored), (2, 1, 1), "{bt:?}");
    assert_eq!(bt.failing_names, ["tests::adds"]);
    let ct = cand.tests.as_ref().expect("candidate tests parsed");
    assert_eq!((ct.passed, ct.failed, ct.ignored), (3, 0, 1), "{ct:?}");
    assert!(ct.failing_names.is_empty());

    // The failing test printed ~240 KB: the log keeps head and tail and says so, while the
    // test counts above were parsed from the whole stream.
    assert!(base.log.truncated);
    assert!(base.log.total_bytes > 240_000, "{}", base.log.total_bytes);
    assert!(base.log.head.len() <= limits.head_bytes);
    assert!(base.log.tail.len() <= limits.tail_bytes);
    assert_eq!(
        base.log.omitted_bytes,
        base.log.total_bytes - (base.log.head.len() + base.log.tail.len()) as u64
    );
    assert!(base.log.head.contains("demo"), "{}", base.log.head);
    assert!(base.log.tail.contains("test result:"), "{}", base.log.tail);
    assert!(!cand.log.truncated);

    let cmp = &report.comparison[0];
    assert_eq!(cmp.verdict, Verdict::Improved);
    assert_eq!(cmp.newly_passing, ["tests::adds"]);
    assert!(cmp.newly_failing.is_empty());

    // Filtered run: only `tests::other` runs, on both sides.
    let fb = report.run_of(1, Side::Baseline).expect("filtered baseline");
    let fc = report
        .run_of(1, Side::Candidate)
        .expect("filtered candidate");
    assert_eq!(fb.status, CheckStatus::Passed, "{:?}", fb.log);
    let ft = fb.tests.as_ref().expect("filtered tests parsed");
    assert_eq!((ft.passed, ft.failed), (1, 0), "{ft:?}");
    assert!(ft.filtered >= 3, "{ft:?}");
    assert!(fb.command.iter().any(|a| a == "other"));
    assert_eq!(fc.status, CheckStatus::Passed);
    assert_eq!(report.comparison[1].verdict, Verdict::Same);

    // Provenance.
    assert_eq!(report.base_commit, repo.git(&["rev-parse", "HEAD"]).trim());
    assert_eq!(
        report.candidate_patch_id,
        titan_verify::patch_id(&plan.candidate)
    );
    assert_eq!(
        report.candidate_changes[0].blake3.as_deref(),
        Some(blake3::hash(fixed.as_bytes()).to_hex().as_str())
    );
    assert!(
        report
            .environment
            .git_version
            .as_deref()
            .is_some_and(|v| v.starts_with("git version"))
    );
    assert!(
        report
            .environment
            .cargo_version
            .as_deref()
            .is_some_and(|v| v.starts_with("cargo "))
    );
    assert!(
        report
            .environment
            .rustc_version
            .as_deref()
            .is_some_and(|v| v.starts_with("rustc "))
    );
    assert_eq!(report.baseline_inspection.dependency_packages, Some(1));
    assert!(
        report.cleanup_warnings.is_empty(),
        "{:?}",
        report.cleanup_warnings
    );
    assert_eq!(repo.worktree_count(), 1);
    assert!(repo.scratch_is_empty());

    let json = serde_json::to_string(&report).expect("serialize report");
    let back: titan_verify::VerificationReport = serde_json::from_str(&json).expect("parse");
    assert_eq!(back, report);

    report_line(&format!(
        "titan-verify timing: improved-test plan total {} ms; cargo test baseline {} ms, \
         candidate {} ms; filtered baseline {} ms, candidate {} ms ({}, {})",
        total.as_millis(),
        base.duration_ms,
        cand.duration_ms,
        fb.duration_ms,
        fc.duration_ms,
        report.environment.os,
        report.environment.rustc_version.as_deref().unwrap_or("?"),
    ));
}

#[test]
fn candidate_breaking_compilation_fails_cargo_check() {
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", LIB_OK)]);
    let workspace = repo.workspace();
    let broken = "pub fn add(a: i32, b: i32) -> i32 {\n    a + \"x\" + b\n}\n";
    let plan = plan(
        vec![Check::CargoCheck { package: None }],
        vec![write("src/lib.rs", broken)],
    );
    let started = Instant::now();
    let report = workspace.run(&plan).expect("run");
    let total = started.elapsed();
    let base = report.run_of(0, Side::Baseline).expect("baseline");
    let cand = report.run_of(0, Side::Candidate).expect("candidate");
    assert_eq!(base.status, CheckStatus::Passed, "{:?}", base.log);
    assert_eq!(cand.status, CheckStatus::Failed);
    assert!(cand.log.contains("error[E0277]"), "{:?}", cand.log);
    assert!(cand.log.contains("src/lib.rs"), "{:?}", cand.log);
    assert!(cand.tests.is_none());
    assert_eq!(report.comparison[0].verdict, Verdict::Regressed);
    assert_eq!(
        repo.worktree_count(),
        1,
        "worktrees left after a failing check"
    );
    assert!(repo.scratch_is_empty());
    report_line(&format!(
        "titan-verify timing: broken-compile plan total {} ms; cargo check baseline {} ms, \
         candidate {} ms",
        total.as_millis(),
        base.duration_ms,
        cand.duration_ms
    ));
}

#[cfg(unix)]
#[test]
fn timeout_kills_the_whole_process_group() {
    let repo = Repo::new(&[
        ("Cargo.toml", CARGO_TOML),
        (
            "src/lib.rs",
            "#[cfg(test)]\nmod tests {\n    #[test]\n    fn sleeps() {}\n}\n",
        ),
    ]);
    let (pid_file, pid_literal) = repo.outside_literal("pids");
    let sleeper = format!(
        "#[cfg(test)]
mod tests {{
    #[test]
    fn sleeps() {{
        let child = std::process::Command::new(\"sleep\").arg(\"300\").spawn().unwrap();
        let path = {pid_literal};
        let pids = format!(\"{{}} {{}}\", std::process::id(), child.id());
        std::fs::write(path, pids).unwrap();
        std::thread::sleep(std::time::Duration::from_secs(300));
    }}
}}
"
    );
    let workspace = repo.workspace();
    let mut plan = plan(vec![cargo_test()], vec![write("src/lib.rs", &sleeper)]);
    plan.timeouts.check_ms = 20_000;
    let report = workspace.run(&plan).expect("run");
    let base = report.run_of(0, Side::Baseline).expect("baseline");
    let cand = report.run_of(0, Side::Candidate).expect("candidate");
    assert_eq!(base.status, CheckStatus::Passed, "{:?}", base.log);
    assert_eq!(cand.status, CheckStatus::TimedOut, "{:?}", cand.log);
    assert_eq!(cand.exit_code, None);
    assert!(cand.duration_ms >= 20_000, "{}", cand.duration_ms);
    assert!(cand.duration_ms < 30_000, "{}", cand.duration_ms);
    assert_eq!(report.comparison[0].verdict, Verdict::Regressed);

    // The test binary (a grandchild of the pipeline) and its own child `sleep` both ran ...
    let pids = std::fs::read_to_string(&pid_file).expect("the sleeping test started");
    let pids: Vec<i32> = pids
        .split_whitespace()
        .map(|p| p.parse().expect("pid"))
        .collect();
    assert_eq!(pids.len(), 2);
    // ... and are gone (killed with the process group; reaped by init).
    let deadline = Instant::now() + Duration::from_secs(10);
    for pid in pids {
        let pid = rustix::process::Pid::from_raw(pid).expect("nonzero pid");
        loop {
            match rustix::process::test_kill_process(pid) {
                Err(rustix::io::Errno::SRCH) => break,
                _ if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(50)),
                other => panic!("process {pid:?} still exists after the timeout: {other:?}"),
            }
        }
    }
    assert_eq!(repo.worktree_count(), 1);
    assert!(repo.scratch_is_empty());
}

#[test]
fn build_scripts_need_trust_and_fmt_runs_regardless() {
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", LIB_OK)]);
    let (marker, _) = repo.outside_literal("build-script-ran");
    // The marker path lives in a data file so that build.rs stays rustfmt-clean whatever the
    // length of the temporary path (build scripts run in the package directory).
    let build_rs = "fn main() {
    let marker = std::fs::read_to_string(\"marker-path.txt\").unwrap();
    std::fs::write(marker.trim(), \"ran\").unwrap();
}
";
    let marker_path = marker.to_string_lossy().into_owned();
    repo.commit(
        &[("build.rs", build_rs), ("marker-path.txt", &marker_path)],
        "add build script",
    );
    let workspace = repo.workspace();
    let checks = vec![Check::CargoCheck { package: None }, Check::CargoFmtCheck];

    let denied = workspace.run(&plan(checks.clone(), vec![])).expect("run");
    for side in [Side::Baseline, Side::Candidate] {
        let check = denied.run_of(0, side).expect("check run");
        match &check.status {
            CheckStatus::NotRun { reason } => {
                assert!(reason.contains("build scripts"), "{reason}");
                assert!(reason.contains("build.rs"), "{reason}");
            }
            other => {
                panic!("cargo check with a build script ran under the default policy: {other:?}")
            }
        }
        assert!(check.command.is_empty());
        let fmt = denied.run_of(1, side).expect("fmt run");
        assert_eq!(fmt.status, CheckStatus::Passed, "{:?}", fmt.log);
    }
    assert_eq!(denied.comparison[0].verdict, Verdict::Incomparable);
    assert!(!denied.candidate_inspection.build_scripts.is_empty());
    assert!(
        !marker.exists(),
        "the build script ran although it was not allowed"
    );

    let mut allowed = plan(checks, vec![]);
    allowed.policy = TrustPolicy {
        allow_build_scripts: true,
        ..TrustPolicy::default()
    };
    let report = workspace.run(&allowed).expect("run");
    for side in [Side::Baseline, Side::Candidate] {
        let check = report.run_of(0, side).expect("check run");
        assert_eq!(check.status, CheckStatus::Passed, "{:?}", check.log);
    }
    assert!(marker.exists(), "the allowed build script did not run");
    assert_eq!(repo.worktree_count(), 1);
}

#[test]
fn proc_macro_crates_need_trust() {
    let manifest = CARGO_TOML.replace(
        "[dependencies]",
        "[lib]\nproc-macro = true\n\n[dependencies]",
    );
    let lib = "use proc_macro::TokenStream;

#[proc_macro]
pub fn identity(input: TokenStream) -> TokenStream {
    input
}
";
    let repo = Repo::new(&[("Cargo.toml", &manifest), ("src/lib.rs", lib)]);
    let workspace = repo.workspace();
    let checks = vec![Check::CargoCheck { package: None }];

    let denied = workspace.run(&plan(checks.clone(), vec![])).expect("run");
    let run = denied.run_of(0, Side::Candidate).expect("run");
    match &run.status {
        CheckStatus::NotRun { reason } => assert!(reason.contains("proc macros"), "{reason}"),
        other => panic!("proc-macro crate compiled under the default policy: {other:?}"),
    }
    let found = denied.candidate_inspection.proc_macros.join(" | ");
    assert!(found.contains("`demo`"), "{found}");
    assert!(found.contains("proc-macro"), "{found}");

    let mut allowed = plan(checks, vec![]);
    allowed.policy.allow_proc_macros = true;
    let report = workspace.run(&allowed).expect("run");
    let run = report.run_of(0, Side::Candidate).expect("run");
    assert_eq!(run.status, CheckStatus::Passed, "{:?}", run.log);
}

#[test]
fn cargo_configuration_in_the_candidate_blocks_every_check() {
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", LIB_OK)]);
    let workspace = repo.workspace();
    let plan = plan(
        vec![Check::CargoCheck { package: None }, Check::CargoFmtCheck],
        vec![write(
            ".cargo/config.toml",
            "[build]\nrustc-wrapper = \"/bin/false\"\n",
        )],
    );
    let report = workspace.run(&plan).expect("run");
    for index in 0..2 {
        let base = report.run_of(index, Side::Baseline).expect("baseline");
        assert_eq!(base.status, CheckStatus::Passed, "{:?}", base.log);
        let cand = report.run_of(index, Side::Candidate).expect("candidate");
        match &cand.status {
            CheckStatus::NotRun { reason } => {
                assert!(reason.contains(".cargo/config.toml"), "{reason}");
            }
            other => panic!("check ran with a candidate cargo config: {other:?}"),
        }
        assert_eq!(report.comparison[index].verdict, Verdict::Incomparable);
    }
}

#[test]
fn environment_is_scrubbed() {
    const SECRET: &str = "TITAN_SECRET_FOR_TEST";
    if std::env::var_os(SECRET).is_none() {
        // Re-run only this test in a child process whose environment holds the secret.
        let exe = std::env::current_exe().expect("current exe");
        let out = std::process::Command::new(exe)
            .args(["environment_is_scrubbed", "--exact", "--test-threads=1"])
            .env(SECRET, "hunter2")
            .output()
            .expect("run child test");
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success() && stdout.contains("1 passed"),
            "child failed:\n{stdout}\n{}",
            String::from_utf8_lossy(&out.stderr)
        );
        return;
    }
    // Child: the secret is in this process's environment; the checks must not see it.
    let lib = format!(
        "#[cfg(test)]
mod tests {{
    #[test]
    fn environment() {{
        if let Some(value) = std::env::var_os({SECRET:?}) {{
            panic!(\"secret leaked: {{value:?}}\");
        }}
        assert_eq!(std::env::var(\"CARGO_TERM_COLOR\").as_deref(), Ok(\"never\"));
    }}
}}
"
    );
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", &lib)]);
    let report = repo
        .workspace()
        .run(&plan(vec![cargo_test()], vec![]))
        .expect("run");
    for side in [Side::Baseline, Side::Candidate] {
        let run = report.run_of(0, side).expect("run");
        assert_eq!(run.status, CheckStatus::Passed, "{:?}", run.log);
        assert_eq!(run.tests.as_ref().map(|t| t.passed), Some(1));
    }
}

#[cfg(unix)]
#[test]
fn worktrees_are_removed_after_success_and_after_failure() {
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", LIB_OK)]);
    let outside = std::fs::canonicalize(repo.outside.path()).expect("outside");
    std::os::unix::fs::symlink(&outside, repo.path.join("escape")).expect("symlink");
    repo.commit(&[], "add a symbolic link pointing outside");
    let workspace = repo.workspace();

    // Success (no checks: just prepare, apply, clean up).
    let report = workspace
        .run(&plan(vec![], vec![write("notes.txt", "hello")]))
        .expect("run");
    assert!(report.runs.is_empty());
    assert_eq!(repo.worktree_count(), 1);
    assert!(repo.scratch_is_empty());

    // Failure after both worktrees exist: the candidate writes through the symbolic link.
    let result = workspace.run(&plan(
        vec![Check::CargoCheck { package: None }],
        vec![write("escape/pwned.txt", "x")],
    ));
    match result {
        Err(VerifyError::InvalidPath { path, reason }) => {
            assert_eq!(path, "escape/pwned.txt");
            assert!(reason.contains("symbolic link"), "{reason}");
        }
        other => panic!("symlink escape was not rejected: {other:?}"),
    }
    assert!(!outside.join("pwned.txt").exists());
    assert_eq!(
        repo.worktree_count(),
        1,
        "worktrees left after a failed run"
    );
    assert!(repo.scratch_is_empty());

    // Explicit prepare / cleanup.
    let prepared = workspace.prepare("HEAD").expect("prepare");
    assert!(prepared.base_path().join("src/lib.rs").exists());
    assert!(prepared.candidate_path().join("Cargo.toml").exists());
    assert_eq!(repo.worktree_count(), 3);
    assert!(prepared.cleanup().is_empty());
    assert_eq!(repo.worktree_count(), 1);
    // Dropping without cleanup also removes them.
    drop(workspace.prepare("HEAD").expect("prepare"));
    assert_eq!(repo.worktree_count(), 1);
    assert!(repo.scratch_is_empty());
}

#[test]
fn invalid_plans_are_rejected_before_any_worktree_exists() {
    let repo = Repo::new(&[("Cargo.toml", CARGO_TOML), ("src/lib.rs", LIB_OK)]);
    let workspace = repo.workspace();
    let bad_plans = [
        plan(
            vec![Check::CargoCheck {
                package: Some("demo; rm -rf /".to_string()),
            }],
            vec![],
        ),
        plan(
            vec![Check::CargoTest {
                package: None,
                test_filter: Some("$(touch /tmp/x)".to_string()),
            }],
            vec![],
        ),
        plan(
            vec![Check::CargoTest {
                package: None,
                test_filter: Some("--nocapture".to_string()),
            }],
            vec![],
        ),
        plan(vec![], vec![write("../x", "y")]),
        plan(vec![], vec![write("/tmp/titan-verify-abs", "y")]),
        plan(
            vec![],
            vec![FileOp::Rename {
                from: "src/lib.rs".to_string(),
                to: ".git/hooks/post-checkout".to_string(),
            }],
        ),
        {
            let mut p = plan(vec![], vec![]);
            p.base_rev = "--upload-pack=touch".to_string();
            p
        },
    ];
    for bad in &bad_plans {
        let result = workspace.run(bad);
        assert!(
            matches!(
                result,
                Err(VerifyError::InvalidArgument { .. } | VerifyError::InvalidPath { .. })
            ),
            "{bad:?}: {result:?}"
        );
    }
    let mut missing = plan(vec![], vec![]);
    missing.base_rev = "no-such-branch".to_string();
    assert!(matches!(
        workspace.run(&missing),
        Err(VerifyError::RevisionNotFound { .. })
    ));
    assert_eq!(repo.worktree_count(), 1);
    assert!(repo.scratch_is_empty());
}
