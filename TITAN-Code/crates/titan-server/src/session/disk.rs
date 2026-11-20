//! Reconciliation of open documents with the file system.
//!
//! Disk reads and hashing run on the task pool; only the comparison and the resulting state
//! change happen on the event loop. Rules:
//! * The disk holds the version the document is based on → `inSync`.
//! * The disk changed and the buffer is **clean** → the buffer is reloaded as one undoable
//!   transaction and the client is told (`action: "reloaded"`).
//! * The disk changed and the buffer is **dirty** → the buffer is kept untouched, the state
//!   becomes `modified` (or `deleted`), and the client is told (`action: "conflict"`). The
//!   next save is refused until the client resolves the conflict
//!   (`document.resolveConflict`): `keepBuffer` (the next save may replace exactly the disk
//!   version the client saw) or `reloadFromDisk`.
//! * Checks are versioned by a per-document epoch: a check started before a save, reload
//!   or resolution is discarded and repeated, so the server never mistakes its own write
//!   for someone else's.

use serde_json::Value;
use titan_doc::{ContentHash, DiskState, read_disk};
use titan_protocol::messages::{
    CheckDiskResult, DiskChangedParams, DocumentIdParams, ResolveConflictParams,
    ResolveConflictResult, notifications,
};
use titan_protocol::rpc::codes;
use titan_tasks::{Priority, TaskOutcome};

use super::{
    DiskStatus, Pending, Reply, ServerError, Session, TaskResult, check_revision, display_path,
    rpc_error, to_json,
};

const KEEP_BUFFER: &str = "keepBuffer";
const RELOAD_FROM_DISK: &str = "reloadFromDisk";

impl Session {
    /// Read the document's file in the background and reconcile when it is done. At most
    /// one check per document is in flight; requests made meanwhile coalesce into one more.
    pub(super) fn schedule_disk_check(&mut self, doc_id: u64, cause: &'static str) {
        let limits = self.options.limits;
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        if od.disk_check.is_some() {
            od.disk_recheck = Some(cause);
            return;
        }
        let path = od.doc.path().to_path_buf();
        let epoch = od.disk_epoch;
        let submitted = self
            .pool
            .submit("document.diskCheck", Priority::Normal, None, move |_| {
                TaskResult::Disk(
                    read_disk(&path, &limits)
                        .map(Box::new)
                        .map_err(|e| e.to_string()),
                )
            });
        match submitted {
            Ok((task, _)) => {
                od.disk_check = Some(task);
                self.counters.disk_checks += 1;
                self.pending.insert(
                    task,
                    Pending::DiskCheck {
                        doc_id,
                        epoch,
                        cause,
                    },
                );
            }
            Err(e) => {
                // Queue full: retried on the next request for this document.
                od.disk_recheck = Some(cause);
                eprintln!("titand: could not queue a disk check: {e}");
            }
        }
    }

    pub(super) fn on_disk_checked(
        &mut self,
        doc_id: u64,
        epoch: u64,
        cause: &'static str,
        outcome: TaskOutcome<TaskResult>,
    ) {
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        od.disk_check = None;
        if epoch != od.disk_epoch {
            // The document's disk version changed through us while the check ran.
            self.counters.stale_disk_checks += 1;
            od.disk_recheck.get_or_insert(cause);
            return self.finish_disk_check(doc_id);
        }
        let snapshot = match outcome {
            TaskOutcome::Completed(TaskResult::Disk(Ok(snapshot))) => Ok(snapshot),
            TaskOutcome::Completed(TaskResult::Disk(Err(message))) => Err(message),
            _ => Err("the disk check did not complete".to_string()),
        };
        let dirty = od.doc.is_dirty();
        let previous = od.disk_status.clone();
        let mut reloaded = false;
        let (status, kind) = match &snapshot {
            Ok(snap) if od.doc.disk_matches(&snap.state) => (DiskStatus::InSync, "restored"),
            Ok(snap) if !snap.state.exists => (DiskStatus::Deleted, "deleted"),
            Ok(snap) if !dirty => {
                let revision = od.doc.text().revision();
                let before = od.doc.text().snapshot();
                match od.doc.reload_from_disk(revision, snap) {
                    Ok(change) => {
                        od.disk_epoch += 1;
                        reloaded = true;
                        self.counters.disk_reloads += 1;
                        if let Some(change) = change {
                            self.after_change(doc_id, &before, &change);
                        }
                        (DiskStatus::InSync, "modified")
                    }
                    Err(e) => (DiskStatus::Unreadable(e.to_string()), "modified"),
                }
            }
            Ok(snap) => (DiskStatus::Modified(snap.state), "modified"),
            Err(message) => (DiskStatus::Unreadable(message.clone()), "modified"),
        };
        let Some(path) = self
            .documents
            .get(&doc_id)
            .map(|od| self.relative_display(od.doc.path()))
        else {
            return;
        };
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        let changed = status != previous || reloaded;
        od.disk_status = status;
        let conflict = !matches!(od.disk_status, DiskStatus::InSync);
        if changed && conflict && dirty {
            self.counters.disk_conflicts += 1;
        }
        if changed && self.caps.disk_notifications {
            let state = snapshot.as_ref().ok().map(|s| s.state).filter(|s| s.exists);
            let params = DiskChangedParams {
                document_id: doc_id,
                path,
                kind: kind.to_string(),
                action: if reloaded {
                    "reloaded"
                } else if conflict && dirty {
                    "conflict"
                } else {
                    "none"
                }
                .to_string(),
                dirty: od.doc.is_dirty(),
                revision: od.doc.text().revision(),
                disk_hash: state.map(|s| s.hash.to_hex()),
                disk_len: state.map(|s| s.len),
                cause: cause.to_string(),
            };
            self.push_notification(notifications::DOCUMENT_DISK_CHANGED, params);
        }
        self.finish_disk_check(doc_id);
    }

