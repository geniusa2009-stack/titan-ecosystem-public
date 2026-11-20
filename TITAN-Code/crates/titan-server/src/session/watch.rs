//! File watching: turn `titan-watch` events into reconciliation work.
//!
//! * Open documents whose file changed, vanished or was renamed away get a background disk
//!   check ([`Session::schedule_disk_check`]); the reconciliation rules live in `disk.rs`.
//!   Dirty buffers are never reloaded.
//! * Rust files that are not open are re-indexed from disk (or dropped from the index).
//! * Directory events and lost events (`Rescan`) start an incremental workspace scan and a
//!   disk check of every open document, because individual changes under them may be
//!   missing.
//! * The project graph is marked stale; the language server gets
//!   `workspace/didChangeWatchedFiles` for files it does not have open.
//! * The client gets `workspace/filesChanged` (if it asked for it). Search reads the disk
//!   at request time, so there is no search cache to invalidate; the notification tells
//!   clients to re-run searches they display.

use std::path::{Path, PathBuf};

use titan_protocol::messages::{FileChange, FilesChangedParams, notifications};
use titan_tasks::Priority;
use titan_watch::{ChangeKind, WatchConfig, WatchEvent, Watcher};

use super::{Pending, Session, TaskResult, index_text, is_rust_file, read_for_index};
use crate::Event;

/// Directories never watched for content (build output, VCS metadata, dependencies).
const IGNORED_DIRS: &[&str] = &[".git", "target", "node_modules", ".hg", ".svn"];

/// LSP `FileChangeType`.
const LSP_CREATED: u8 = 1;
const LSP_CHANGED: u8 = 2;
const LSP_DELETED: u8 = 3;

fn ignored(rel: &Path) -> bool {
    rel.components()
        .next()
        .is_some_and(|c| IGNORED_DIRS.iter().any(|d| c.as_os_str() == *d))
}

fn is_manifest(path: &Path) -> bool {
    path.file_name().is_some_and(|n| n == "Cargo.toml")
}

impl Session {
    pub(super) fn start_watcher(&mut self, root: &Path) {
        self.stop_watcher();
        if !self.options.watch {
            return;
        }
        let events = self.events.clone();
        let config = WatchConfig::new(root).with_ignore(ignored);
        match Watcher::start(config, move |event| {
            let _ = events.send(Event::Fs(event));
        }) {
            Ok(watcher) => {
                self.watch.watcher = Some(watcher);
                self.watch.error = None;
            }
            Err(e) => {
                eprintln!("titand: file watching is not available: {e}");
                self.watch.error = Some(e.to_string());
            }
        }
    }

    pub(super) fn stop_watcher(&mut self) {
        if let Some(watcher) = self.watch.watcher.take() {
            watcher.stop();
        }
    }

    pub(crate) fn on_fs_event(&mut self, event: WatchEvent) {
        match event {
            WatchEvent::Changes { seq, changes } => self.on_fs_changes(seq, changes),
            WatchEvent::Rescan { seq, reason } => {
                eprintln!("titand: file events were lost ({reason}); rescanning");
                self.watch.rescans += 1;
                self.full_reconcile("rescan");
                if self.caps.file_events {
                    self.push_notification(
                        notifications::WORKSPACE_FILES_CHANGED,
                        FilesChangedParams {
                            seq,
                            rescan: true,
                            changes: Vec::new(),
                        },
                    );
                }
            }
            WatchEvent::Error { message } => {
                eprintln!("titand: file watcher error: {message}");
                self.watch.errors += 1;
                self.watch.error = Some(message);
            }
        }
    }

    /// Re-check everything: incremental index scan, every open document, the graph.
    fn full_reconcile(&mut self, cause: &'static str) {
        self.start_scan();
        let ids: Vec<u64> = self.documents.keys().copied().collect();
        for id in ids {
            self.schedule_disk_check(id, cause);
        }
        self.mark_graph_stale();
    }

