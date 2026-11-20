use super::*;
use titan_text::{ByteOffset, TextEdit};

fn write(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let path = dir.join(name);
    fs::write(&path, bytes).unwrap();
    path
}

fn names(dir: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    names
}

fn edit(doc: &mut Document, at: usize, text: &str) {
    let rev = doc.text().revision();
    doc.text_mut()
        .apply(rev, vec![TextEdit::insert(ByteOffset(at), text)])
        .unwrap();
}

fn conflict_kind(result: Result<SaveOutcome, DocError>) -> ConflictKind {
    match result {
        Err(DocError::ExternalModification { kind, .. }) => kind,
        other => panic!("expected a conflict, got {other:?}"),
    }
}

#[test]
fn open_edit_undo_redo_save_reopen() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "main.rs", "fn main() {\r\n}\r\n".as_bytes());
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    assert_eq!(doc.line_ending(), LineEnding::Crlf);
    assert!(!doc.is_dirty());

    doc.text_mut()
        .apply(0, vec![TextEdit::insert(ByteOffset(13), "    // سلام\r\n")])
        .unwrap();
    assert!(doc.is_dirty());
    doc.text_mut().undo(1).unwrap();
    assert!(!doc.is_dirty(), "undo back to the saved state is clean");
    doc.text_mut().redo(2).unwrap();
    assert!(doc.is_dirty());

    let outcome = doc.save(3, false).unwrap();
    assert_eq!(outcome.revision, 3);
    assert!(!doc.is_dirty());

    let expected = "fn main() {\r\n    // سلام\r\n}\r\n";
    assert_eq!(fs::read_to_string(&path).unwrap(), expected);
    let reopened = Document::open(&path, &Limits::default()).unwrap();
    assert_eq!(reopened.text().buffer().text(), expected);
    assert_eq!(reopened.line_ending(), LineEnding::Crlf);
    assert_eq!(
        reopened.disk_state().hash,
        ContentHash::of(expected.as_bytes())
    );
}

#[test]
fn bom_and_mixed_line_endings_round_trip() {
    let dir = tempfile::tempdir().unwrap();
    let original = b"\xEF\xBB\xBFa\nb\r\nc\rd";
    let path = write(dir.path(), "mixed.txt", original);
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    assert!(doc.has_bom());
    assert_eq!(doc.line_ending(), LineEnding::Mixed);
    assert_eq!(doc.text().buffer().text(), "a\nb\r\nc\rd");
    doc.save(0, false).unwrap();
    assert_eq!(fs::read(&path).unwrap(), original);
}

#[test]
fn rejects_binary_utf16_invalid_and_large() {
    let dir = tempfile::tempdir().unwrap();
    let bin = write(dir.path(), "a.bin", b"abc\0def");
    assert!(matches!(
        Document::open(&bin, &Limits::default()),
        Err(DocError::Binary { .. })
    ));
    let utf16 = write(dir.path(), "u16.txt", b"\xFF\xFEa\0");
    assert!(matches!(
        Document::open(&utf16, &Limits::default()),
        Err(DocError::UnsupportedEncoding { .. })
    ));
    let bad = write(dir.path(), "bad.txt", b"ok\xC3\x28");
    assert!(matches!(
        Document::open(&bad, &Limits::default()),
        Err(DocError::NotUtf8 { valid_up_to: 2, .. })
    ));
    let big = write(dir.path(), "big.txt", &[b'x'; 100]);
    let small = Limits { max_file_bytes: 10 };
    assert!(matches!(
        Document::open(&big, &small),
        Err(DocError::TooLarge { len: 100, .. })
    ));
}

