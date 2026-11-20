//! Real-filesystem tests for `titan-watch` (inotify on Linux, FSEvents on macOS).
//!
//! Assertions that depend on backend precision are strict on Linux and tolerant (but still
//! meaningful) elsewhere; every tolerant branch records what it observed via `observe`.

mod common;

use std::fs;
use std::time::{Duration, Instant};

use common::*;
use titan_watch::{ChangeKind, WatchEvent};

const LINUX: bool = cfg!(target_os = "linux");

#[test]
fn create_modify_delete_file() {
    let h = start(|_, _| {}, |c| c);
    let a = h.path("a.txt");

    write(&a, b"one");
    let events = h.wait_for("a.txt created", |c| c.path == a);
    let created = changes_for(&events, &a);
    observe("create", &created);
    assert_eq!(
        created.len(),
        1,
        "one change per path per batch: {created:?}"
    );
    assert_eq!(created[0].is_dir, Some(false));
    if LINUX {
        assert_eq!(created[0].kind, ChangeKind::Created);
    } else {
        assert!(is_created_or_modified(&created[0]), "{created:?}");
    }

    append(&a, b" two");
    let events = h.wait_for("a.txt modified", |c| c.path == a);
    let modified = changes_for(&events, &a);
    observe("modify", &modified);
    if LINUX {
        assert_eq!(modified[0].kind, ChangeKind::Modified);
    } else {
        // FSEvents flags are cumulative: a recently created file may be reported as Created.
        assert!(is_created_or_modified(&modified[0]), "{modified:?}");
    }

    fs::remove_file(&a).unwrap();
    let events = h.wait_for("a.txt removed", |c| {
        c.path == a && c.kind == ChangeKind::Removed
    });
    observe("delete", changes_for(&events, &a));
}

#[test]
fn rename_file_within_workspace() {
    let h = start(
        |root, _| write(&root.join("old.rs"), b"fn main() {}"),
        |c| c,
    );
    let old = h.path("old.rs");
    let new = h.path("new.rs");

    fs::rename(&old, &new).unwrap();
    // Wait until both sides are accounted for: either a paired rename, or the fallback
    // Removed(old) + Created(new), possibly split over batches.
    let mut events = h.wait_for("new.rs reported", |c| c.path == new);
    let accounted = |events: &[WatchEvent]| {
        all_changes(events).iter().any(|c| {
            (c.path == new && c.kind == ChangeKind::Renamed { from: old.clone() })
                || (c.path == old && c.kind == ChangeKind::Removed)
        })
    };
    if !accounted(&events) {
        events.extend(h.wait_for("old.rs accounted for", |c| {
            c.path == old && c.kind == ChangeKind::Removed
        }));
    }
    events.extend(h.drain_quiet(Duration::from_millis(300)));
    let changes = all_changes(&events);
    observe("rename", &changes);

    let paired = changes
        .iter()
        .any(|c| c.path == new && c.kind == ChangeKind::Renamed { from: old.clone() });
    if LINUX {
        assert!(paired, "inotify pairs renames by cookie: {changes:?}");
        assert!(
            !changes.iter().any(|c| c.path == old),
            "the source is covered by Renamed: {changes:?}"
        );
    } else if !paired {
        assert!(
            changes
                .iter()
                .any(|c| c.path == new && is_created_or_modified(c))
        );
        assert!(
            changes
                .iter()
                .any(|c| c.path == old && c.kind == ChangeKind::Removed)
        );
    }
    let target = changes.iter().find(|c| c.path == new).unwrap();
    assert_eq!(target.is_dir, Some(false));
}

#[test]
fn atomic_replacement_reports_target_not_temp() {
    let h = start(|root, _| write(&root.join("a.rs"), b"old"), |c| c);
    let target = h.path("a.rs");
    let temp = h.path(".titan-save-Xy12Ab");
    let ignored_before = h.watcher().stats().ignored;

    write(&temp, b"new contents");
    fs::rename(&temp, &target).unwrap();

    let mut events = h.wait_for("a.rs replaced", |c| c.path == target);
    events.extend(h.drain_quiet(Duration::from_millis(500)));
    let changes = all_changes(&events);
    observe("atomic-save", &changes);

    let for_target = changes_for(&events, &target);
    assert!(
        for_target.iter().all(is_created_or_modified),
        "the target exists, so it is Created or Modified: {for_target:?}"
    );
    assert!(
        mentioned_paths(&events)
            .iter()
            .all(|p| !p.to_string_lossy().contains(titan_watch::TITAN_SAVE_PREFIX)),
        "temp file leaked: {changes:?}"
    );
    assert!(h.watcher().stats().ignored > ignored_before);
    assert_eq!(fs::read(&target).unwrap(), b"new contents");
}

