//! Validation problems, previews and patch ids on a real file system.

mod common;

use std::collections::HashMap;
use std::fs;

use common::{BufferView, Ws, create};
use titan_changes::{
    ApplyError, ByteEdit, ChangeAnnotation, ExpectedBase, FileChange, PreviewKind, Problem,
    ProblemKind, ProposalStatus, Target, WorkspaceView, apply, patch_id, validate,
};

fn kinds(problems: &[Problem]) -> Vec<ProblemKind> {
    problems.iter().map(|p| p.kind).collect()
}

fn problems_of(ws: &Ws, view: &dyn WorkspaceView, changes: Vec<FileChange>) -> Vec<ProblemKind> {
    let report = validate(&ws.proposal(changes), view);
    assert_eq!(report.ok, report.problems.is_empty());
    kinds(&report.problems)
}

#[test]
fn a_valid_proposal_has_no_problems_and_previews_every_change() {
    let ws = Ws::new(&[
        ("a.txt", b"one\ntwo\n"),
        ("b.txt", b"bee\n"),
        ("c.txt", b"sea\n"),
    ]);
    let changes = vec![
        ws.edit("a.txt", vec![ByteEdit::new(4, 7, "TWO")]),
        create("new/dir/d.txt", "dee\n"),
        ws.delete("b.txt"),
        ws.rename("c.txt", "moved/c.txt"),
    ];
    let report = validate(&ws.proposal(changes), &ws.view);
    assert!(report.ok, "{:?}", report.problems);
    let preview_kinds: Vec<PreviewKind> = report.preview.iter().map(|p| p.kind).collect();
    assert_eq!(
        preview_kinds,
        [
            PreviewKind::Edit,
            PreviewKind::Create,
            PreviewKind::Delete,
            PreviewKind::Rename
        ]
    );
    assert!(report.preview.iter().all(|p| p.target == Target::Disk));
    assert_eq!(report.preview[0].edits_applied, 1);
    assert_eq!(report.preview[0].after_len, Some(8));
    // Validation never writes.
    assert_eq!(ws.files(), ["a.txt", "b.txt", "c.txt"]);
}

#[test]
fn paths_must_stay_inside_the_workspace() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    let outside = tempfile::tempdir().expect("tempdir");
    fs::write(outside.path().join("secret.txt"), "secret\n").expect("write");
    std::os::unix::fs::symlink(outside.path(), ws.path("escape")).expect("symlink");
    std::os::unix::fs::symlink(ws.path("a.txt"), ws.path("alias.txt")).expect("symlink");
    fs::create_dir(ws.path("sub")).expect("mkdir");
    std::os::unix::fs::symlink(ws.path("sub"), ws.path("inner")).expect("symlink");

    let cases: Vec<(&str, ProblemKind)> = vec![
        ("../a.txt", ProblemKind::InvalidPath),
        ("sub/../../a.txt", ProblemKind::InvalidPath),
        ("", ProblemKind::InvalidPath),
        ("/etc/passwd", ProblemKind::OutsideRoot),
        ("escape/secret.txt", ProblemKind::OutsideRoot),
        ("escape/new.txt", ProblemKind::OutsideRoot),
        ("alias.txt", ProblemKind::InvalidPath),
        ("a.txt/child", ProblemKind::InvalidPath),
    ];
    for (path, kind) in cases {
        let got = problems_of(&ws, &ws.view, vec![create(path, "x")]);
        assert_eq!(got, [kind], "path {path:?}");
    }
    // A symlinked directory that stays inside the workspace is fine.
    assert_eq!(
        problems_of(&ws, &ws.view, vec![create("inner/new.txt", "x")]),
        []
    );
    // Nothing was created anywhere.
    assert!(!outside.path().join("new.txt").exists());
    assert!(!ws.path("sub/new.txt").exists());
}