#[test]
fn external_modification_blocks_save_unless_forced() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "f.txt", b"one");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 3, " two");
    fs::write(&path, b"someone else").unwrap();
    assert!(doc.changed_on_disk().unwrap());
    assert_eq!(conflict_kind(doc.save(1, false)), ConflictKind::Modified);
    assert_eq!(fs::read(&path).unwrap(), b"someone else");
    assert_eq!(doc.text().buffer().text(), "one two", "buffer is preserved");
    assert!(doc.is_dirty());
    doc.save(1, true).unwrap();
    assert_eq!(fs::read(&path).unwrap(), b"one two");
    assert!(!doc.changed_on_disk().unwrap());
}

#[test]
fn same_length_external_change_is_detected_by_content() {
    // Same size, and possibly the same timestamp tick: only the content hash tells.
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "f.txt", b"aaaa");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 0, "x");
    fs::write(&path, b"bbbb").unwrap();
    assert_eq!(conflict_kind(doc.save(1, false)), ConflictKind::Modified);
    assert_eq!(fs::read(&path).unwrap(), b"bbbb");
}

#[test]
fn deleted_file_is_a_conflict_and_can_be_recreated_deliberately() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "gone.txt", b"x");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    fs::remove_file(&path).unwrap();
    assert!(doc.changed_on_disk().unwrap());
    assert_eq!(conflict_kind(doc.save(0, false)), ConflictKind::Deleted);
    assert!(!path.exists(), "a refused save creates nothing");

    // The user keeps the buffer: the document now expects no file.
    doc.adopt_disk_state(DiskState::missing());
    assert!(doc.is_dirty());
    assert!(!doc.changed_on_disk().unwrap());
    let outcome = doc.save(0, false).unwrap();
    assert_eq!(outcome.strategy, SaveStrategy::CreateNew);
    assert_eq!(fs::read(&path).unwrap(), b"x");
    assert!(!doc.is_dirty());
}

#[test]
fn file_created_while_document_expects_none_is_never_overwritten() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "new.txt", b"mine");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    fs::remove_file(&path).unwrap();
    doc.adopt_disk_state(DiskState::missing());
    let result = doc.save_with_hook(0, false, &mut |phase| {
        if phase == SavePhase::BeforeReplace {
            fs::write(&path, b"theirs").unwrap();
        }
    });
    assert_eq!(conflict_kind(result), ConflictKind::Created);
    assert_eq!(fs::read(&path).unwrap(), b"theirs");
    assert_eq!(names(dir.path()), vec!["new.txt".to_string()]);
}

#[test]
fn change_after_validation_is_caught_by_the_final_check() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "f.rs", b"fn a() {}\n");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 0, "// mine\n");
    let result = doc.save_with_hook(1, false, &mut |phase| {
        if phase == SavePhase::Validated {
            fs::write(&path, b"fn a() {} // theirs, longer\n").unwrap();
        }
    });
    assert_eq!(conflict_kind(result), ConflictKind::ModifiedDuringSave);
    assert_eq!(
        fs::read(&path).unwrap(),
        b"fn a() {} // theirs, longer\n".to_vec()
    );
    assert_eq!(doc.text().buffer().text(), "// mine\nfn a() {}\n");
    assert_eq!(
        names(dir.path()),
        vec!["f.rs".to_string()],
        "no temp files left"
    );
}

