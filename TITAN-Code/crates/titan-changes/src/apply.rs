//! Applying a validated proposal to disk: prepare (stage + journal), commit file by file,
//! roll back on failure, recover after a crash, restore after success.
//!
//! Multi-file changes are **not atomic**: other processes can observe the workspace while
//! some files are written and others are not. What this module guarantees instead:
//!
//! * Nothing is written before every original file's bytes are copied into the
//!   transaction directory and the plan is journaled and `fsync`ed.
//! * Each file is replaced with [`titan_doc::replace_file`] against the exact content that
//!   was validated (creates against "absent"), so a concurrent change to a file is detected
//!   instead of overwritten.
//! * On failure the files already written are put back in reverse order, again only if
//!   they still hold exactly the bytes this transaction wrote. A file someone changed in
//!   the meantime is left alone and reported (both versions stay available: theirs on disk,
//!   ours and the original in the transaction directory).
//! * After a crash, [`recover`] finishes the rollback with the same rules.
//! * Deleting never unlinks content that has not been verified: the file is first moved
//!   aside (atomic rename), hashed, and only removed if it is exactly the expected content
//!   (a copy of which is in the transaction directory); otherwise it is put back.
//!
//! **Buffers.** Changes whose target is an open editor buffer are never written here. They
//! are returned as [`BufferEdits`] in the [`ApplyReport`] for the caller to apply in memory
//! (the server applies them as undoable transactions on the named revision). Only disk
//! targets take part in the transaction, the rollback and [`restore`].

use std::collections::HashSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use titan_doc::{ContentHash, Expected, ReplaceError, SavePhase, replace_file};

use crate::evidence::AffectedFile;
use crate::journal::{self, JournalWriter, OpKind, PlanOp, Record, Version};
use crate::model::{ByteEdit, Proposal, ProposalId, ProposalStatus, fresh_id, is_valid_id};
use crate::util::{
    DiskVersion, disk_version, file_mode, read_verified, set_mode, sync_dir, unix_ms,
    write_new_synced,
};
use crate::validate::{Resolved, Target, ValidationReport, analyze};
use crate::view::WorkspaceView;

/// Name of the directory under the caller's state directory that holds transactions and
/// evidence.
pub const CHANGES_DIR: &str = "changes";

/// Points at which [`apply_with_hook`] / [`restore_with_hook`] call the test hook.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplyPoint {
    /// Originals staged and the plan journaled; nothing written yet.
    Prepared,
    /// `begin <index>` is journaled; operation `index` is about to be written.
    BeforeOp { index: usize },
    /// Operation `index` was written and `applied <index>` journaled.
    AfterOp { index: usize },
    /// Inside [`titan_doc::replace_file`] for operation `index` (the return value is
    /// ignored here; use it to modify files at the worst moment).
    Save { index: usize, phase: SavePhase },
    /// Operation `index` is about to be undone (rollback, recovery or restore).
    BeforeUndo { index: usize },
}

/// What the hook wants to happen next.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HookAction {
    Continue,
    /// Fail at this point as if an I/O error had occurred (before an operation or after it:
    /// triggers the rollback; before an undo: that undo fails and is reported).
    Fail(String),
    /// Stop immediately without writing anything more, as if the process had been killed.
    /// The function returns [`ApplyError::Interrupted`]; [`recover`] finishes the job.
    Crash,
}

/// Overall result of applying a proposal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Outcome {
    /// Every disk operation was written.
    Applied,
    /// A failure occurred and every written file was put back.
    RolledBack,
    /// A failure occurred and at least one file could not be put back (see the conflicts).
    PartiallyApplied,
    /// A failure occurred before any file was written.
    Failed,
}

/// State of one operation at the end of [`apply`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum OpState {
    /// Never started.
    NotAttempted,
    /// Written (and still in place).
    Applied,
    /// Refused or failed before anything was written.
    #[serde(rename_all = "camelCase")]
    NotApplied { reason: String },
    /// Written, then put back during the rollback.
    Undone,
    /// Was found holding its original content during the rollback (nothing to undo).
    AlreadyOriginal,
    /// Could not be put back; see `rollback_conflicts`.
    UndoFailed,
}

/// Report for one planned file operation.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OpReport {
    pub index: usize,
    pub change_index: usize,
    pub kind: OpKind,
    pub path: String,
    pub state: OpState,
}

/// A file that could not be put back. Nothing was overwritten: the bytes on disk are left
/// as found, and this transaction's copies are named here.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RollbackConflict {
    pub path: String,
    /// The file in the workspace (as found, not modified by the rollback).
    pub disk_path: PathBuf,
    /// Copy of the original content in the transaction directory (none for creates).
    pub original_copy: Option<PathBuf>,
    /// Copy of the content this transaction wrote (none for deletes).
    pub proposed_copy: Option<PathBuf>,
    /// Any other file kept aside (for example a displaced file that could not be moved
    /// back).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kept_aside: Option<PathBuf>,
    pub detail: String,
}

/// Edits for an open buffer, to be applied by the caller (this crate never writes them).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BufferEdits {
    pub path: String,
    pub abs_path: PathBuf,
    /// The buffer revision the edits were validated against; apply them only at it.
    pub expected_revision: u64,
    pub edits: Vec<ByteEdit>,
    /// BLAKE3 (hex) of the buffer text before and after the edits.
    pub before_hash: String,
    pub after_hash: String,
}