#[test]
fn stale_bases_are_refused() {
    let ws = Ws::new(&[("a.txt", b"hello\n")]);
    let stale_hash = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::disk(b"something else\n"),
        edits: vec![],
    };
    let wrong_len = FileChange::Edit {
        path: "a.txt".into(),
        expected: match ExpectedBase::disk(b"hello\n") {
            ExpectedBase::Disk { hash, .. } => ExpectedBase::Disk { hash, len: 99 },
            other => other,
        },
        edits: vec![],
    };
    let missing = FileChange::Edit {
        path: "gone.txt".into(),
        expected: ExpectedBase::disk(b"x"),
        edits: vec![],
    };
    let delete_stale = FileChange::Delete {
        path: "a.txt".into(),
        expected: ExpectedBase::disk(b"old\n"),
        annotation: None,
    };
    let rename_missing = FileChange::Rename {
        from: "gone.txt".into(),
        to: "b.txt".into(),
        expected: ExpectedBase::disk(b"x"),
        annotation: None,
    };
    for change in [stale_hash, wrong_len, missing, delete_stale, rename_missing] {
        assert_eq!(
            problems_of(&ws, &ws.view, vec![change]),
            [ProblemKind::Stale]
        );
    }

    // Buffers: stale revision and stale text.
    let view = BufferView {
        disk: ws.view.clone(),
        buffers: HashMap::from([(ws.path("a.txt"), (7, "hello, buffer\n".to_string()))]),
    };
    let old_revision = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::buffer(6, "hello, buffer\n"),
        edits: vec![],
    };
    let old_text = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::buffer(7, "hello\n"),
        edits: vec![],
    };
    for change in [old_revision, old_text] {
        assert_eq!(problems_of(&ws, &view, vec![change]), [ProblemKind::Stale]);
    }
}

#[test]
fn edit_ranges_are_checked() {
    let ws = Ws::new(&[("a.txt", "héllo wörld\n".as_bytes())]);
    let len = "héllo wörld\n".len();
    let cases: Vec<(Vec<ByteEdit>, ProblemKind)> = vec![
        (
            vec![ByteEdit::new(0, len + 1, "")],
            ProblemKind::EditOutOfBounds,
        ),
        (vec![ByteEdit::new(5, 3, "")], ProblemKind::EditOutOfBounds),
        // 'é' occupies bytes 1..3.
        (
            vec![ByteEdit::new(2, 4, "")],
            ProblemKind::EditNotOnCharBoundary,
        ),
        (
            vec![ByteEdit::new(6, 7, "W"), ByteEdit::new(0, 1, "H")],
            ProblemKind::EditsUnsorted,
        ),
        (
            vec![ByteEdit::new(0, 5, "x"), ByteEdit::new(3, 6, "y")],
            ProblemKind::EditsOverlap,
        ),
        (
            vec![ByteEdit::new(0, 5, "x"), ByteEdit::new(0, 0, "y")],
            ProblemKind::EditsOverlap,
        ),
    ];
    for (edits, kind) in cases {
        let got = problems_of(&ws, &ws.view, vec![ws.edit("a.txt", edits.clone())]);
        assert_eq!(got, [kind], "{edits:?}");
    }
    // Several insertions at one offset are applied in order; an insertion may precede a
    // replacement starting at the same offset.
    let edits = vec![
        ByteEdit::new(0, 0, "1"),
        ByteEdit::new(0, 0, "2"),
        ByteEdit::new(0, 1, "H"),
        ByteEdit::new(len, len, "end\n"),
    ];
    let report = validate(&ws.proposal(vec![ws.edit("a.txt", edits)]), &ws.view);
    assert!(report.ok, "{:?}", report.problems);
    assert_eq!(
        report.preview[0].unified_diff,
        "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1,2 @@\n-héllo wörld\n+12Héllo wörld\n+end\n"
    );
}

#[test]
fn non_utf8_files_cannot_be_edited_but_can_be_deleted() {
    let ws = Ws::new(&[("bin.dat", &[0x66, 0xff, 0xfe, 0x0a])]);
    assert_eq!(
        problems_of(&ws, &ws.view, vec![ws.edit("bin.dat", vec![])]),
        [ProblemKind::NotUtf8]
    );
    let report = validate(&ws.proposal(vec![ws.delete("bin.dat")]), &ws.view);
    assert!(report.ok);
    assert_eq!(
        report.preview[0].unified_diff,
        "binary file a/bin.dat deleted (4 bytes)\n"
    );
}