#[test]
fn directory_create_rename_and_recursive_delete() {
    let h = start(|_, _| {}, |c| c);
    let d = h.path("dir");
    let e = h.path("renamed");

    fs::create_dir(&d).unwrap();
    let events = h.wait_for("dir created", |c| c.path == d);
    let created = changes_for(&events, &d);
    observe("mkdir", &created);
    assert_eq!(created[0].is_dir, Some(true));
    if LINUX {
        assert_eq!(created[0].kind, ChangeKind::Created);
    }

    // The new directory is watched (it was reported, so the backend has processed it).
    write(&d.join("x.txt"), b"x");
    h.wait_for("dir/x.txt", |c| c.path == d.join("x.txt"));

    fs::rename(&d, &e).unwrap();
    let mut events = h.wait_for("renamed dir", |c| c.path == e);
    events.extend(h.drain_quiet(Duration::from_millis(300)));
    let changes = all_changes(&events);
    observe("rename-dir", &changes);
    let target = changes.iter().find(|c| c.path == e).unwrap();
    assert_eq!(target.is_dir, Some(true));
    if LINUX {
        assert_eq!(target.kind, ChangeKind::Renamed { from: d.clone() });
    }

    // The subtree is still watched under its new name.
    write(&e.join("y.txt"), b"y");
    h.wait_for("renamed/y.txt", |c| c.path == e.join("y.txt"));

    fs::create_dir(e.join("nested")).unwrap();
    h.wait_for("renamed/nested", |c| c.path == e.join("nested"));
    write(&e.join("nested/z.txt"), b"z");
    h.wait_for("renamed/nested/z.txt", |c| c.path == e.join("nested/z.txt"));

    fs::remove_dir_all(&e).unwrap();
    let mut events = h.wait_for("renamed dir removed", |c| {
        c.path == e && c.kind == ChangeKind::Removed
    });
    events.extend(h.drain_quiet(Duration::from_millis(300)));
    let changes = all_changes(&events);
    observe("remove-dir-all", &changes);
    let removed = changes
        .iter()
        .find(|c| c.path == e && c.kind == ChangeKind::Removed)
        .unwrap();
    if LINUX {
        assert_eq!(removed.is_dir, Some(true));
    }
    assert!(
        changes
            .iter()
            .filter(|c| c.path.starts_with(&e))
            .all(|c| c.kind == ChangeKind::Removed),
        "{changes:?}"
    );
}

#[test]
fn burst_of_writes_is_coalesced() {
    let h = start(|root, _| write(&root.join("hot.txt"), b""), |c| c);
    let hot = h.path("hot.txt");
    let before = h.watcher().stats();

    for i in 0..500 {
        append(&hot, format!("line {i}\n").as_bytes());
    }
    let mut events = h.wait_for("hot.txt", |c| c.path == hot);
    events.extend(h.drain_quiet(Duration::from_millis(500)));
    let after = h.watcher().stats();

    let reported = changes_for(&events, &hot);
    let batches_with_hot = events
        .iter()
        .filter(|e| matches!(e, WatchEvent::Changes { changes, .. } if changes.iter().any(|c| c.path == hot)))
        .count();
    let raw = after.raw_events - before.raw_events;
    let coalesced = after.coalesced - before.coalesced;
    observe(
        "burst",
        (
            ("reported", reported.len()),
            ("batches_with_path", batches_with_hot),
            ("raw_events", raw),
            ("coalesced", coalesced),
        ),
    );
    assert_eq!(rescans(&events), 0);
    assert!(
        (1..=5).contains(&reported.len()),
        "500 writes must collapse into a handful of changes, got {}: {reported:?}",
        reported.len()
    );
    assert_eq!(
        reported.len(),
        batches_with_hot,
        "at most one change per path per batch"
    );
    assert!(
        coalesced > 0 && raw > reported.len() as u64,
        "coalescing must have merged raw events (raw {raw}, coalesced {coalesced})"
    );
}

#[test]
fn too_many_paths_in_one_window_yield_one_rescan() {
    // A long debounce keeps the whole burst in one window.
    let mut h = start(
        |_, _| {},
        |mut c| {
            c.max_pending = 10;
            c.debounce = Duration::from_millis(1000);
            c
        },
    );
    let before = h.watcher().stats();
    for i in 0..200 {
        write(&h.path(&format!("many-{i:03}.txt")), b"x");
    }
    let started = Instant::now();
    let mut events = Vec::new();
    while rescans(&events) == 0 && started.elapsed() < DEADLINE {
        events.extend(collect_quiet(&h.rx, Duration::from_millis(200)));
    }
    events.extend(h.drain_quiet(Duration::from_millis(2500)));
    let after = h.watcher().stats();
    observe(
        "overflow",
        (
            ("rescans", rescans(&events)),
            ("changes", all_changes(&events).len()),
        ),
    );
    assert_eq!(
        rescans(&events),
        1,
        "exactly one Rescan for the window: {events:#?}"
    );
    assert_eq!(after.overflows - before.overflows, 1);
    assert!(
        all_changes(&events).len() <= 10,
        "buffered detail is dropped on overflow"
    );
    if let Some(WatchEvent::Rescan { reason, .. }) = events
        .iter()
        .find(|e| matches!(e, WatchEvent::Rescan { .. }))
    {
        assert!(reason.contains("10"), "{reason}");
    }

    // The watcher keeps working afterwards.
    let marker = h.path("after-overflow.txt");
    write(&marker, b"ok");
    h.wait_for("marker after overflow", |c| c.path == marker);
    h.stop();
}