    fn on_fs_changes(&mut self, seq: u64, changes: Vec<titan_watch::FsChange>) {
        self.watch.batches += 1;
        self.watch.changes += changes.len() as u64;
        let mut wire = Vec::with_capacity(changes.len());
        let mut lsp: Vec<(PathBuf, u8)> = Vec::new();
        let mut rescan = false;
        let mut graph_stale = false;
        for change in changes {
            let path = change.path.clone();
            let (kind, from) = match &change.kind {
                ChangeKind::Created => ("created", None),
                ChangeKind::Modified => ("modified", None),
                ChangeKind::Removed => ("removed", None),
                ChangeKind::Renamed { from } => ("renamed", Some(from.clone())),
            };
            if let Some(from) = &from {
                self.graph_file_renamed(from, &path);
            }
            let is_dir = change.is_dir == Some(true)
                || (change.is_dir.is_none()
                    && matches!(change.kind, ChangeKind::Removed)
                    && self
                        .by_path
                        .keys()
                        .any(|p| p.starts_with(&path) && p != &path));
            if is_dir {
                // Children may have changed without individual events (rename, delete,
                // files created before the new directory was watched).
                rescan = true;
                let affected: Vec<u64> = self
                    .by_path
                    .iter()
                    .filter(|(p, _)| {
                        p.starts_with(&path) || from.as_ref().is_some_and(|f| p.starts_with(f))
                    })
                    .map(|(_, id)| *id)
                    .collect();
                for id in affected {
                    self.schedule_disk_check(id, "watcher");
                }
            } else {
                for p in std::iter::once(&path).chain(from.as_ref()) {
                    if let Some(&id) = self.by_path.get(p) {
                        self.schedule_disk_check(id, "watcher");
                    } else if is_rust_file(p) {
                        self.reindex_from_disk(p.clone());
                    }
                }
                if is_manifest(&path) {
                    rescan = true;
                }
            }
            if is_rust_file(&path) || is_manifest(&path) || is_dir {
                graph_stale = true;
            }
            match &change.kind {
                ChangeKind::Created => lsp.push((path.clone(), LSP_CREATED)),
                ChangeKind::Modified => lsp.push((path.clone(), LSP_CHANGED)),
                ChangeKind::Removed => lsp.push((path.clone(), LSP_DELETED)),
                ChangeKind::Renamed { from } => {
                    lsp.push((from.clone(), LSP_DELETED));
                    lsp.push((path.clone(), LSP_CREATED));
                }
            }
            wire.push(FileChange {
                path: self.relative_display(&path),
                kind: kind.to_string(),
                from: from.as_deref().map(|f| self.relative_display(f)),
                is_dir: change.is_dir,
            });
        }
        if rescan {
            self.start_scan();
        }
        if graph_stale {
            self.mark_graph_stale();
        }
        self.lsp_watched_files(&lsp);
        if self.caps.file_events && !wire.is_empty() {
            self.push_notification(
                notifications::WORKSPACE_FILES_CHANGED,
                FilesChangedParams {
                    seq,
                    rescan: false,
                    changes: wire,
                },
            );
        }
    }

    /// Re-index a Rust file that is not open from its disk content (or drop it).
    pub(super) fn reindex_from_disk(&mut self, path: PathBuf) {
        if self.by_path.contains_key(&path) {
            return;
        }
        if !path.exists() {
            self.index.remove(&path);
            return;
        }
        let job_path = path.clone();
        let submitted = self
            .pool
            .submit("index.file", Priority::Low, None, move |_| {
                TaskResult::FileIndex(
                    read_for_index(&job_path)
                        .and_then(|(version, text)| index_text(version, &text)),
                )
            });
        if let Ok((task, _)) = submitted {
            self.pending.insert(task, Pending::FileIndex { path });
        }
    }
}

#[derive(Default)]
pub(super) struct WatchState {
    pub watcher: Option<Watcher>,
    pub error: Option<String>,
    pub batches: u64,
    pub changes: u64,
    pub rescans: u64,
    pub errors: u64,
}

impl WatchState {
    pub fn metrics(&self) -> serde_json::Value {
        let stats = self.watcher.as_ref().map(Watcher::stats);
        serde_json::json!({
            "active": self.watcher.is_some(),
            "error": self.error,
            "batches": self.batches,
            "changes": self.changes,
            "rescans": self.rescans,
            "errors": self.errors,
            "backend": stats.map(|s| serde_json::json!({
                "rawEvents": s.raw_events,
                "batches": s.batches,
                "changes": s.changes,
                "coalesced": s.coalesced,
                "ignored": s.ignored,
                "outsideRoot": s.outside_root,
                "overflows": s.overflows,
                "rescans": s.rescans,
                "errors": s.errors,
            })),
        })
    }
}