#[test]
fn creates_and_renames_never_target_existing_paths() {
    let ws = Ws::new(&[("a.txt", b"a\n"), ("b.txt", b"b\n")]);
    assert_eq!(
        problems_of(&ws, &ws.view, vec![create("a.txt", "new")]),
        [ProblemKind::TargetExists]
    );
    assert_eq!(
        problems_of(&ws, &ws.view, vec![ws.rename("a.txt", "b.txt")]),
        [ProblemKind::TargetExists]
    );
    // An open (unsaved, never written) buffer at the path also counts.
    let view = BufferView {
        disk: ws.view.clone(),
        buffers: HashMap::from([(ws.path("c.txt"), (1, "draft".to_string()))]),
    };
    assert_eq!(
        problems_of(&ws, &view, vec![create("c.txt", "new")]),
        [ProblemKind::TargetExists]
    );
}

#[test]
fn one_path_may_not_be_changed_twice() {
    let ws = Ws::new(&[("a.txt", b"a\n"), ("d/x.txt", b"x\n")]);
    let twice = vec![
        ws.edit("a.txt", vec![ByteEdit::new(0, 0, "1")]),
        ws.edit("./a.txt", vec![ByteEdit::new(1, 1, "2")]),
    ];
    assert_eq!(
        problems_of(&ws, &ws.view, twice),
        [ProblemKind::PathConflict]
    );
    let rename_onto_deleted = vec![ws.delete("a.txt"), ws.rename("d/x.txt", "a.txt")];
    // Rename target exists (validation sees the current disk) and conflicts with the delete.
    let got = problems_of(&ws, &ws.view, rename_onto_deleted);
    assert!(got.contains(&ProblemKind::PathConflict), "{got:?}");
    let nested = vec![create("n", "file"), create("n/inner.txt", "inside")];
    assert_eq!(
        problems_of(&ws, &ws.view, nested),
        [ProblemKind::PathConflict]
    );
}

#[test]
fn unsaved_buffers_are_protected_and_buffer_expectations_need_a_buffer() {
    let ws = Ws::new(&[("a.txt", b"disk\n"), ("b.txt", b"bee\n")]);
    let view = BufferView {
        disk: ws.view.clone(),
        buffers: HashMap::from([(ws.path("a.txt"), (3, "unsaved\n".to_string()))]),
    };
    // Computed against the disk while the buffer has unsaved edits.
    assert_eq!(
        problems_of(
            &ws,
            &view,
            vec![ws.edit("a.txt", vec![ByteEdit::new(0, 0, "x")])]
        ),
        [ProblemKind::BufferIsOpen]
    );
    assert_eq!(
        problems_of(&ws, &view, vec![ws.delete("a.txt")]),
        [ProblemKind::BufferIsOpen]
    );
    // Computed against a buffer that is not open.
    let ghost = FileChange::Edit {
        path: "b.txt".into(),
        expected: ExpectedBase::buffer(1, "bee\n"),
        edits: vec![],
    };
    assert_eq!(
        problems_of(&ws, &view, vec![ghost]),
        [ProblemKind::BufferNotOpen]
    );
    // The right expectation validates and previews as a buffer target.
    let ok = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::buffer(3, "unsaved\n"),
        edits: vec![ByteEdit::new(0, 7, "saved")],
    };
    let report = validate(&ws.proposal(vec![ok]), &view);
    assert!(report.ok, "{:?}", report.problems);
    assert_eq!(report.preview[0].target, Target::Buffer);
    assert_eq!(
        report.preview[0].unified_diff,
        "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-unsaved\n+saved\n"
    );
}