/// Result of [`apply`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ApplyReport {
    pub proposal_id: ProposalId,
    pub patch_id: String,
    pub txn_id: String,
    pub txn_dir: PathBuf,
    pub outcome: Outcome,
    pub validation: ValidationReport,
    pub ops: Vec<OpReport>,
    pub affected: Vec<AffectedFile>,
    /// Changes to open buffers, for the caller. Only meaningful when `outcome` is
    /// `applied`.
    pub buffer_edits: Vec<BufferEdits>,
    /// Why the application stopped.
    pub failure: Option<String>,
    pub rollback_conflicts: Vec<RollbackConflict>,
}

impl ApplyReport {
    /// The proposal status matching this outcome.
    pub fn status(&self) -> ProposalStatus {
        match self.outcome {
            Outcome::Applied => ProposalStatus::Applied,
            Outcome::RolledBack => ProposalStatus::RolledBack,
            Outcome::PartiallyApplied => ProposalStatus::PartiallyApplied,
            Outcome::Failed => ProposalStatus::Failed {
                reason: self.failure.clone().unwrap_or_default(),
            },
        }
    }
}

/// Why [`apply`] did not run the transaction to an outcome.
#[derive(Debug, thiserror::Error)]
pub enum ApplyError {
    /// Validation found problems; nothing was written.
    #[error("the proposal is not valid ({} problems)", .0.problems.len())]
    Invalid(Box<ValidationReport>),
    /// Annotations need confirmation and the proposal is not approved; nothing written.
    #[error("annotations {annotations:?} need confirmation; approve the proposal first")]
    NeedsApproval { annotations: Vec<String> },
    /// Staging or journaling failed before any workspace file was written.
    #[error("could not prepare the transaction: {source}")]
    Prepare {
        #[source]
        source: io::Error,
    },
    /// The hook asked to stop ([`HookAction::Crash`]); the journal is left as a crash
    /// would leave it.
    #[error("interrupted (simulated crash); run recover() for transaction {txn_id}")]
    Interrupted { txn_id: String, txn_dir: PathBuf },
}

/// Errors of [`recover`], [`restore`] and the evidence store.
#[derive(Debug, thiserror::Error)]
pub enum ChangeError {
    #[error("{context}: {source}")]
    Io {
        context: String,
        #[source]
        source: io::Error,
    },
    #[error("cannot restore: {reason}")]
    NotRestorable { reason: String },
    #[error("invalid identifier {id:?}")]
    InvalidId { id: String },
    #[error("evidence {path:?} is unreadable: {reason}")]
    Evidence { path: PathBuf, reason: String },
    #[error("interrupted (simulated crash) in transaction {txn_id}")]
    Interrupted { txn_id: String },
}

pub(crate) fn io_ctx(context: impl Into<String>) -> impl FnOnce(io::Error) -> ChangeError {
    let context = context.into();
    move |source| ChangeError::Io { context, source }
}

pub(crate) fn changes_dir(state_dir: &Path) -> PathBuf {
    state_dir.join(CHANGES_DIR)
}

fn txn_dir(state_dir: &Path, txn_id: &str) -> PathBuf {
    changes_dir(state_dir).join(format!("txn-{txn_id}"))
}

fn before_copy(dir: &Path, index: usize) -> PathBuf {
    dir.join("before").join(index.to_string())
}

fn after_copy(dir: &Path, index: usize) -> PathBuf {
    dir.join("after").join(index.to_string())
}

fn trash_path(dir: &Path, index: usize, tag: &str) -> PathBuf {
    dir.join("trash").join(format!("{index}-{tag}"))
}

/// Hidden name next to `path` used while deleting it.
fn aside_path(path: &Path, txn_id: &str, index: usize, tag: &str) -> PathBuf {
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let short: String = txn_id.chars().take(12).collect();
    path.with_file_name(format!(".{name}.titan-{short}-{index}-{tag}"))
}

fn version_of(bytes: &[u8]) -> Version {
    Version {
        hash: ContentHash::of(bytes).to_hex(),
        len: bytes.len() as u64,
    }
}

fn parse_version(version: &Version) -> io::Result<(ContentHash, u64)> {
    ContentHash::from_hex(&version.hash)
        .map(|hash| (hash, version.len))
        .ok_or_else(|| io::Error::other(format!("bad hash {:?} in the journal", version.hash)))
}

fn matches_version(found: DiskVersion, expected: &Option<Version>) -> bool {
    match (found, expected) {
        (DiskVersion::Absent, None) => true,
        (DiskVersion::File { hash, len }, Some(v)) => v.len == len && v.hash == hash.to_hex(),
        _ => false,
    }
}

fn describe(found: DiskVersion) -> String {
    match found {
        DiskVersion::Absent => "no file".to_string(),
        DiskVersion::File { hash, len } => format!("{len} bytes with blake3 {hash}"),
        DiskVersion::Other => "something that is not a regular file".to_string(),
    }
}

/// Missing ancestors of `path` below `root`, outermost first.
fn missing_dirs(root: &Path, path: &Path) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    let mut current = path.parent();
    while let Some(dir) = current {
        if dir == root || !dir.starts_with(root) || dir.exists() {
            break;
        }
        dirs.push(dir.to_path_buf());
        current = dir.parent();
    }
    dirs.reverse();
    dirs
}

/// Permission bits a newly created file gets in this process (honours the umask).
fn default_mode(probe: &Path) -> Option<u32> {
    fs::File::create(probe).ok()?;
    let mode = file_mode(probe);
    let _ = fs::remove_file(probe);
    mode
}

enum StepError {
    /// Refused; nothing of ours was written.
    Conflict(String),
    /// Failed in a way that may or may not have written our bytes.
    Unknown(String),
}

