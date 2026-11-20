//! Applying, rolling back, recovering and restoring on a real file system.

mod common;

use std::collections::HashMap;
use std::fs;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::Path;

use common::{BufferView, Ws, create};
use titan_changes::{
    ApplyError, ApplyPoint, ByteEdit, ChangeError, CheckRecord, CheckStatus, EvidenceRecord,
    ExpectedBase, FileChange, HookAction, OpState, Outcome, RecoveryAction, SavePhase, Target,
    apply, apply_with_hook, list_evidence, load_evidence, recover, restore, write_evidence,
};

fn numbered(n: usize) -> Ws {
    let files: Vec<(String, Vec<u8>)> = (0..n)
        .map(|i| (format!("f{i}.txt"), format!("original {i}\n").into_bytes()))
        .collect();
    let refs: Vec<(&str, &[u8])> = files
        .iter()
        .map(|(p, b)| (p.as_str(), b.as_slice()))
        .collect();
    Ws::new(&refs)
}

fn original(i: usize) -> Vec<u8> {
    format!("original {i}\n").into_bytes()
}

fn proposed(i: usize) -> String {
    format!("proposed {i}\n")
}

fn rewrite_all(ws: &Ws, n: usize) -> Vec<FileChange> {
    (0..n)
        .map(|i| ws.rewrite(&format!("f{i}.txt"), &proposed(i)))
        .collect()
}

fn journal_text(dir: &Path) -> String {
    fs::read_to_string(dir.join("journal")).expect("journal")
}

fn inode(path: &Path) -> u64 {
    fs::metadata(path).expect("metadata").ino()
}

fn mode(path: &Path) -> u32 {
    fs::metadata(path).expect("metadata").permissions().mode() & 0o7777
}

#[test]
fn multi_file_apply_writes_exact_bytes_and_stages_the_originals() {
    let ws = Ws::new(&[
        ("a.txt", b"alpha\n"),
        ("b.txt", b"beta\n"),
        ("c.txt", b"gamma\r\n"),
    ]);
    fs::set_permissions(ws.path("c.txt"), fs::Permissions::from_mode(0o640)).expect("chmod");
    let changes = vec![
        ws.edit("a.txt", vec![ByteEdit::new(0, 5, "ALPHA")]),
        create("src/new/mod.rs", "pub fn f() {}\n"),
        ws.delete("b.txt"),
        ws.rename("c.txt", "lib/c.txt"),
    ];
    let proposal = ws.proposal(changes);
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    assert_eq!(report.outcome, Outcome::Applied, "{:?}", report.failure);
    assert_eq!(report.status(), titan_changes::ProposalStatus::Applied);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"ALPHA\n"[..]));
    assert_eq!(
        ws.read("src/new/mod.rs").as_deref(),
        Some(&b"pub fn f() {}\n"[..])
    );
    assert_eq!(ws.read("b.txt"), None);
    assert_eq!(ws.read("c.txt"), None);
    assert_eq!(ws.read("lib/c.txt").as_deref(), Some(&b"gamma\r\n"[..]));
    assert_eq!(mode(&ws.path("lib/c.txt")), 0o640);
    // No temporary or aside files are left in the workspace.
    assert_eq!(ws.files(), ["a.txt", "lib/c.txt", "src/new/mod.rs"]);
    // New files get the process default mode, like any other created file.
    fs::write(ws.state.join("probe"), b"").expect("probe");
    assert_eq!(
        mode(&ws.path("src/new/mod.rs")),
        mode(&ws.state.join("probe"))
    );

    // Five operations: write, create, delete, create (rename target), delete (source).
    let states: Vec<&OpState> = report.ops.iter().map(|op| &op.state).collect();
    assert_eq!(states, [&OpState::Applied; 5]);
    let kinds: Vec<_> = report
        .ops
        .iter()
        .map(|op| (op.kind, op.path.as_str()))
        .collect();
    use titan_changes::OpKind::{Create, Delete, Write};
    assert_eq!(
        kinds,
        [
            (Write, "a.txt"),
            (Create, "src/new/mod.rs"),
            (Delete, "b.txt"),
            (Create, "lib/c.txt"),
            (Delete, "c.txt"),
        ]
    );
    // The originals are staged in the transaction directory.
    let dir = &report.txn_dir;
    assert!(dir.starts_with(ws.state.join("changes")));
    assert_eq!(fs::read(dir.join("before/0")).expect("copy"), b"alpha\n");
    assert_eq!(fs::read(dir.join("before/2")).expect("copy"), b"beta\n");
    assert_eq!(fs::read(dir.join("before/4")).expect("copy"), b"gamma\r\n");
    assert_eq!(
        fs::read(dir.join("after/1")).expect("copy"),
        b"pub fn f() {}\n"
    );
    let journal = journal_text(dir);
    assert!(journal.starts_with("TITANCHG1\n"));
    assert!(
        journal.trim_end().ends_with("{\"type\":\"committed\"}"),
        "{journal}"
    );
    assert_eq!(report.affected.len(), 5);
    assert!(report.affected.iter().all(|a| a.target == Target::Disk));
}