#[test]
fn malformed_changes_and_unreadable_targets_are_reported() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    fs::create_dir(ws.path("dir")).expect("mkdir");
    let absent_edit = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::Absent,
        edits: vec![],
    };
    let bad_hash = FileChange::Edit {
        path: "a.txt".into(),
        expected: ExpectedBase::Disk {
            hash: "not-a-hash".into(),
            len: 2,
        },
        edits: vec![],
    };
    let self_rename = ws.rename("a.txt", "./a.txt");
    let delete_buffer = FileChange::Delete {
        path: "a.txt".into(),
        expected: ExpectedBase::buffer(1, "a\n"),
        annotation: None,
    };
    for change in [absent_edit, bad_hash, self_rename, delete_buffer] {
        assert_eq!(
            problems_of(&ws, &ws.view, vec![change.clone()]),
            [ProblemKind::InvalidChange],
            "{change:?}"
        );
    }
    let dir_edit = FileChange::Edit {
        path: "dir".into(),
        expected: ExpectedBase::disk(b""),
        edits: vec![],
    };
    assert_eq!(
        problems_of(&ws, &ws.view, vec![dir_edit]),
        [ProblemKind::Unreadable]
    );
}

#[test]
fn annotations_must_exist_and_confirmation_requires_approval() {
    let ws = Ws::new(&[("a.txt", b"fn old() {}\n")]);
    let mut edit = ByteEdit::new(3, 6, "new");
    edit.annotation = Some("rename-public".into());
    let mut proposal = ws.proposal(vec![ws.edit("a.txt", vec![edit])]);
    let report = validate(&proposal, &ws.view);
    assert_eq!(kinds(&report.problems), [ProblemKind::UnknownAnnotation]);

    proposal.annotations.push(ChangeAnnotation {
        id: "rename-public".into(),
        label: "Renames a public item".into(),
        needs_confirmation: true,
    });
    let report = validate(&proposal, &ws.view);
    assert!(report.ok);
    assert_eq!(report.needs_confirmation.len(), 1);
    assert_eq!(report.needs_confirmation[0].paths, ["a.txt"]);

    match apply(&proposal, &ws.view, &ws.state) {
        Err(ApplyError::NeedsApproval { annotations }) => {
            assert_eq!(annotations, ["rename-public"]);
        }
        other => panic!("expected NeedsApproval, got {other:?}"),
    }
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"fn old() {}\n"[..]));
    proposal.status = ProposalStatus::Approved {
        by: "reviewer".into(),
        at_unix_ms: 1,
    };
    let report = apply(&proposal, &ws.view, &ws.state).expect("apply");
    assert_eq!(report.outcome, titan_changes::Outcome::Applied);
    assert_eq!(ws.read("a.txt").as_deref(), Some(&b"fn new() {}\n"[..]));
}

#[test]
fn preview_goldens_for_create_delete_rename_crlf_and_missing_newline() {
    let ws = Ws::new(&[
        ("crlf.txt", b"one\r\ntwo\r\nthree\r\n"),
        ("tail.txt", b"a\nb"),
        ("gone.txt", b"x\ny\n"),
        ("old.txt", b"same\n"),
    ]);
    let changes = vec![
        ws.edit("crlf.txt", vec![ByteEdit::new(5, 8, "TWO")]),
        ws.edit("tail.txt", vec![ByteEdit::new(3, 3, "\nc")]),
        create("made.txt", "new\nfile"),
        ws.delete("gone.txt"),
        ws.rename("old.txt", "new.txt"),
    ];
    let report = validate(&ws.proposal(changes), &ws.view);
    assert!(report.ok, "{:?}", report.problems);
    let diffs: Vec<&str> = report
        .preview
        .iter()
        .map(|p| p.unified_diff.as_str())
        .collect();
    assert_eq!(
        diffs,
        [
            "--- a/crlf.txt\n+++ b/crlf.txt\n@@ -1,3 +1,3 @@\n one\r\n-two\r\n+TWO\r\n three\r\n",
            "--- a/tail.txt\n+++ b/tail.txt\n@@ -1,2 +1,3 @@\n a\n-b\n\\ No newline at end of file\n\
             +b\n+c\n\\ No newline at end of file\n",
            "--- /dev/null\n+++ b/made.txt\n@@ -0,0 +1,2 @@\n+new\n+file\n\\ No newline at end of file\n",
            "--- a/gone.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-x\n-y\n",
            "rename from old.txt\nrename to new.txt\n",
        ]
    );
    let rename = &report.preview[4];
    assert_eq!(rename.renamed_to.as_deref(), Some("new.txt"));
    assert_eq!(rename.before_hash, rename.after_hash);
    let delete = &report.preview[3];
    assert_eq!(delete.after_hash, None);
    assert_eq!(delete.after_len, None);
    let created = &report.preview[2];
    assert_eq!(created.before_hash, None);
    assert_eq!(created.after_len, Some(8));
}

