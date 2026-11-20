//! Shared harness for the real-filesystem watcher tests.
//!
//! Synchronisation is always "wait until the expected change arrives" with a generous
//! deadline; quiet periods are only used to make sure nothing *else* arrives afterwards.

#![allow(dead_code)]

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};

use titan_watch::{ChangeKind, FsChange, WatchConfig, WatchEvent, Watcher};

pub const DEADLINE: Duration = Duration::from_secs(10);
const READY_DEADLINE: Duration = Duration::from_secs(20);

pub struct Harness {
    _tmp: tempfile::TempDir,
    /// Canonical workspace root (`<tmp>/ws`).
    pub root: PathBuf,
    /// Canonical sibling directory outside the root (`<tmp>/outside`).
    pub outside: PathBuf,
    pub rx: Receiver<WatchEvent>,
    pub watcher: Option<Watcher>,
}

/// Creates `<tmp>/ws` and `<tmp>/outside`, lets `prepare` populate them before the watcher
/// starts, starts the watcher with `configure`, and waits until it demonstrably delivers.
pub fn start(
    prepare: impl FnOnce(&Path, &Path),
    configure: impl FnOnce(WatchConfig) -> WatchConfig,
) -> Harness {
    let tmp = tempfile::tempdir().expect("tempdir");
    let base = fs::canonicalize(tmp.path()).expect("canonical tempdir");
    let root = base.join("ws");
    let outside = base.join("outside");
    fs::create_dir(&root).expect("create ws");
    fs::create_dir(&outside).expect("create outside");
    prepare(&root, &outside);

    let (tx, rx) = mpsc::channel();
    let config = configure(WatchConfig::new(&root));
    let watcher = Watcher::start(config, move |event| {
        let _ = tx.send(event);
    })
    .expect("watcher starts");
    assert_eq!(watcher.root(), root.as_path());

    let mut harness = Harness {
        _tmp: tmp,
        root,
        outside,
        rx,
        watcher: Some(watcher),
    };
    harness.wait_ready();
    harness
}

impl Harness {
    pub fn path(&self, rel: &str) -> PathBuf {
        self.root.join(rel)
    }

    pub fn watcher(&self) -> &Watcher {
        self.watcher.as_ref().expect("watcher running")
    }

    /// Backends may start delivering slightly after `watch` returns (FSEvents). Touch a probe
    /// file until it is seen, then let the backlog drain.
    fn wait_ready(&mut self) {
        let probe = self.root.join("ready-probe");
        let deadline = Instant::now() + READY_DEADLINE;
        let mut n = 0u32;
        loop {
            n += 1;
            write(&probe, format!("{n}").as_bytes());
            // Longer than any debounce used in the tests, so rewriting the probe cannot keep
            // a window open forever.
            let round = Instant::now() + Duration::from_millis(2500);
            while Instant::now() < round {
                match self.rx.recv_timeout(Duration::from_millis(50)) {
                    Ok(WatchEvent::Changes { changes, .. })
                        if changes.iter().any(|c| c.path == probe) =>
                    {
                        self.drain_quiet(Duration::from_millis(300));
                        return;
                    }
                    Ok(_) | Err(RecvTimeoutError::Timeout) => {}
                    Err(RecvTimeoutError::Disconnected) => panic!("watcher thread ended"),
                }
            }
            assert!(
                Instant::now() < deadline,
                "watcher never reported the probe file within {READY_DEADLINE:?}"
            );
        }
    }

    /// Discards events until none arrived for `quiet` (bounded by `DEADLINE`).
    pub fn drain_quiet(&self, quiet: Duration) -> Vec<WatchEvent> {
        collect_quiet(&self.rx, quiet)
    }

    /// Collects events until one contains a change satisfying `pred`. Panics with everything
    /// seen if that does not happen within `DEADLINE`.
    pub fn wait_for(&self, what: &str, mut pred: impl FnMut(&FsChange) -> bool) -> Vec<WatchEvent> {
        let deadline = Instant::now() + DEADLINE;
        let mut seen = Vec::new();
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self.rx.recv_timeout(left) {
                Ok(event) => {
                    let hit = matches!(&event, WatchEvent::Changes { changes, .. }
                        if changes.iter().any(&mut pred));
                    seen.push(event);
                    if hit {
                        return seen;
                    }
                }
                Err(_) => panic!("timed out after {DEADLINE:?} waiting for {what}; saw {seen:#?}"),
            }
        }
    }

    /// Stops the watcher and returns its final stats.
    pub fn stop(&mut self) -> titan_watch::WatchStats {
        let watcher = self.watcher.take().expect("watcher running");
        let stats = watcher.stats();
        watcher.stop();
        stats
    }
}

pub fn collect_quiet(rx: &Receiver<WatchEvent>, quiet: Duration) -> Vec<WatchEvent> {
    let deadline = Instant::now() + DEADLINE;
    let mut seen = Vec::new();
    while Instant::now() < deadline {
        match rx.recv_timeout(quiet) {
            Ok(event) => seen.push(event),
            Err(_) => break,
        }
    }
    seen
}

pub fn write(path: &Path, bytes: &[u8]) {
    let mut file = fs::File::create(path).expect("create file");
    file.write_all(bytes).expect("write file");
}

pub fn append(path: &Path, bytes: &[u8]) {
    let mut file = fs::OpenOptions::new()
        .append(true)
        .open(path)
        .expect("open for append");
    file.write_all(bytes).expect("append");
}

/// All changes in `events`, in delivery order.
pub fn all_changes(events: &[WatchEvent]) -> Vec<FsChange> {
    events
        .iter()
        .flat_map(|e| match e {
            WatchEvent::Changes { changes, .. } => changes.clone(),
            _ => Vec::new(),
        })
        .collect()
}

pub fn changes_for(events: &[WatchEvent], path: &Path) -> Vec<FsChange> {
    all_changes(events)
        .into_iter()
        .filter(|c| c.path == path)
        .collect()
}

pub fn rescans(events: &[WatchEvent]) -> usize {
    events
        .iter()
        .filter(|e| matches!(e, WatchEvent::Rescan { .. }))
        .count()
}

/// Every path mentioned by `events` (targets and rename sources).
pub fn mentioned_paths(events: &[WatchEvent]) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for change in all_changes(events) {
        if let ChangeKind::Renamed { from } = &change.kind {
            paths.push(from.clone());
        }
        paths.push(change.path);
    }
    paths
}

pub fn is_created_or_modified(change: &FsChange) -> bool {
    matches!(change.kind, ChangeKind::Created | ChangeKind::Modified)
}

/// Prints an observation; visible with `--nocapture` (the macOS CI job uses it).
pub fn observe(test: &str, what: impl std::fmt::Debug) {
    eprintln!(
        "[titan-watch observation] os={} test={test}: {what:?}",
        std::env::consts::OS
    );
}