/// The hardest case: the other writer changes the file after every check, immediately
/// before the replacement, keeping size (and possibly timestamps) equal. Only the swap +
/// verification closes this window.
#[cfg(any(target_os = "linux", target_vendor = "apple"))]
#[test]
fn change_right_before_replacement_is_swapped_back() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "f.rs", b"0000");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 4, "-mine");
    // Precondition: this file system supports the atomic swap.
    let probe = write(dir.path(), "probe.txt", b"p");
    let mut probe_doc = Document::open(&probe, &Limits::default()).unwrap();
    edit(&mut probe_doc, 1, "q");
    assert_eq!(
        probe_doc.save(1, false).unwrap().strategy,
        SaveStrategy::Exchange
    );
    fs::remove_file(&probe).unwrap();

    // In place, same length.
    let result = doc.save_with_hook(1, false, &mut |phase| {
        if phase == SavePhase::BeforeReplace {
            fs::write(&path, b"1111").unwrap();
        }
    });
    assert_eq!(conflict_kind(result), ConflictKind::ModifiedDuringSave);
    assert_eq!(fs::read(&path).unwrap(), b"1111");
    assert_eq!(names(dir.path()), vec!["f.rs".to_string()]);

    // Replaced by another editor's atomic save (new inode).
    doc.adopt_disk_state(read_disk(&path, &Limits::default()).unwrap().state);
    let result = doc.save_with_hook(1, false, &mut |phase| {
        if phase == SavePhase::BeforeReplace {
            let other = dir.path().join("other-editor.tmp");
            fs::write(&other, b"2222").unwrap();
            fs::rename(&other, &path).unwrap();
        }
    });
    assert_eq!(conflict_kind(result), ConflictKind::ModifiedDuringSave);
    assert_eq!(fs::read(&path).unwrap(), b"2222");
    assert_eq!(names(dir.path()), vec!["f.rs".to_string()]);
    assert_eq!(doc.text().buffer().text(), "0000-mine");

    // Without interference the same save goes through by swap.
    doc.adopt_disk_state(read_disk(&path, &Limits::default()).unwrap().state);
    let outcome = doc.save(1, false).unwrap();
    assert_eq!(outcome.strategy, SaveStrategy::Exchange);
    assert_eq!(fs::read(&path).unwrap(), b"0000-mine");
    assert_eq!(names(dir.path()), vec!["f.rs".to_string()]);
}

#[test]
fn reload_from_disk_is_one_undoable_transaction() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "r.txt", b"old\n");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    fs::write(&path, b"\xEF\xBB\xBFnew\r\n").unwrap();
    let snapshot = read_disk(&path, &Limits::default()).unwrap();
    assert!(!doc.disk_matches(&snapshot.state));
    let change = doc.reload_from_disk(0, &snapshot).unwrap().unwrap();
    assert_eq!(change.revision, 1);
    assert_eq!(doc.text().buffer().text(), "new\r\n");
    assert!(doc.has_bom());
    assert_eq!(doc.line_ending(), LineEnding::Crlf);
    assert!(!doc.is_dirty());
    assert!(doc.disk_matches(&snapshot.state));
    doc.text_mut().undo(1).unwrap();
    assert_eq!(doc.text().buffer().text(), "old\n");
    assert!(
        doc.is_dirty(),
        "the undone state no longer matches the disk"
    );
    // Stale revisions are refused.
    assert!(matches!(
        doc.reload_from_disk(0, &snapshot),
        Err(DocError::Text(TextError::StaleRevision { .. }))
    ));
}

#[test]
fn adopting_a_disk_version_allows_exactly_that_overwrite() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "a.txt", b"base");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 4, "+mine");
    fs::write(&path, b"theirs v1").unwrap();
    let seen = read_disk(&path, &Limits::default()).unwrap();
    doc.adopt_disk_state(seen.state);
    assert!(doc.is_dirty());
    // A newer version than the one the user looked at is still protected.
    fs::write(&path, b"theirs v2").unwrap();
    assert_eq!(conflict_kind(doc.save(1, false)), ConflictKind::Modified);
    assert_eq!(fs::read(&path).unwrap(), b"theirs v2");
    // Adopting v2 lets the save replace it.
    doc.adopt_disk_state(read_disk(&path, &Limits::default()).unwrap().state);
    doc.save(1, false).unwrap();
    assert_eq!(fs::read(&path).unwrap(), b"base+mine");
}

#[test]
fn read_disk_reports_missing_files() {
    let dir = tempfile::tempdir().unwrap();
    let snapshot = read_disk(&dir.path().join("nope"), &Limits::default()).unwrap();
    assert!(!snapshot.state.exists);
    assert!(snapshot.state.same_content(&DiskState::missing()));
}

#[test]
fn stale_save_is_rejected() {
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "s.txt", b"x");
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    assert!(matches!(
        doc.save(7, false),
        Err(DocError::Text(TextError::StaleRevision { .. }))
    ));
}