#[test]
fn huge_rewrites_fall_back_to_a_summary_with_hashes() {
    let old: String = (0..5000).map(|i| format!("old line {i}\n")).collect();
    let ws = Ws::new(&[("big.txt", old.as_bytes())]);
    let new: String = (0..5000).map(|i| format!("new line {i}\n")).collect();
    let report = validate(&ws.proposal(vec![ws.rewrite("big.txt", &new)]), &ws.view);
    assert!(report.ok);
    let preview = &report.preview[0];
    assert!(preview.diff_truncated);
    assert!(
        preview
            .unified_diff
            .starts_with("file replaced: 5000 lines -> 5000 lines")
    );
    assert!(
        preview
            .unified_diff
            .contains(preview.after_hash.as_deref().unwrap_or("?"))
    );
}

#[test]
fn patch_ids_depend_on_the_changes_only() {
    let ws = Ws::new(&[("a.txt", b"a\n"), ("b.txt", b"b\n")]);
    let changes = vec![
        ws.edit("a.txt", vec![ByteEdit::new(0, 1, "A")]),
        ws.delete("b.txt"),
    ];
    let first = ws.proposal(changes.clone());
    let second = ws.proposal(changes.clone());
    assert_ne!(first.id, second.id);
    assert_eq!(first.patch_id(), second.patch_id());
    assert_eq!(first.patch_id().len(), 64);
    // Survives a JSON round trip of the proposal.
    let json = serde_json::to_string(&first).expect("serialize");
    let back: titan_changes::Proposal = serde_json::from_str(&json).expect("deserialize");
    assert_eq!(back, first);
    assert_eq!(back.patch_id(), first.patch_id());
    // Equivalent paths and hex case give the same id.
    let mut spelled = changes.clone();
    if let FileChange::Edit { path, expected, .. } = &mut spelled[0] {
        *path = "./a.txt".into();
        if let ExpectedBase::Disk { hash, .. } = expected {
            *hash = hash.to_uppercase();
        }
    }
    assert_eq!(patch_id(&spelled), patch_id(&changes));
    // Any change to the edits, their order, or the targets changes it.
    let other = vec![
        ws.edit("a.txt", vec![ByteEdit::new(0, 1, "B")]),
        ws.delete("b.txt"),
    ];
    assert_ne!(patch_id(&other), patch_id(&changes));
    let reversed: Vec<FileChange> = changes.iter().rev().cloned().collect();
    assert_ne!(patch_id(&reversed), patch_id(&changes));
    // Validation reports the same id.
    assert_eq!(validate(&first, &ws.view).patch_id, first.patch_id());
}

#[test]
fn proposal_json_uses_camel_case_and_ids_are_validated() {
    let ws = Ws::new(&[("a.txt", b"a\n")]);
    let proposal = ws.proposal(vec![ws.rename("a.txt", "b.txt")]);
    let json = serde_json::to_value(&proposal).expect("serialize");
    assert!(json.get("createdUnixMs").is_some());
    assert_eq!(json["changes"][0]["kind"], "rename");
    assert_eq!(json["changes"][0]["expected"]["kind"], "disk");
    assert_eq!(json["status"]["state"], "proposed");
    assert_eq!(
        json["base"]["workspaceRoot"],
        ws.root.to_string_lossy().as_ref()
    );
    let mut bad = json.clone();
    bad["id"] = serde_json::json!("../../etc");
    assert!(serde_json::from_value::<titan_changes::Proposal>(bad).is_err());
}