fn replace_error(e: ReplaceError) -> StepError {
    match e {
        ReplaceError::Conflict {
            kind,
            preserved_copy: None,
        } => StepError::Conflict(format!("conflict: the file was {kind}")),
        ReplaceError::Conflict {
            kind,
            preserved_copy: Some(copy),
        } => StepError::Unknown(format!(
            "conflict: the file was {kind}; the displaced version was kept at {}",
            copy.display()
        )),
        ReplaceError::Io(e) => StepError::Unknown(format!("I/O error: {e}")),
    }
}

/// Outcome of a verified delete that did not happen.
struct DeleteRefused {
    detail: String,
    kept_aside: Option<PathBuf>,
}

/// Delete `path` only if it holds exactly `expected`: move it aside atomically, hash what
/// was moved, and remove it only on a match (moving it into `trash` when possible).
/// Otherwise move it back without overwriting anything.
fn verified_delete(
    path: &Path,
    expected: &Version,
    aside: &Path,
    trash: &Path,
) -> Result<(), Result<DeleteRefused, String>> {
    let (hash, len) = parse_version(expected).map_err(|e| Err(e.to_string()))?;
    if fs::symlink_metadata(aside).is_ok() {
        return Err(Err(format!(
            "the temporary name {} is already in use",
            aside.display()
        )));
    }
    match fs::rename(path, aside) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            return Err(Ok(DeleteRefused {
                detail: "the file no longer exists".to_string(),
                kept_aside: None,
            }));
        }
        Err(e) => return Err(Err(format!("could not move the file aside: {e}"))),
    }
    let dir = path.parent().unwrap_or(Path::new("."));
    let moved = disk_version(aside);
    if matches!(moved, Ok(DiskVersion::File { hash: h, len: n }) if h == hash && n == len) {
        if fs::rename(aside, trash).is_err() {
            // Different file system: the verified bytes are in the transaction directory.
            fs::remove_file(aside).map_err(|e| Err(format!("could not remove: {e}")))?;
        }
        sync_dir(dir).map_err(|e| Err(format!("could not sync the directory: {e}")))?;
        return Ok(());
    }
    // Not the expected content: put it back, never over another file.
    let found = moved.map(describe).unwrap_or_else(|e| e.to_string());
    match fs::hard_link(aside, path) {
        Ok(()) => {
            let _ = fs::remove_file(aside);
            let _ = sync_dir(dir);
            Err(Ok(DeleteRefused {
                detail: format!("the file changed (found {found}); left in place"),
                kept_aside: None,
            }))
        }
        Err(_) => Err(Ok(DeleteRefused {
            detail: format!(
                "the file changed (found {found}) and could not be moved back; it was kept at {}",
                aside.display()
            ),
            kept_aside: Some(aside.to_path_buf()),
        })),
    }
}