/// A second writer replaces the file continuously while this document saves repeatedly.
/// Every save either succeeds or reports a conflict with the buffer intact; no temporary
/// file is ever left behind; the final file is one of the versions that was written.
#[test]
fn concurrent_writer_stress() {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "hot.txt", b"start");
    let stop = Arc::new(AtomicBool::new(false));
    let writer = {
        let stop = Arc::clone(&stop);
        let path = path.clone();
        let dir = dir.path().to_path_buf();
        std::thread::spawn(move || {
            let mut n = 0u32;
            while !stop.load(Ordering::Relaxed) {
                let tmp = dir.join(format!("w{n}.part"));
                fs::write(&tmp, format!("external {n}")).unwrap();
                fs::rename(&tmp, &path).unwrap();
                n += 1;
                std::thread::yield_now();
            }
            n
        })
    };
    let (mut ok, mut conflicts) = (0, 0);
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    for i in 0..300 {
        if let Ok(snapshot) = read_disk(&path, &Limits::default()) {
            if snapshot.state.exists {
                let rev = doc.text().revision();
                let _ = doc.reload_from_disk(rev, &snapshot);
            }
        }
        let rev = doc.text().revision();
        let len = doc.text().buffer().len_bytes();
        doc.text_mut()
            .apply(
                rev,
                vec![TextEdit::insert(ByteOffset(len), format!(" +{i}"))],
            )
            .unwrap();
        let before = doc.text().buffer().text();
        match doc.save(rev + 1, false) {
            Ok(_) => ok += 1,
            Err(DocError::ExternalModification { preserved_copy, .. }) => {
                conflicts += 1;
                assert_eq!(doc.text().buffer().text(), before);
                if let Some(copy) = preserved_copy {
                    fs::remove_file(copy).unwrap();
                }
            }
            Err(e) => panic!("unexpected error: {e}"),
        }
    }
    stop.store(true, Ordering::Relaxed);
    let writes = writer.join().unwrap();
    assert!(writes > 0);
    assert_eq!(ok + conflicts, 300);
    let leftovers: Vec<String> = names(dir.path())
        .into_iter()
        .filter(|n| n.starts_with(".titan-save-") || n.contains("titan-conflict"))
        .collect();
    assert!(leftovers.is_empty(), "left behind: {leftovers:?}");
    let last = fs::read_to_string(&path).unwrap();
    assert!(
        last.starts_with("external ") || last.contains(" +"),
        "{last}"
    );
    eprintln!("stress: {ok} saved, {conflicts} conflicts, {writes} external writes");
}

#[cfg(unix)]
#[test]
fn save_preserves_permissions_and_leaves_no_temp_files() {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let path = write(dir.path(), "run.sh", b"echo hi\n");
    fs::set_permissions(&path, fs::Permissions::from_mode(0o750)).unwrap();
    let mut doc = Document::open(&path, &Limits::default()).unwrap();
    edit(&mut doc, 0, "#!/bin/sh\n");
    doc.save(1, false).unwrap();
    let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode, 0o750);
    assert_eq!(names(dir.path()), vec!["run.sh".to_string()]);
}

// ----- adversarial tests with real external processes (Unix shells) -----------------------

#[cfg(unix)]
mod external_processes {
    use super::*;
    use std::io::{BufRead, BufReader, Write as _};
    use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

    /// A shell that runs `script` with `$1` = path, prints `ready`, then waits for a line on
    /// stdin before continuing.
    struct Shell {
        child: Child,
        stdin: ChildStdin,
        stdout: BufReader<ChildStdout>,
    }

    impl Shell {
        fn start(script: &str, path: &Path) -> Self {
            let mut child = Command::new("sh")
                .arg("-c")
                .arg(script)
                .arg("sh")
                .arg(path)
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .expect("sh");
            let stdin = child.stdin.take().unwrap();
            let stdout = BufReader::new(child.stdout.take().unwrap());
            let mut shell = Self {
                child,
                stdin,
                stdout,
            };
            assert_eq!(shell.line(), "ready");
            shell
        }