#[test]
fn injected_failure_on_the_third_of_five_files_rolls_back_the_first_two() {
    let ws = numbered(5);
    let untouched: Vec<u64> = (2..5)
        .map(|i| inode(&ws.path(&format!("f{i}.txt"))))
        .collect();
    let proposal = ws.proposal(rewrite_all(&ws, 5));
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::BeforeOp { index: 2 } => HookAction::Fail("disk on fire".into()),
        _ => HookAction::Continue,
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::RolledBack);
    assert!(
        report.failure.as_deref().unwrap_or("").contains("f2.txt"),
        "{:?}",
        report.failure
    );
    for i in 0..5 {
        assert_eq!(ws.read(&format!("f{i}.txt")), Some(original(i)), "file {i}");
    }
    // Files 3-5 were never touched at all (same inode, never replaced).
    let after: Vec<u64> = (2..5)
        .map(|i| inode(&ws.path(&format!("f{i}.txt"))))
        .collect();
    assert_eq!(after, untouched);
    let states: Vec<OpState> = report.ops.iter().map(|op| op.state.clone()).collect();
    assert_eq!(states[0], OpState::Undone);
    assert_eq!(states[1], OpState::Undone);
    assert!(matches!(states[2], OpState::NotApplied { .. }));
    assert_eq!(states[3], OpState::NotAttempted);
    assert_eq!(states[4], OpState::NotAttempted);
    assert!(report.rollback_conflicts.is_empty());
    assert_eq!(
        ws.files(),
        ["f0.txt", "f1.txt", "f2.txt", "f3.txt", "f4.txt"]
    );
    assert!(
        journal_text(&report.txn_dir)
            .trim_end()
            .ends_with("{\"type\":\"rolledBack\"}")
    );
    assert_eq!(report.status(), titan_changes::ProposalStatus::RolledBack);
}

#[test]
fn a_failure_right_after_a_write_undoes_that_write_too() {
    let ws = numbered(3);
    let proposal = ws.proposal(rewrite_all(&ws, 3));
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::AfterOp { index: 1 } => HookAction::Fail("late".into()),
        _ => HookAction::Continue,
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::RolledBack);
    for i in 0..3 {
        assert_eq!(ws.read(&format!("f{i}.txt")), Some(original(i)));
    }
    assert_eq!(report.ops[1].state, OpState::Undone);
    assert_eq!(report.ops[2].state, OpState::NotAttempted);
}

#[test]
fn a_file_changed_during_rollback_is_kept_and_both_versions_are_reported() {
    let ws = numbered(3);
    let proposal = ws.proposal(rewrite_all(&ws, 3));
    let f1 = ws.path("f1.txt");
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::BeforeOp { index: 2 } => HookAction::Fail("boom".into()),
        ApplyPoint::BeforeUndo { index: 1 } => {
            fs::write(&f1, b"theirs\n").expect("external write");
            HookAction::Continue
        }
        _ => HookAction::Continue,
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::PartiallyApplied);
    assert_eq!(ws.read("f0.txt"), Some(original(0)));
    assert_eq!(ws.read("f1.txt").as_deref(), Some(&b"theirs\n"[..]));
    assert_eq!(ws.read("f2.txt"), Some(original(2)));
    assert_eq!(report.rollback_conflicts.len(), 1);
    let conflict = &report.rollback_conflicts[0];
    assert_eq!(conflict.path, "f1.txt");
    assert_eq!(conflict.disk_path, f1);
    let original_copy = conflict.original_copy.as_ref().expect("original copy");
    let proposed_copy = conflict.proposed_copy.as_ref().expect("proposed copy");
    assert_eq!(fs::read(original_copy).expect("read"), original(1));
    assert_eq!(
        fs::read(proposed_copy).expect("read"),
        proposed(1).into_bytes()
    );
    assert_eq!(report.ops[1].state, OpState::UndoFailed);
    assert!(journal_text(&report.txn_dir).contains("rollbackIncomplete"));
    // The transaction is finished; recovery leaves it alone.
    assert!(recover(&ws.state).expect("recover").transactions.is_empty());
    assert_eq!(ws.read("f1.txt").as_deref(), Some(&b"theirs\n"[..]));
}