#[test]
fn ignore_filter_and_builtin_temp_filter() {
    let h = start(
        |root, _| fs::create_dir_all(root.join("target/debug")).unwrap(),
        |c| c.with_ignore(|rel| rel.starts_with("target")),
    );
    let noisy = [
        "target/debug/app.d",
        "target/new.rlib",
        ".main.rs.swp",
        ".main.rs.swx",
        "main.rs~",
        "4913",
        ".DS_Store",
        ".#main.rs",
        "build.tmp",
        "file.txt.crswap",
        ".titan-save-abcdef",
    ];
    for rel in noisy {
        write(&h.path(rel), b"noise");
    }
    let kept = h.path("kept.rs");
    write(&kept, b"fn kept() {}");
    let mut events = h.wait_for("kept.rs", |c| c.path == kept);
    events.extend(h.drain_quiet(Duration::from_millis(500)));
    let mentioned = mentioned_paths(&events);
    observe("ignore", &mentioned);
    for rel in noisy {
        assert!(
            !mentioned.contains(&h.path(rel)),
            "{rel} must be filtered: {mentioned:?}"
        );
    }
    assert!(!mentioned.iter().any(|p| p.starts_with(h.path("target"))));
    assert!(h.watcher().stats().ignored >= noisy.len() as u64);
}

#[test]
fn events_outside_root_are_never_reported() {
    let h = start(
        |_, outside| fs::create_dir_all(outside.join("deep")).unwrap(),
        |c| c,
    );
    for i in 0..20 {
        write(&h.outside.join(format!("x{i}.txt")), b"outside");
        write(&h.outside.join(format!("deep/y{i}.txt")), b"outside");
    }
    let marker = h.path("marker.txt");
    write(&marker, b"inside");
    let mut events = h.wait_for("marker", |c| c.path == marker);
    events.extend(h.drain_quiet(Duration::from_millis(500)));
    let mentioned = mentioned_paths(&events);
    observe("outside-root", &mentioned);
    assert!(
        mentioned.iter().all(|p| p.starts_with(&h.root)),
        "{mentioned:?}"
    );
}

#[cfg(unix)]
#[test]
fn symlink_to_outside_is_not_followed() {
    let h = start(
        |root, outside| {
            write(&outside.join("secret.txt"), b"s");
            std::os::unix::fs::symlink(outside, root.join("link")).unwrap();
        },
        |c| c,
    );
    for i in 0..10 {
        append(&h.outside.join("secret.txt"), b"more");
        // Writing through the link also lands outside the root.
        write(&h.path(&format!("link/via-link-{i}.txt")), b"via link");
    }
    let marker = h.path("marker.txt");
    write(&marker, b"inside");
    let mut events = h.wait_for("marker", |c| c.path == marker);
    events.extend(h.drain_quiet(Duration::from_millis(500)));
    let mentioned = mentioned_paths(&events);
    observe("symlink-out", &mentioned);
    assert!(
        mentioned.iter().all(|p| p.starts_with(&h.root)),
        "{mentioned:?}"
    );
    assert!(
        !mentioned.iter().any(|p| p.starts_with(h.path("link"))),
        "nothing behind the symlink is reported: {mentioned:?}"
    );
}

#[test]
fn stop_is_prompt_and_silences_callbacks() {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

    let tmp = tempfile::tempdir().unwrap();
    let root = fs::canonicalize(tmp.path()).unwrap();
    let stopped = Arc::new(AtomicBool::new(false));
    let late = Arc::new(AtomicUsize::new(0));
    let delivered = Arc::new(AtomicUsize::new(0));
    let (s, l, d) = (stopped.clone(), late.clone(), delivered.clone());
    let watcher = titan_watch::Watcher::start(titan_watch::WatchConfig::new(&root), move |_| {
        if s.load(Ordering::SeqCst) {
            l.fetch_add(1, Ordering::SeqCst);
        }
        d.fetch_add(1, Ordering::SeqCst);
    })
    .unwrap();

    // Keep the watcher busy until something has been delivered.
    let deadline = Instant::now() + DEADLINE;
    let mut i = 0;
    while delivered.load(Ordering::SeqCst) == 0 {
        assert!(Instant::now() < deadline, "nothing delivered");
        write(&root.join(format!("busy-{i}.txt")), b"x");
        i += 1;
        std::thread::sleep(Duration::from_millis(5));
    }
    for j in 0..50 {
        write(&root.join(format!("pending-{j}.txt")), b"x");
    }

    let begun = Instant::now();
    watcher.stop();
    let took = begun.elapsed();
    stopped.store(true, Ordering::SeqCst);
    observe("stop", took);
    assert!(took < Duration::from_secs(2), "stop took {took:?}");

    for j in 0..50 {
        write(&root.join(format!("after-{j}.txt")), b"x");
    }
    std::thread::sleep(Duration::from_millis(500));
    assert_eq!(
        late.load(Ordering::SeqCst),
        0,
        "callback ran after stop returned"
    );
}