fn create_dirs(dirs: &[PathBuf]) -> io::Result<()> {
    for dir in dirs {
        match fs::create_dir(dir) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn remove_created_dirs(dirs: &[PathBuf]) {
    for dir in dirs.iter().rev() {
        // Only empty directories are removed; anything else stays.
        let _ = fs::remove_dir(dir);
    }
}

/// Write operation `op` (whose new content is `after`).
fn commit_op(
    op: &PlanOp,
    after: &[u8],
    txn_id: &str,
    dir: &Path,
    save_hook: &mut dyn FnMut(SavePhase),
) -> Result<(), StepError> {
    match (&op.before, &op.after) {
        (Some(before), Some(_)) => {
            let (hash, len) =
                parse_version(before).map_err(|e| StepError::Conflict(e.to_string()))?;
            replace_file(&op.path, after, Expected::Content { hash, len }, save_hook)
                .map(|_| ())
                .map_err(replace_error)
        }
        (None, Some(_)) => {
            create_dirs(&op.created_dirs)
                .map_err(|e| StepError::Unknown(format!("could not create directories: {e}")))?;
            replace_file(&op.path, after, Expected::Absent, save_hook).map_err(replace_error)?;
            set_mode(&op.path, op.mode)
                .map_err(|e| StepError::Unknown(format!("could not set permissions: {e}")))
        }
        (Some(before), None) => verified_delete(
            &op.path,
            before,
            &aside_path(&op.path, txn_id, op.index, "del"),
            &trash_path(dir, op.index, "deleted"),
        )
        .map_err(|e| match e {
            Ok(refused) => match refused.kept_aside {
                None => StepError::Conflict(format!("conflict: {}", refused.detail)),
                Some(_) => StepError::Unknown(refused.detail),
            },
            Err(detail) => StepError::Unknown(detail),
        }),
        (None, None) => Ok(()),
    }
}

enum Undo {
    Undone,
    AlreadyOriginal,
    Conflict(RollbackConflict),
}

fn conflict_for(
    op: &PlanOp,
    dir: &Path,
    detail: String,
    kept: Option<PathBuf>,
) -> RollbackConflict {
    RollbackConflict {
        path: op.rel.clone(),
        disk_path: op.path.clone(),
        original_copy: op.before.as_ref().map(|_| before_copy(dir, op.index)),
        proposed_copy: op.after.as_ref().map(|_| after_copy(dir, op.index)),
        kept_aside: kept,
        detail,
    }
}

/// Put operation `op` back, deciding by the content currently on disk:
/// our bytes → restore the original; the original → nothing to do; anything else → leave
/// it and report.
fn undo_op(op: &PlanOp, txn_id: &str, dir: &Path) -> Undo {
    let found = match disk_version(&op.path) {
        Ok(found) => found,
        Err(e) => {
            return Undo::Conflict(conflict_for(op, dir, format!("could not read: {e}"), None));
        }
    };
    let result = if op.before == op.after || !matches_version(found, &op.after) {
        if matches_version(found, &op.before) {
            if op.before.is_none() {
                remove_created_dirs(&op.created_dirs);
            }
            Undo::AlreadyOriginal
        } else {
            Undo::Conflict(conflict_for(
                op,
                dir,
                format!(
                    "the file was changed by someone else after it was written (found {}); \
                     left as is",
                    describe(found)
                ),
                None,
            ))
        }
    } else {
        match undo_write(op, txn_id, dir) {
            Ok(()) => Undo::Undone,
            Err((detail, kept)) => Undo::Conflict(conflict_for(op, dir, detail, kept)),
        }
    };
    // A delete interrupted between "move aside" and "remove" leaves the moved file behind.
    if let (Some(before), None) = (&op.before, &op.after) {
        let aside = aside_path(&op.path, txn_id, op.index, "del");
        if fs::symlink_metadata(&aside).is_ok() {
            let ours = matches!(disk_version(&aside), Ok(found) if matches_version(found, &Some(before.clone())));
            let restored = matches!(result, Undo::Undone | Undo::AlreadyOriginal);
            if ours && restored {
                let _ = fs::remove_file(&aside);
            } else {
                return Undo::Conflict(conflict_for(
                    op,
                    dir,
                    format!(
                        "a file moved aside during the delete was kept at {}",
                        aside.display()
                    ),
                    Some(aside),
                ));
            }
        }
    }
    result
}

fn undo_write(op: &PlanOp, txn_id: &str, dir: &Path) -> Result<(), (String, Option<PathBuf>)> {
    let original = |before: &Version| -> Result<Vec<u8>, (String, Option<PathBuf>)> {
        let (hash, len) = parse_version(before).map_err(|e| (e.to_string(), None))?;
        read_verified(&before_copy(dir, op.index), hash, len)
            .map_err(|e| (format!("the original copy is unusable: {e}"), None))
    };
    let replace_detail = |e: ReplaceError| match e {
        ReplaceError::Conflict {
            kind,
            preserved_copy,
        } => (
            format!("the file was {kind} while it was being restored; left as found"),
            preserved_copy,
        ),
        ReplaceError::Io(e) => (format!("I/O error while restoring: {e}"), None),
    };
    match (&op.before, &op.after) {
        (Some(before), Some(after)) => {
            let bytes = original(before)?;
            let (hash, len) = parse_version(after).map_err(|e| (e.to_string(), None))?;
            replace_file(
                &op.path,
                &bytes,
                Expected::Content { hash, len },
                &mut |_| {},
            )
            .map(|_| ())
            .map_err(replace_detail)
        }
        (Some(before), None) => {
            let bytes = original(before)?;
            replace_file(&op.path, &bytes, Expected::Absent, &mut |_| {})
                .map_err(replace_detail)?;
            set_mode(&op.path, op.mode)
                .map_err(|e| (format!("could not set permissions: {e}"), None))
        }
        (None, Some(after)) => {
            let outcome = verified_delete(
                &op.path,
                after,
                &aside_path(&op.path, txn_id, op.index, "undo"),
                &trash_path(dir, op.index, "undone"),
            );
            match outcome {
                Ok(()) => {
                    remove_created_dirs(&op.created_dirs);
                    Ok(())
                }
                Err(Ok(refused)) => Err((refused.detail, refused.kept_aside)),
                Err(Err(detail)) => Err((detail, None)),
            }
        }
        (None, None) => Ok(()),
    }
}

/// Per-operation progress while applying.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Progress {
    NotStarted,
    /// Refused before writing anything of ours.
    NotWritten,
    Done,
    /// Failed in a way that may have written; decided by content during the rollback.
    Unknown,
}

struct Plan {
    ops: Vec<PlanOp>,
    after: Vec<Vec<u8>>,
    before: Vec<Option<Vec<u8>>>,
    buffer_edits: Vec<BufferEdits>,
    affected: Vec<AffectedFile>,
}

fn build_plan(resolved: Vec<Resolved>, root: &Path, change_indices: &[usize]) -> Plan {
    let mut plan = Plan {
        ops: Vec::new(),
        after: Vec::new(),
        before: Vec::new(),
        buffer_edits: Vec::new(),
        affected: Vec::new(),
    };
    let push = |plan: &mut Plan,
                change_index: usize,
                kind: OpKind,
                rel: String,
                path: PathBuf,
                before: Option<Vec<u8>>,
                after: Option<Vec<u8>>,
                mode: Option<u32>| {
        let created_dirs = if before.is_none() {
            missing_dirs(root, &path)
        } else {
            Vec::new()
        };
        plan.affected.push(AffectedFile {
            path: rel.clone(),
            before_hash: before.as_deref().map(|b| ContentHash::of(b).to_hex()),
            after_hash: after.as_deref().map(|a| ContentHash::of(a).to_hex()),
            target: Target::Disk,
        });
        plan.ops.push(PlanOp {
            index: plan.ops.len(),
            change_index,
            kind,
            rel,
            path,
            before: before.as_deref().map(version_of),
            after: after.as_deref().map(version_of),
            mode,
            created_dirs,
        });
        plan.after.push(after.unwrap_or_default());
        plan.before.push(before);
    };
    for (resolved, &change_index) in resolved.into_iter().zip(change_indices) {
        match resolved {
            Resolved::DiskEdit {
                rel,
                abs,
                before,
                after,
            } => push(
                &mut plan,
                change_index,
                OpKind::Write,
                rel,
                abs,
                Some(before),
                Some(after),
                None,
            ),
            Resolved::DiskCreate { rel, abs, after } => push(
                &mut plan,
                change_index,
                OpKind::Create,
                rel,
                abs,
                None,
                Some(after),
                None,
            ),
            Resolved::DiskDelete { rel, abs, before } => {
                let mode = file_mode(&abs);
                push(
                    &mut plan,
                    change_index,
                    OpKind::Delete,
                    rel,
                    abs,
                    Some(before),
                    None,
                    mode,
                );
            }
            Resolved::DiskRename {
                from_rel,
                from_abs,
                to_rel,
                to_abs,
                bytes,
            } => {
                let mode = file_mode(&from_abs);
                push(
                    &mut plan,
                    change_index,
                    OpKind::Create,
                    to_rel,
                    to_abs,
                    None,
                    Some(bytes.clone()),
                    mode,
                );
                push(
                    &mut plan,
                    change_index,
                    OpKind::Delete,
                    from_rel,
                    from_abs,
                    Some(bytes),
                    None,
                    mode,
                );
            }
            Resolved::BufferEdit {
                rel,
                abs,
                revision,
                edits,
                before_hash,
                after_hash,
            } => {
                plan.affected.push(AffectedFile {
                    path: rel.clone(),
                    before_hash: Some(before_hash.to_hex()),
                    after_hash: Some(after_hash.to_hex()),
                    target: Target::Buffer,
                });
                plan.buffer_edits.push(BufferEdits {
                    path: rel,
                    abs_path: abs,
                    expected_revision: revision,
                    edits,
                    before_hash: before_hash.to_hex(),
                    after_hash: after_hash.to_hex(),
                });
            }
        }
    }
    plan
}

/// Stage the originals and proposed bytes and write the `prepared` record.
fn prepare(
    dir: &Path,
    txn_id: &str,
    proposal: &Proposal,
    patch_id: &str,
    root: &Path,
    plan: &mut Plan,
) -> io::Result<JournalWriter> {
    fs::create_dir_all(dir.parent().unwrap_or(dir))?;
    fs::create_dir(dir)?;
    for sub in ["before", "after", "trash"] {
        fs::create_dir(dir.join(sub))?;
    }
    let default = default_mode(&dir.join("mode-probe"));
    for (i, op) in plan.ops.iter_mut().enumerate() {
        if op.kind == OpKind::Create && op.mode.is_none() {
            op.mode = default;
        }
        if let Some(before) = &plan.before[i] {
            write_new_synced(&before_copy(dir, i), before)?;
        }
        if op.after.is_some() {
            write_new_synced(&after_copy(dir, i), &plan.after[i])?;
        }
    }
    for sub in ["before", "after", "trash"] {
        sync_dir(&dir.join(sub))?;
    }
    sync_dir(dir)?;
    let journal = JournalWriter::create(
        &dir.join("journal"),
        &Record::Prepared {
            format_version: journal::FORMAT_VERSION,
            txn_id: txn_id.to_string(),
            proposal_id: proposal.id.to_string(),
            patch_id: patch_id.to_string(),
            workspace_root: root.to_path_buf(),
            created_unix_ms: unix_ms(),
            ops: plan.ops.clone(),
        },
    )?;
    if let Some(parent) = dir.parent() {
        sync_dir(parent)?;
    }
    Ok(journal)
}

/// [`apply_with_hook`] without a hook.
pub fn apply(
    proposal: &Proposal,
    view: &dyn WorkspaceView,
    state_dir: &Path,
) -> Result<ApplyReport, ApplyError> {
    apply_with_hook(proposal, view, state_dir, &mut |_| HookAction::Continue)
}

/// Validate `proposal` again, then apply its disk changes as a journaled transaction under
/// `state_dir/changes/txn-<id>/` (see the module documentation for the guarantees). Buffer
/// changes are returned in [`ApplyReport::buffer_edits`]. `hook` is called at the
/// [`ApplyPoint`]s; tests use it to inject failures and simulated crashes.
pub fn apply_with_hook(
    proposal: &Proposal,
    view: &dyn WorkspaceView,
    state_dir: &Path,
    hook: &mut dyn FnMut(ApplyPoint) -> HookAction,
) -> Result<ApplyReport, ApplyError> {
    let analysis = analyze(proposal, view);
    let validation = analysis.report;
    if !validation.ok {
        return Err(ApplyError::Invalid(Box::new(validation)));
    }
    if !validation.needs_confirmation.is_empty() && proposal.approval().is_none() {
        return Err(ApplyError::NeedsApproval {
            annotations: validation
                .needs_confirmation
                .iter()
                .map(|c| c.annotation_id.clone())
                .collect(),
        });
    }
    let change_indices: Vec<usize> = validation.preview.iter().map(|p| p.change_index).collect();
    let mut plan = build_plan(analysis.resolved, view.root(), &change_indices);
    let txn_id = fresh_id("txn");
    let dir = txn_dir(state_dir, &txn_id);
    let patch_id = validation.patch_id.clone();
    let mut journal = match prepare(&dir, &txn_id, proposal, &patch_id, view.root(), &mut plan) {
        Ok(journal) => journal,
        Err(source) => {
            // Nothing in the workspace was touched; drop the partial staging area.
            let _ = fs::remove_dir_all(&dir);
            return Err(ApplyError::Prepare { source });
        }
    };
    let interrupted = || ApplyError::Interrupted {
        txn_id: txn_id.clone(),
        txn_dir: dir.clone(),
    };

    let count = plan.ops.len();
    let mut progress = vec![Progress::NotStarted; count];
    let mut reasons: Vec<Option<String>> = vec![None; count];
    let mut failure: Option<String> = None;
    match hook(ApplyPoint::Prepared) {
        HookAction::Continue => {}
        HookAction::Fail(reason) => failure = Some(format!("injected failure: {reason}")),
        HookAction::Crash => return Err(interrupted()),
    }
    let mut i = 0;
    while failure.is_none() && i < count {
        if let Err(e) = journal.append(&Record::Begin { index: i }) {
            failure = Some(format!("could not write the journal: {e}"));
            break;
        }
        match hook(ApplyPoint::BeforeOp { index: i }) {
            HookAction::Continue => {}
            HookAction::Fail(reason) => {
                let reason = format!("injected failure: {reason}");
                progress[i] = Progress::NotWritten;
                let _ = journal.append(&Record::NotApplied {
                    index: i,
                    reason: reason.clone(),
                });
                reasons[i] = Some(reason.clone());
                failure = Some(format!("{}: {reason}", plan.ops[i].rel));
                break;
            }
            HookAction::Crash => return Err(interrupted()),
        }
        let op = &plan.ops[i];
        let result = commit_op(op, &plan.after[i], &txn_id, &dir, &mut |phase| {
            let _ = hook(ApplyPoint::Save { index: i, phase });
        });
        match result {
            Ok(()) => {
                progress[i] = Progress::Done;
                if let Err(e) = journal.append(&Record::Applied { index: i }) {
                    failure = Some(format!("could not write the journal: {e}"));
                    break;
                }
            }
            Err(StepError::Conflict(reason)) => {
                progress[i] = Progress::NotWritten;
                let _ = journal.append(&Record::NotApplied {
                    index: i,
                    reason: reason.clone(),
                });
                failure = Some(format!("{}: {reason}", op.rel));
                reasons[i] = Some(reason);
                break;
            }
            Err(StepError::Unknown(reason)) => {
                progress[i] = Progress::Unknown;
                failure = Some(format!("{}: {reason}", op.rel));
                reasons[i] = Some(reason);
                break;
            }
        }
        match hook(ApplyPoint::AfterOp { index: i }) {
            HookAction::Continue => {}
            HookAction::Fail(reason) => {
                failure = Some(format!(
                    "injected failure after {}: {reason}",
                    plan.ops[i].rel
                ));
                break;
            }
            HookAction::Crash => return Err(interrupted()),
        }
        i += 1;
    }
    if failure.is_none() {
        match journal.append(&Record::Committed) {
            Ok(()) => {}
            Err(e) => failure = Some(format!("could not record the commit: {e}")),
        }
    }

    let mut op_states: Vec<OpState> = (0..count)
        .map(|i| match progress[i] {
            Progress::NotStarted => OpState::NotAttempted,
            Progress::NotWritten => OpState::NotApplied {
                reason: reasons[i].clone().unwrap_or_default(),
            },
            Progress::Done | Progress::Unknown => OpState::Applied,
        })
        .collect();
    let mut rollback_conflicts = Vec::new();
    let outcome = match &failure {
        None => Outcome::Applied,
        Some(reason) => {
            let to_undo: Vec<usize> = (0..count)
                .rev()
                .filter(|&i| matches!(progress[i], Progress::Done | Progress::Unknown))
                .collect();
            let _ = journal.append(&Record::RollbackStarted {
                reason: reason.clone(),
            });
            let undone = undo_all(&plan.ops, &to_undo, &txn_id, &dir, &mut journal, hook)
                .map_err(|_| interrupted())?;
            for (index, state) in undone.states {
                op_states[index] = state;
            }
            rollback_conflicts = undone.conflicts;
            let _ = journal.append(&if rollback_conflicts.is_empty() {
                Record::RolledBack
            } else {
                Record::RollbackIncomplete {
                    conflicts: rollback_conflicts.iter().map(|c| c.path.clone()).collect(),
                }
            });
            if !rollback_conflicts.is_empty() {
                Outcome::PartiallyApplied
            } else if to_undo.is_empty() {
                Outcome::Failed
            } else {
                Outcome::RolledBack
            }
        }
    };
    let ops = plan
        .ops
        .iter()
        .zip(op_states)
        .map(|(op, state)| OpReport {
            index: op.index,
            change_index: op.change_index,
            kind: op.kind,
            path: op.rel.clone(),
            state,
        })
        .collect();
    Ok(ApplyReport {
        proposal_id: proposal.id.clone(),
        patch_id,
        txn_id: txn_id.clone(),
        txn_dir: dir.clone(),
        outcome,
        validation,
        ops,
        affected: plan.affected,
        buffer_edits: if outcome == Outcome::Applied {
            plan.buffer_edits
        } else {
            Vec::new()
        },
        failure,
        rollback_conflicts,
    })
}

struct UndoSummary {
    states: Vec<(usize, OpState)>,
    conflicts: Vec<RollbackConflict>,
}

struct Interrupt;

fn undone_paths(ops: &[PlanOp], summary: &UndoSummary) -> Vec<String> {
    summary
        .states
        .iter()
        .filter(|(_, state)| *state == OpState::Undone)
        .filter_map(|(index, _)| ops.get(*index).map(|op| op.rel.clone()))
        .collect()
}

/// Undo `indices` (already in the order to undo), journaling each result.
fn undo_all(
    ops: &[PlanOp],
    indices: &[usize],
    txn_id: &str,
    dir: &Path,
    journal: &mut JournalWriter,
    hook: &mut dyn FnMut(ApplyPoint) -> HookAction,
) -> Result<UndoSummary, Interrupt> {
    let mut summary = UndoSummary {
        states: Vec::new(),
        conflicts: Vec::new(),
    };
    for &index in indices {
        let Some(op) = ops.get(index) else {
            continue;
        };
        let undo = match hook(ApplyPoint::BeforeUndo { index }) {
            HookAction::Continue => undo_op(op, txn_id, dir),
            HookAction::Fail(reason) => Undo::Conflict(conflict_for(
                op,
                dir,
                format!("injected failure while undoing: {reason}"),
                None,
            )),
            HookAction::Crash => return Err(Interrupt),
        };
        let (state, result) = match undo {
            Undo::Undone => (OpState::Undone, "undone".to_string()),
            Undo::AlreadyOriginal => (OpState::AlreadyOriginal, "alreadyOriginal".to_string()),
            Undo::Conflict(conflict) => {
                let detail = format!("conflict: {}", conflict.detail);
                summary.conflicts.push(conflict);
                (OpState::UndoFailed, detail)
            }
        };
        let _ = journal.append(&Record::Undone { index, result });
        summary.states.push((index, state));
    }
    Ok(summary)
}

/// What [`recover`] did with one unfinished transaction.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RecoveryAction {
    /// Every file the transaction may have written holds its original content again.
    RolledBack,
    /// Some files could not be put back (see the conflicts).
    RollbackIncomplete,
    /// An interrupted [`restore`] was finished.
    Restored,
    /// An interrupted [`restore`] was finished except for the conflicts.
    RestoreIncomplete,
    /// The transaction never got past preparation; no workspace file was written.
    NothingToDo,
    /// The journal could not be used; nothing was touched.
    Unreadable,
}