#[test]
fn a_crash_after_two_applied_files_is_rolled_back_by_recover() {
    let ws = numbered(4);
    let proposal = ws.proposal(rewrite_all(&ws, 4));
    let err = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::AfterOp { index: 1 } => HookAction::Crash,
        _ => HookAction::Continue,
    })
    .expect_err("simulated crash");
    let ApplyError::Interrupted { txn_dir, .. } = err else {
        panic!("expected Interrupted, got {err:?}");
    };
    // The "crashed" state: two files written, two untouched, no terminal record.
    assert_eq!(ws.read("f0.txt"), Some(proposed(0).into_bytes()));
    assert_eq!(ws.read("f1.txt"), Some(proposed(1).into_bytes()));
    assert_eq!(ws.read("f2.txt"), Some(original(2)));
    let journal = journal_text(&txn_dir);
    assert!(
        journal.contains("{\"type\":\"applied\",\"index\":1}"),
        "{journal}"
    );
    assert!(!journal.contains("committed"));

    let report = recover(&ws.state).expect("recover");
    assert_eq!(report.transactions.len(), 1);
    let txn = &report.transactions[0];
    assert_eq!(txn.action, RecoveryAction::RolledBack);
    assert_eq!(txn.undone, ["f1.txt", "f0.txt"]);
    assert_eq!(txn.proposal_id.as_deref(), Some(proposal.id.as_str()));
    for i in 0..4 {
        assert_eq!(ws.read(&format!("f{i}.txt")), Some(original(i)));
    }
    assert_eq!(ws.files(), ["f0.txt", "f1.txt", "f2.txt", "f3.txt"]);
    // Idempotent: nothing left to do.
    assert!(recover(&ws.state).expect("recover").transactions.is_empty());
}

#[test]
fn a_crash_during_the_rollback_is_finished_by_recover() {
    let ws = numbered(3);
    let proposal = ws.proposal(rewrite_all(&ws, 3));
    let err = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::BeforeOp { index: 2 } => HookAction::Fail("boom".into()),
        ApplyPoint::BeforeUndo { index: 0 } => HookAction::Crash,
        _ => HookAction::Continue,
    })
    .expect_err("simulated crash");
    assert!(matches!(err, ApplyError::Interrupted { .. }));
    assert_eq!(ws.read("f0.txt"), Some(proposed(0).into_bytes()));
    assert_eq!(ws.read("f1.txt"), Some(original(1)));
    let report = recover(&ws.state).expect("recover");
    assert_eq!(report.transactions[0].action, RecoveryAction::RolledBack);
    for i in 0..3 {
        assert_eq!(ws.read(&format!("f{i}.txt")), Some(original(i)));
    }
}

#[test]
fn creates_never_clobber_a_file_that_appears_concurrently() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    let proposal = ws.proposal(vec![
        ws.rewrite("a.txt", "A\n"),
        create("new.txt", "ours\n"),
    ]);
    let target = ws.path("new.txt");
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::BeforeOp { index: 1 } => {
            fs::write(&target, b"theirs\n").expect("concurrent create");
            HookAction::Continue
        }
        _ => HookAction::Continue,
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::RolledBack);
    assert_eq!(ws.read("new.txt").as_deref(), Some(&b"theirs\n"[..]));
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"a\n"[..]));
    match &report.ops[1].state {
        OpState::NotApplied { reason } => assert!(reason.contains("created"), "{reason}"),
        other => panic!("unexpected {other:?}"),
    }
    assert_eq!(ws.files(), ["a.txt", "new.txt"]);
}