    /// Answer `document.checkDisk` waiters and start a coalesced re-check if one is due.
    fn finish_disk_check(&mut self, doc_id: u64) {
        let Some(od) = self.documents.get_mut(&doc_id) else {
            return;
        };
        if let Some(cause) = od.disk_recheck.take() {
            // Waiters keep waiting for the fresh result.
            return self.schedule_disk_check(doc_id, cause);
        }
        let waiters = std::mem::take(&mut od.disk_waiters);
        let result = CheckDiskResult {
            document_id: doc_id,
            changed_on_disk: od.disk_status != DiskStatus::InSync,
            disk: od.disk_status.as_str().to_string(),
        };
        for id in waiters {
            let value = to_json(&result);
            self.respond(id, value);
        }
    }

    pub(super) fn relative_display(&self, path: &std::path::Path) -> String {
        self.workspace
            .as_ref()
            .and_then(|ws| ws.relative(path))
            .map(|p| display_path(&p))
            .unwrap_or_else(|| display_path(path))
    }

    /// `document.checkDisk`: answered when a fresh background check completes.
    pub(super) fn check_disk(&mut self, params: DocumentIdParams, id: Option<Value>) -> Reply {
        let doc_id = params.document_id;
        let od = match self.open_doc_mut(doc_id) {
            Ok(od) => od,
            Err(e) => return Reply::Now(Err(e)),
        };
        if let Some(id) = id {
            od.disk_waiters.push(id);
        }
        if od.disk_check.is_some() {
            // The running check may predate the request: check once more afterwards.
            od.disk_recheck = Some("checkDisk");
        } else {
            self.schedule_disk_check(doc_id, "checkDisk");
        }
        Reply::Deferred
    }

    pub(super) fn resolve_conflict(
        &mut self,
        params: ResolveConflictParams,
        id: Option<Value>,
    ) -> Reply {
        match params.resolution.as_str() {
            KEEP_BUFFER => Reply::Now(self.keep_buffer(&params)),
            RELOAD_FROM_DISK => self.start_reload(params, id),
            other => Reply::Now(Err(rpc_error(
                codes::INVALID_PARAMS,
                format!(
                    "unknown resolution {other:?}; use \"{KEEP_BUFFER}\" or \"{RELOAD_FROM_DISK}\""
                ),
            ))),
        }
    }

    fn keep_buffer(&mut self, params: &ResolveConflictParams) -> Result<Value, ServerError> {
        let doc_id = params.document_id;
        let od = self.open_doc_mut(doc_id)?;
        check_revision(&od.doc, params.expected_revision)?;
        let state = match &od.disk_status {
            DiskStatus::InSync => {
                return Err(rpc_error(
                    codes::NO_CONFLICT,
                    "the document is in sync with the disk",
                ));
            }
            DiskStatus::Modified(state) => *state,
            DiskStatus::Deleted => DiskState::missing(),
            DiskStatus::Unreadable(message) => {
                return Err(rpc_error(
                    codes::IO_ERROR,
                    format!("the file cannot be read ({message}); save with force to replace it"),
                ));
            }
        };
        if let Some(seen) = params.disk_hash.as_deref() {
            let matches = state.exists && ContentHash::from_hex(seen) == Some(state.hash);
            if !matches {
                return Err(disk_changed_again());
            }
        }
        od.doc.adopt_disk_state(state);
        od.disk_epoch += 1;
        od.disk_status = DiskStatus::InSync;
        let resolution = KEEP_BUFFER.to_string();
        // The disk may have moved on since `state` was observed; a save still validates
        // against `state` and refuses anything newer.
        self.schedule_disk_check(doc_id, "resolveConflict");
        to_json(ResolveConflictResult {
            document: self.info(doc_id)?,
            resolution,
            reloaded: false,
        })
    }