/// One transaction handled by [`recover`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveredTxn {
    pub txn_id: String,
    pub proposal_id: Option<String>,
    pub txn_dir: PathBuf,
    pub action: RecoveryAction,
    /// Paths put back to their original content.
    pub undone: Vec<String>,
    pub conflicts: Vec<RollbackConflict>,
    pub notes: Vec<String>,
}

/// Result of [`recover`].
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecoveryReport {
    pub transactions: Vec<RecoveredTxn>,
}

/// Find transactions under `state_dir/changes/` whose journal has no terminal record (the
/// process stopped while applying, rolling back or restoring) and finish them by rolling
/// back with the same rules as a failed apply. Call it at startup, before applying
/// anything else; it must not run concurrently with an apply on the same state directory.
pub fn recover(state_dir: &Path) -> Result<RecoveryReport, ChangeError> {
    let root = changes_dir(state_dir);
    let mut report = RecoveryReport::default();
    let entries = match fs::read_dir(&root) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(report),
        Err(e) => return Err(io_ctx(format!("reading {}", root.display()))(e)),
    };
    let mut dirs: Vec<(String, PathBuf)> = entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let name = entry.file_name().to_str()?.to_string();
            let id = name.strip_prefix("txn-")?.to_string();
            entry.file_type().ok()?.is_dir().then(|| (id, entry.path()))
        })
        .collect();
    dirs.sort();
    for (txn_id, dir) in dirs {
        if let Some(recovered) = recover_txn(&txn_id, &dir)? {
            report.transactions.push(recovered);
        }
    }
    Ok(report)
}