#[test]
fn a_change_made_inside_the_save_window_is_detected() {
    let ws = Ws::new(&[("a.txt", b"base\n")]);
    let proposal = ws.proposal(vec![ws.rewrite("a.txt", "ours\n")]);
    let target = ws.path("a.txt");
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| {
        if point
            == (ApplyPoint::Save {
                index: 0,
                phase: SavePhase::BeforeReplace,
            })
        {
            // Same length, in place: only the post-swap verification can notice.
            fs::write(&target, b"edit\n").expect("external write");
        }
        HookAction::Continue
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::Failed, "{:?}", report.ops);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"edit\n"[..]));
    assert_eq!(ws.files(), ["a.txt"]);
}

#[test]
fn deletes_verify_the_content_before_removing_anything() {
    let ws = Ws::new(&[("a.txt", b"delete me\n")]);
    let proposal = ws.proposal(vec![ws.delete("a.txt")]);
    let target = ws.path("a.txt");
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| {
        if point == (ApplyPoint::BeforeOp { index: 0 }) {
            fs::write(&target, b"keep me!!\n").expect("external write");
        }
        HookAction::Continue
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::Failed);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"keep me!!\n"[..]));
    assert_eq!(ws.files(), ["a.txt"]);
}

#[test]
fn renames_verify_the_source_and_remove_the_copy_on_failure() {
    let ws = Ws::new(&[("a.txt", b"content\n")]);
    let proposal = ws.proposal(vec![ws.rename("a.txt", "dir/b.txt")]);
    let source = ws.path("a.txt");
    let report = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| {
        // Operation 0 created dir/b.txt; operation 1 is about to delete the source.
        if point == (ApplyPoint::BeforeOp { index: 1 }) {
            fs::write(&source, b"edited\n").expect("external write");
        }
        HookAction::Continue
    })
    .expect("apply runs");
    assert_eq!(report.outcome, Outcome::RolledBack);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"edited\n"[..]));
    assert_eq!(ws.read("dir/b.txt"), None);
    // The directory created for the target was removed again.
    assert!(!ws.path("dir").exists());
    assert_eq!(ws.files(), ["a.txt"]);
}

#[test]
fn restore_puts_back_the_originals_after_a_successful_apply() {
    let ws = Ws::new(&[("a.txt", b"a\n"), ("b.txt", b"b\n"), ("c.txt", b"c\n")]);
    let proposal = ws.proposal(vec![
        ws.rewrite("a.txt", "A\n"),
        create("d/new.txt", "new\n"),
        ws.delete("b.txt"),
        ws.rename("c.txt", "e.txt"),
    ]);
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    assert_eq!(report.outcome, Outcome::Applied);
    let mut evidence = EvidenceRecord::from_apply(&proposal, &report);
    evidence.checks.push(CheckRecord::not_run(
        "cargo test",
        "no test command configured",
    ));
    assert!(evidence.restore.available);
    write_evidence(&ws.state, &evidence).expect("write evidence");
    let loaded = load_evidence(&ws.state, &proposal.id).expect("load");

    let restored = restore(&ws.state, &loaded).expect("restore");
    assert!(restored.restored, "{restored:?}");
    assert!(restored.refused.is_empty());
    assert_eq!(ws.files(), ["a.txt", "b.txt", "c.txt"]);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"a\n"[..]));
    assert_eq!(ws.read("b.txt").as_deref(), Some(&b"b\n"[..]));
    assert_eq!(ws.read("c.txt").as_deref(), Some(&b"c\n"[..]));
    // A second restore is refused: the transaction is no longer in the committed state.
    assert!(matches!(
        restore(&ws.state, &loaded),
        Err(ChangeError::NotRestorable { .. })
    ));
}

#[test]
fn restore_is_refused_when_a_file_changed_after_apply() {
    let ws = Ws::new(&[("a.txt", b"a\n"), ("b.txt", b"b\n")]);
    let proposal = ws.proposal(vec![ws.rewrite("a.txt", "A\n"), ws.rewrite("b.txt", "B\n")]);
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    let evidence = EvidenceRecord::from_apply(&proposal, &report);
    ws.write("b.txt", b"later edit\n");
    let restored = restore(&ws.state, &evidence).expect("restore runs");
    assert!(!restored.restored);
    assert_eq!(restored.refused.len(), 1);
    assert_eq!(restored.refused[0].path, "b.txt");
    assert!(restored.refused[0].detail.contains("changed since"));
    // Nothing was touched.
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"A\n"[..]));
    assert_eq!(ws.read("b.txt").as_deref(), Some(&b"later edit\n"[..]));
    // Once the file is back to the applied content, restoring works.
    ws.write("b.txt", b"B\n");
    assert!(restore(&ws.state, &evidence).expect("restore").restored);
    assert_eq!(ws.read("b.txt").as_deref(), Some(&b"b\n"[..]));
}