    fn start_reload(&mut self, params: ResolveConflictParams, id: Option<Value>) -> Reply {
        let limits = self.options.limits;
        let doc_id = params.document_id;
        let od = match self.open_doc_mut(doc_id) {
            Ok(od) => od,
            Err(e) => return Reply::Now(Err(e)),
        };
        if let Err(e) = check_revision(&od.doc, params.expected_revision) {
            return Reply::Now(Err(e.into()));
        }
        let path = od.doc.path().to_path_buf();
        let epoch = od.disk_epoch;
        let submitted = self
            .pool
            .submit("document.reload", Priority::High, None, move |_| {
                TaskResult::Disk(
                    read_disk(&path, &limits)
                        .map(Box::new)
                        .map_err(|e| e.to_string()),
                )
            });
        match submitted {
            Ok((task, _)) => {
                self.pending.insert(
                    task,
                    Pending::DiskReload {
                        doc_id,
                        epoch,
                        id,
                        expected_revision: params.expected_revision,
                        disk_hash: params.disk_hash,
                    },
                );
                Reply::Deferred
            }
            Err(e) => {
                self.counters.busy_rejections += 1;
                Reply::Now(Err(rpc_error(codes::SERVER_BUSY, e.to_string())))
            }
        }
    }

    pub(super) fn on_disk_reloaded(
        &mut self,
        doc_id: u64,
        epoch: u64,
        id: Option<Value>,
        expected_revision: u64,
        disk_hash: Option<String>,
        outcome: TaskOutcome<TaskResult>,
    ) {
        let result = self.apply_reload(doc_id, epoch, expected_revision, disk_hash, outcome);
        match id {
            Some(id) => self.respond(id, result),
            None => {
                if let Err(e) = result {
                    let e = e.into_rpc();
                    eprintln!("titand: reload failed: {}", e.message);
                }
            }
        }
    }

    fn apply_reload(
        &mut self,
        doc_id: u64,
        epoch: u64,
        expected_revision: u64,
        disk_hash: Option<String>,
        outcome: TaskOutcome<TaskResult>,
    ) -> Result<Value, ServerError> {
        let snapshot = match outcome {
            TaskOutcome::Completed(TaskResult::Disk(Ok(snapshot))) => snapshot,
            TaskOutcome::Completed(TaskResult::Disk(Err(message))) => {
                return Err(rpc_error(codes::IO_ERROR, message));
            }
            _ => {
                return Err(rpc_error(
                    codes::INTERNAL_ERROR,
                    "the reload did not complete",
                ));
            }
        };
        let od = self.open_doc_mut(doc_id)?;
        if epoch != od.disk_epoch {
            return Err(disk_changed_again());
        }
        if !snapshot.state.exists {
            return Err(rpc_error(
                codes::IO_ERROR,
                format!("{} no longer exists", display_path(od.doc.path())),
            ));
        }
        if let Some(seen) = disk_hash.as_deref() {
            if ContentHash::from_hex(seen) != Some(snapshot.state.hash) {
                return Err(disk_changed_again());
            }
        }
        let before = od.doc.text().snapshot();
        let change = od.doc.reload_from_disk(expected_revision, &snapshot)?;
        od.disk_epoch += 1;
        od.disk_status = DiskStatus::InSync;
        self.counters.disk_reloads += 1;
        let reloaded = change.is_some();
        if let Some(change) = change {
            self.after_change(doc_id, &before, &change);
        }
        to_json(ResolveConflictResult {
            document: self.info(doc_id)?,
            resolution: RELOAD_FROM_DISK.to_string(),
            reloaded,
        })
    }
}

fn disk_changed_again() -> ServerError {
    rpc_error(
        codes::DISK_CHANGED_AGAIN,
        "the file changed again since the version you resolved against",
    )
}