fn recover_txn(txn_id: &str, dir: &Path) -> Result<Option<RecoveredTxn>, ChangeError> {
    let mut recovered = RecoveredTxn {
        txn_id: txn_id.to_string(),
        proposal_id: None,
        txn_dir: dir.to_path_buf(),
        action: RecoveryAction::NothingToDo,
        undone: Vec::new(),
        conflicts: Vec::new(),
        notes: Vec::new(),
    };
    let journal_path = dir.join("journal");
    let parsed = match journal::read(&journal_path) {
        Ok(parsed) => parsed,
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            recovered
                .notes
                .push("no journal: preparation did not finish; no file was written".to_string());
            return Ok(Some(recovered));
        }
        Err(e) => return Err(io_ctx(format!("reading {}", journal_path.display()))(e)),
    };
    if parsed.is_finished() {
        return Ok(None);
    }
    if let Some(reason) = &parsed.damaged {
        // A record in the middle does not check out (not a torn final write): the journal
        // cannot be trusted to say what happened, so nothing is touched automatically.
        recovered.action = RecoveryAction::Unreadable;
        recovered.proposal_id = parsed.prepared().map(|(_, id, _)| id.to_string());
        recovered.notes.push(format!(
            "journal damaged ({reason}); nothing was touched. The original and proposed \
             bytes of every file are in the transaction directory"
        ));
        return Ok(Some(recovered));
    }
    if parsed.torn_tail {
        recovered
            .notes
            .push("the last journal record was incomplete and was ignored".to_string());
    }
    let Some((_, proposal_id, ops)) = parsed.prepared() else {
        recovered
            .notes
            .push("the plan was never recorded; no file was written".to_string());
        return Ok(Some(recovered));
    };
    recovered.proposal_id = Some(proposal_id.to_string());
    let restoring = parsed
        .records
        .iter()
        .any(|r| matches!(r, Record::RestoreStarted));
    let mut begun: HashSet<usize> = HashSet::new();
    for record in &parsed.records {
        match record {
            Record::Begin { index } => {
                begun.insert(*index);
            }
            Record::NotApplied { index, .. } => {
                begun.remove(index);
            }
            _ => {}
        }
    }
    let mut indices: Vec<usize> = begun.into_iter().collect();
    indices.sort_unstable_by(|a, b| b.cmp(a));
    let mut journal = JournalWriter::open(&journal_path)
        .map_err(io_ctx(format!("opening {}", journal_path.display())))?;
    if !restoring {
        let _ = journal.append(&Record::RollbackStarted {
            reason: "recovery after an interrupted apply".to_string(),
        });
    }
    let summary = undo_all(ops, &indices, txn_id, dir, &mut journal, &mut |_| {
        HookAction::Continue
    })
    .map_err(|_| ChangeError::Interrupted {
        txn_id: txn_id.to_string(),
    })?;
    recovered.undone = undone_paths(ops, &summary);
    let paths: Vec<String> = summary.conflicts.iter().map(|c| c.path.clone()).collect();
    let complete = summary.conflicts.is_empty();
    recovered.conflicts = summary.conflicts;
    let (record, action) = match (restoring, complete) {
        (false, true) => (Record::RolledBack, RecoveryAction::RolledBack),
        (false, false) => (
            Record::RollbackIncomplete { conflicts: paths },
            RecoveryAction::RollbackIncomplete,
        ),
        (true, true) => (Record::Restored, RecoveryAction::Restored),
        (true, false) => (
            Record::RestoreIncomplete { conflicts: paths },
            RecoveryAction::RestoreIncomplete,
        ),
    };
    journal
        .append(&record)
        .map_err(io_ctx(format!("writing {}", journal_path.display())))?;
    recovered.action = action;
    Ok(Some(recovered))
}