        fn line(&mut self) -> String {
            let mut line = String::new();
            self.stdout.read_line(&mut line).unwrap();
            line.trim_end().to_string()
        }

        fn go(&mut self) -> String {
            writeln!(self.stdin, "go").unwrap();
            self.stdin.flush().unwrap();
            self.line()
        }
    }

    impl Drop for Shell {
        fn drop(&mut self) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }

    /// The open-descriptor gap: a program opened the file before our save and appends
    /// afterwards. Its bytes go to the displaced inode; we detect them and can recover them.
    #[test]
    fn late_append_through_an_old_descriptor_is_detected_and_recoverable() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(dir.path(), "log.txt", b"v1\n");
        let mut shell = Shell::start(
            r#"exec 3>>"$1"; echo ready; read x; printf 'LATE\n' >&3; echo done"#,
            &path,
        );
        let mut doc = Document::open(&path, &Limits::default()).unwrap();
        edit(&mut doc, 3, "mine\n");
        doc.save(1, false).unwrap();
        let mut displaced = doc
            .take_displaced()
            .expect("the replaced version is kept open");
        assert!(!displaced.touched().unwrap(), "nothing written yet");
        assert_eq!(displaced.late_write(u64::MAX).unwrap(), None);

        assert_eq!(shell.go(), "done");
        assert!(displaced.touched().unwrap(), "fstat sees the late append");
        let late = displaced.late_write(u64::MAX).unwrap().expect("late bytes");
        assert_eq!(late, b"v1\nLATE\n");
        assert_eq!(
            displaced.late_write(u64::MAX).unwrap(),
            None,
            "reported once"
        );
        assert!(!displaced.touched().unwrap());
        // The saved file is untouched by the late writer.
        assert_eq!(fs::read(&path).unwrap(), b"v1\nmine\n");
        assert_eq!(
            names(dir.path()),
            vec!["log.txt".to_string()],
            "nothing visible left"
        );
    }

    /// In-place rewrite through an old read-write descriptor (same length): only the content
    /// check can tell.
    #[test]
    fn late_overwrite_of_the_same_length_is_detected_by_content() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(dir.path(), "c.txt", b"aaaa");
        let mut shell = Shell::start(
            r#"exec 3<>"$1"; echo ready; read x; printf 'bbbb' >&3; echo done"#,
            &path,
        );
        let mut doc = Document::open(&path, &Limits::default()).unwrap();
        edit(&mut doc, 4, "+");
        doc.save(1, false).unwrap();
        let mut displaced = doc.take_displaced().unwrap();
        assert_eq!(shell.go(), "done");
        assert_eq!(
            displaced.late_write(u64::MAX).unwrap().as_deref(),
            Some(&b"bbbb"[..])
        );
        assert_eq!(fs::read(&path).unwrap(), b"aaaa+");
    }

    /// External truncation and append by path: before a save they are conflicts; after a save
    /// they modify the new file, which `changed_on_disk` reports.
    #[test]
    fn truncation_and_append_by_path() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(dir.path(), "t.txt", b"0123456789");
        let mut doc = Document::open(&path, &Limits::default()).unwrap();
        edit(&mut doc, 0, ">");
        let status = Command::new("sh")
            .arg("-c")
            .arg(r#": > "$1""#)
            .arg("sh")
            .arg(&path)
            .status()
            .unwrap();
        assert!(status.success());
        assert_eq!(conflict_kind(doc.save(1, false)), ConflictKind::Modified);
        assert_eq!(fs::read(&path).unwrap(), b"");
        doc.adopt_disk_state(read_disk(&path, &Limits::default()).unwrap().state);
        doc.save(1, false).unwrap();
        let status = Command::new("sh")
            .arg("-c")
            .arg(r#"printf 'tail' >> "$1""#)
            .arg("sh")
            .arg(&path)
            .status()
            .unwrap();
        assert!(status.success());
        assert!(doc.changed_on_disk().unwrap());
        assert_eq!(fs::read(&path).unwrap(), b">0123456789tail");
    }

    /// The directory is renamed away while a save runs: the save fails as a conflict, the
    /// buffer is kept, nothing is written at the old or new location.
    #[test]
    fn directory_renamed_during_save() {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir(dir.path().join("pkg")).unwrap();
        let path = write(&dir.path().join("pkg"), "m.rs", b"fn m() {}\n");
        let mut doc = Document::open(&path, &Limits::default()).unwrap();
        edit(&mut doc, 0, "// x\n");
        let moved = dir.path().join("moved");
        let result = doc.save_with_hook(1, false, &mut |phase| {
            if phase == SavePhase::Validated {
                fs::rename(dir.path().join("pkg"), &moved).unwrap();
            }
        });
        assert!(result.is_err(), "{result:?}");
        assert_eq!(doc.text().buffer().text(), "// x\nfn m() {}\n");
        assert_eq!(fs::read(moved.join("m.rs")).unwrap(), b"fn m() {}\n");
        let leftovers: Vec<String> = names(&moved)
            .into_iter()
            .filter(|n| n.starts_with(".titan-save-"))
            .collect();
        assert!(leftovers.len() <= 1, "{leftovers:?}");
    }

    /// Three external processes replace the file atomically in a loop (like other editors)
    /// while this document reloads, edits and saves. Every save succeeds or is refused as a
    /// conflict; no temporary files remain; the final file is a complete version.
    #[test]
    fn concurrent_external_writer_processes() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(dir.path(), "hot.rs", b"start\n");
        let writers: Vec<Child> = (0..3)
            .map(|w| {
                Command::new("sh")
                    .arg("-c")
                    .arg(format!(
                        r#"i=0; while [ $i -lt 300 ]; do printf 'writer {w} %s\n' $i > "$1.w{w}"; mv "$1.w{w}" "$1"; i=$((i+1)); done"#
                    ))
                    .arg("sh")
                    .arg(&path)
                    .spawn()
                    .unwrap()
            })
            .collect();
        let mut doc = Document::open(&path, &Limits::default()).unwrap();
        let (mut ok, mut conflicts) = (0, 0);
        for i in 0..200 {
            if let Ok(snapshot) = read_disk(&path, &Limits::default()) {
                if snapshot.state.exists {
                    let rev = doc.text().revision();
                    let _ = doc.reload_from_disk(rev, &snapshot);
                }
            }
            let rev = doc.text().revision();
            let len = doc.text().buffer().len_bytes();
            doc.text_mut()
                .apply(
                    rev,
                    vec![TextEdit::insert(ByteOffset(len), format!("ours {i}\n"))],
                )
                .unwrap();
            let before = doc.text().buffer().text();
            match doc.save(rev + 1, false) {
                Ok(_) => ok += 1,
                Err(DocError::ExternalModification { preserved_copy, .. }) => {
                    conflicts += 1;
                    assert_eq!(doc.text().buffer().text(), before);
                    if let Some(copy) = preserved_copy {
                        let _ = fs::remove_file(copy);
                    }
                }
                Err(e) => panic!("unexpected error: {e}"),
            }
        }
        for mut w in writers {
            assert!(w.wait().unwrap().success());
        }
        assert_eq!(ok + conflicts, 200);
        let leftovers: Vec<String> = names(dir.path())
            .into_iter()
            .filter(|n| n.starts_with(".titan-save-") || n.contains("titan-conflict"))
            .collect();
        assert!(leftovers.is_empty(), "{leftovers:?}");
        let last = fs::read_to_string(&path).unwrap();
        assert!(last.ends_with('\n'), "a complete version: {last:?}");
        eprintln!("external writers: {ok} saves, {conflicts} conflicts");
    }
}