#[test]
fn evidence_round_trips_through_json_files() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    let mut proposal = ws.proposal(vec![ws.rewrite("a.txt", "A\n")]);
    proposal.status = titan_changes::ProposalStatus::Approved {
        by: "dev".into(),
        at_unix_ms: 42,
    };
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    let mut record = EvidenceRecord::from_apply(&proposal, &report);
    record.checks = vec![
        CheckRecord {
            name: "cargo test".into(),
            status: CheckStatus::Passed,
            exit_code: Some(0),
            duration_ms: Some(1234),
            log_excerpt: "test result: ok. 3 passed".into(),
            details: serde_json::json!({"command": ["cargo", "test"]}),
        },
        CheckRecord {
            name: "cargo clippy".into(),
            status: CheckStatus::Failed,
            exit_code: Some(101),
            duration_ms: Some(10),
            log_excerpt: "error: ...".into(),
            details: serde_json::Value::Null,
        },
        CheckRecord {
            name: "bench".into(),
            status: CheckStatus::TimedOut,
            exit_code: None,
            duration_ms: Some(60_000),
            log_excerpt: String::new(),
            details: serde_json::Value::Null,
        },
        CheckRecord {
            name: "lsp diagnostics".into(),
            status: CheckStatus::Error {
                message: "server not running".into(),
            },
            exit_code: None,
            duration_ms: None,
            log_excerpt: String::new(),
            details: serde_json::Value::Null,
        },
        CheckRecord::not_run("integration tests", "no network in this environment"),
    ];
    record.notes.push("recorded by the test".into());
    let path = write_evidence(&ws.state, &record).expect("write");
    assert_eq!(
        path,
        ws.state
            .join("changes/evidence")
            .join(format!("{}.json", proposal.id))
    );
    let loaded = load_evidence(&ws.state, &proposal.id).expect("load");
    assert_eq!(loaded, record);
    assert_eq!(
        loaded.approval,
        Some(titan_changes::Approval {
            by: "dev".into(),
            at_unix_ms: 42
        })
    );
    let text = fs::read_to_string(&path).expect("read");
    for key in [
        "proposalId",
        "patchId",
        "beforeHash",
        "notRun",
        "logExcerpt",
        "txnDir",
    ] {
        assert!(text.contains(key), "missing {key}");
    }
    assert!(!text.to_lowercase().contains("proof"));
    // Atomic write: only the record itself is in the directory.
    let names: Vec<String> = fs::read_dir(ws.state.join("changes/evidence"))
        .expect("dir")
        .map(|e| e.expect("entry").file_name().to_string_lossy().into_owned())
        .collect();
    assert_eq!(names, [format!("{}.json", proposal.id)]);
    // Rewriting replaces it.
    record.notes.push("second write".into());
    write_evidence(&ws.state, &record).expect("rewrite");
    let listing = list_evidence(&ws.state).expect("list");
    assert_eq!(listing.records, [record]);
    assert!(listing.unreadable.is_empty());
    // Damaged files are listed as unreadable, not silently dropped.
    fs::write(ws.state.join("changes/evidence/broken.json"), b"{not json").expect("write");
    let listing = list_evidence(&ws.state).expect("list");
    assert_eq!(listing.records.len(), 1);
    assert_eq!(listing.unreadable.len(), 1);
    let missing = titan_changes::ProposalId::parse("nope").expect("id");
    assert!(load_evidence(&ws.state, &missing).is_err());
}