/// A file [`restore`] refused to touch.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RestoreRefusal {
    pub path: String,
    pub detail: String,
}

/// Result of [`restore`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RestoreReport {
    /// Every file holds its original content again.
    pub restored: bool,
    /// Files that no longer hold the applied content; when non-empty nothing was touched.
    pub refused: Vec<RestoreRefusal>,
    /// Paths put back to their original content.
    pub undone: Vec<String>,
    /// Files that changed while the restore ran (left as found).
    pub conflicts: Vec<RollbackConflict>,
}

/// [`restore_with_hook`] without a hook.
pub fn restore(
    state_dir: &Path,
    evidence: &crate::evidence::EvidenceRecord,
) -> Result<RestoreReport, ChangeError> {
    restore_with_hook(state_dir, evidence, &mut |_| HookAction::Continue)
}

/// Put back the original disk content of a successfully applied proposal, using the
/// copies in its transaction directory. Refuses (touching nothing) unless every file still
/// holds exactly the content the transaction wrote; the refusal names each changed file.
/// Buffer edits are not covered (undo them in the editor).
pub fn restore_with_hook(
    state_dir: &Path,
    evidence: &crate::evidence::EvidenceRecord,
    hook: &mut dyn FnMut(ApplyPoint) -> HookAction,
) -> Result<RestoreReport, ChangeError> {
    let not_restorable = |reason: String| ChangeError::NotRestorable { reason };
    let txn_id = evidence
        .restore
        .txn_id
        .as_deref()
        .ok_or_else(|| not_restorable("the evidence names no transaction".to_string()))?;
    if !is_valid_id(txn_id) {
        return Err(ChangeError::InvalidId {
            id: txn_id.to_string(),
        });
    }
    let dir = txn_dir(state_dir, txn_id);
    let journal_path = dir.join("journal");
    let parsed = journal::read(&journal_path)
        .map_err(io_ctx(format!("reading {}", journal_path.display())))?;
    if let Some(reason) = &parsed.damaged {
        return Err(not_restorable(format!("the journal is damaged ({reason})")));
    }
    let Some((_, proposal_id, ops)) = parsed.prepared() else {
        return Err(not_restorable("the journal has no plan".to_string()));
    };
    if proposal_id != evidence.proposal_id.as_str() {
        return Err(not_restorable(format!(
            "the transaction belongs to proposal {proposal_id}, not {}",
            evidence.proposal_id
        )));
    }
    if parsed.last() != Some(&Record::Committed) {
        return Err(not_restorable(
            "the transaction is not in the committed state (it was rolled back, already \
             restored, or is unfinished; run recover first)"
                .to_string(),
        ));
    }
    let mut report = RestoreReport {
        restored: false,
        refused: Vec::new(),
        undone: Vec::new(),
        conflicts: Vec::new(),
    };
    for op in ops {
        let found =
            disk_version(&op.path).map_err(io_ctx(format!("reading {}", op.path.display())))?;
        if !matches_version(found, &op.after) {
            let expected = match &op.after {
                Some(v) => format!("{} bytes with blake3 {}", v.len, v.hash),
                None => "no file".to_string(),
            };
            report.refused.push(RestoreRefusal {
                path: op.rel.clone(),
                detail: format!(
                    "changed since the change was applied: expected {expected}, found {}",
                    describe(found)
                ),
            });
            continue;
        }
        if let Some(before) = &op.before {
            let usable = parse_version(before)
                .and_then(|(hash, len)| read_verified(&before_copy(&dir, op.index), hash, len));
            if let Err(e) = usable {
                report.refused.push(RestoreRefusal {
                    path: op.rel.clone(),
                    detail: format!("the original copy is unusable: {e}"),
                });
            }
        }
    }
    if !report.refused.is_empty() {
        return Ok(report);
    }
    let mut journal = JournalWriter::open(&journal_path)
        .map_err(io_ctx(format!("opening {}", journal_path.display())))?;
    journal
        .append(&Record::RestoreStarted)
        .map_err(io_ctx(format!("writing {}", journal_path.display())))?;
    let indices: Vec<usize> = ops.iter().rev().map(|op| op.index).collect();
    let summary = undo_all(ops, &indices, txn_id, &dir, &mut journal, hook).map_err(|_| {
        ChangeError::Interrupted {
            txn_id: txn_id.to_string(),
        }
    })?;
    report.undone = undone_paths(ops, &summary);
    report.conflicts = summary.conflicts;
    report.restored = report.conflicts.is_empty();
    let record = if report.restored {
        Record::Restored
    } else {
        Record::RestoreIncomplete {
            conflicts: report.conflicts.iter().map(|c| c.path.clone()).collect(),
        }
    };
    journal
        .append(&record)
        .map_err(io_ctx(format!("writing {}", journal_path.display())))?;
    Ok(report)
}