#[test]
fn buffer_targets_are_returned_to_the_caller_and_never_written() {
    let ws = Ws::new(&[("a.txt", b"disk text\n"), ("b.txt", b"bee\n")]);
    let view = BufferView {
        disk: ws.view.clone(),
        buffers: HashMap::from([(ws.path("a.txt"), (5, "buffer text\n".to_string()))]),
    };
    let buffer_edit = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::buffer(5, "buffer text\n"),
        edits: vec![ByteEdit::new(0, 6, "BUFFER")],
    };
    let proposal = ws.proposal(vec![buffer_edit, ws.rewrite("b.txt", "BEE\n")]);
    let report = apply(&proposal, &view, &ws.state).expect("apply");
    assert_eq!(report.outcome, Outcome::Applied);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"disk text\n"[..]));
    assert_eq!(ws.read("b.txt").as_deref(), Some(&b"BEE\n"[..]));
    assert_eq!(report.buffer_edits.len(), 1);
    let edits = &report.buffer_edits[0];
    assert_eq!(edits.path, "a.txt");
    assert_eq!(edits.expected_revision, 5);
    assert_eq!(edits.edits, [ByteEdit::new(0, 6, "BUFFER")]);
    assert_eq!(
        report.ops.len(),
        1,
        "only the disk file is in the transaction"
    );
    let targets: Vec<Target> = report.affected.iter().map(|a| a.target).collect();
    assert_eq!(targets, [Target::Buffer, Target::Disk]);
}

#[test]
fn invalid_proposals_write_nothing() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    let stale = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::disk(b"old\n"),
        edits: vec![ByteEdit::new(0, 0, "x")],
    };
    let err = apply(&ws.proposal(vec![stale]), &ws.view, &ws.state).expect_err("invalid");
    match err {
        ApplyError::Invalid(report) => {
            assert_eq!(report.problems[0].kind, titan_changes::ProblemKind::Stale);
        }
        other => panic!("unexpected {other:?}"),
    }
    assert!(!ws.state.join("changes").exists());
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"a\n"[..]));
}

#[test]
fn recover_skips_finished_transactions_and_never_acts_on_damaged_journals() {
    let ws = numbered(2);
    let proposal = ws.proposal(rewrite_all(&ws, 2));
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    assert_eq!(report.outcome, Outcome::Applied);
    assert!(recover(&ws.state).expect("recover").transactions.is_empty());

    // Corrupt a record in the middle of the journal: recovery must not roll back an
    // applied change based on it.
    let journal_path = report.txn_dir.join("journal");
    let text = fs::read_to_string(&journal_path).expect("journal");
    let damaged = text.replacen("\"begin\"", "\"bEgin\"", 1);
    assert_ne!(damaged, text);
    fs::write(&journal_path, damaged).expect("write");
    let recovered = recover(&ws.state).expect("recover");
    assert_eq!(recovered.transactions.len(), 1);
    assert_eq!(recovered.transactions[0].action, RecoveryAction::Unreadable);
    assert_eq!(ws.read("f0.txt"), Some(proposed(0).into_bytes()));
    assert_eq!(ws.read("f1.txt"), Some(proposed(1).into_bytes()));

    // A transaction directory without a journal (crash during preparation).
    fs::create_dir_all(ws.state.join("changes/txn-abc")).expect("mkdir");
    let recovered = recover(&ws.state).expect("recover");
    let empty = recovered
        .transactions
        .iter()
        .find(|t| t.txn_id == "abc")
        .expect("listed");
    assert_eq!(empty.action, RecoveryAction::NothingToDo);
}

#[test]
fn a_torn_final_journal_record_is_ignored() {
    let ws = numbered(2);
    let proposal = ws.proposal(rewrite_all(&ws, 2));
    let err = apply_with_hook(&proposal, &ws.view, &ws.state, &mut |point| match point {
        ApplyPoint::AfterOp { index: 0 } => HookAction::Crash,
        _ => HookAction::Continue,
    })
    .expect_err("crash");
    let ApplyError::Interrupted { txn_dir, .. } = err else {
        panic!("unexpected {err:?}");
    };
    // Half of a record, as an interrupted write would leave it.
    let mut journal = fs::OpenOptions::new()
        .append(true)
        .open(txn_dir.join("journal"))
        .expect("open");
    std::io::Write::write_all(&mut journal, b"0badc0de {\"type\":\"beg").expect("append");
    drop(journal);
    let report = recover(&ws.state).expect("recover");
    assert_eq!(report.transactions[0].action, RecoveryAction::RolledBack);
    assert!(
        report.transactions[0]
            .notes
            .iter()
            .any(|n| n.contains("incomplete"))
    );
    assert_eq!(ws.read("f0.txt"), Some(original(0)));
    assert_eq!(ws.read("f1.txt"), Some(original(1)));
}
