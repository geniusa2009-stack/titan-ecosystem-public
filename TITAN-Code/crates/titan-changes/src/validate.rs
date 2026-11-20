//! Validation and preview of a proposal against the current workspace.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use titan_doc::ContentHash;

use crate::diff::unified_diff;
use crate::model::{ByteEdit, ExpectedBase, FileChange, Proposal, normalize_rel, patch_id};
use crate::view::{Current, ViewError, WorkspaceView};

/// Category of a validation problem. Stable names (camelCase in JSON).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ProblemKind {
    /// Empty, absolute or `..` path, a symlink as the final component, or a parent that is
    /// not a directory.
    InvalidPath,
    /// The path resolves outside the workspace root (absolute path elsewhere or a symlink
    /// leading outside).
    OutsideRoot,
    /// The current content could not be read.
    Unreadable,
    /// The change is malformed (unparseable hash, edit of an absent file, rename onto
    /// itself, delete of an open buffer, ...).
    InvalidChange,
    /// The expected base does not match the current content (revision or hash differ, or
    /// the file is gone).
    Stale,
    /// The change expects the disk file, but the file is open in an editor buffer (which may
    /// hold unsaved edits); the change must be computed against the buffer.
    BufferIsOpen,
    /// The change expects an open buffer, but the path is not open.
    BufferNotOpen,
    /// The base content of an edit is not UTF-8 text.
    NotUtf8,
    /// An edit range is reversed or beyond the end of the content.
    EditOutOfBounds,
    /// An edit boundary falls inside a UTF-8 character.
    EditNotOnCharBoundary,
    /// Edits are not sorted by start offset.
    EditsUnsorted,
    /// Two edits overlap.
    EditsOverlap,
    /// An annotation id is not declared in the proposal.
    UnknownAnnotation,
    /// A create or rename target already exists.
    TargetExists,
    /// Two changes touch the same path, or one path lies inside another changed path.
    PathConflict,
}

/// One validation problem.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Problem {
    /// Workspace-relative path (as given when it could not be normalised).
    pub path: String,
    /// Index of the change in `Proposal::changes`.
    pub change_index: Option<usize>,
    pub kind: ProblemKind,
    pub detail: String,
}

/// Whether a change goes to a disk file or to an open editor buffer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Target {
    Disk,
    Buffer,
}

/// What a preview entry describes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum PreviewKind {
    Edit,
    Create,
    Delete,
    Rename,
}

/// Preview of one change.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FilePreview {
    pub path: String,
    pub change_index: usize,
    pub kind: PreviewKind,
    pub target: Target,
    /// Destination of a rename.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub renamed_to: Option<String>,
    /// BLAKE3 (hex) of the content before; `None` when there was no file.
    pub before_hash: Option<String>,
    /// BLAKE3 (hex) of the content after; `None` when the file is removed.
    pub after_hash: Option<String>,
    pub after_len: Option<u64>,
    /// Unified diff of the text with 3 lines of context, or a "file replaced" summary
    /// when `diff_truncated` is set.
    pub unified_diff: String,
    /// The diff exceeded the size caps and `unified_diff` is only a summary.
    pub diff_truncated: bool,
    /// Number of edits applied (edits only).
    pub edits_applied: usize,
}

/// An annotation that requires explicit approval, and the paths of the changes using it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ConfirmationNeeded {
    pub annotation_id: String,
    pub label: String,
    pub paths: Vec<String>,
}

/// Result of [`validate`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidationReport {
    /// No problems were found. Confirmation requirements do not affect it.
    pub ok: bool,
    /// [`patch_id`] of the proposal's changes.
    pub patch_id: String,
    pub problems: Vec<Problem>,
    /// One entry per change without problems, in change order.
    pub preview: Vec<FilePreview>,
    /// Annotations with `needsConfirmation` that the changes use; applying then requires
    /// the proposal status `approved`.
    pub needs_confirmation: Vec<ConfirmationNeeded>,
}

/// A change resolved against the current workspace (only used when validation passed).
#[derive(Clone, Debug)]
pub(crate) enum Resolved {
    DiskEdit {
        rel: String,
        abs: PathBuf,
        before: Vec<u8>,
        after: Vec<u8>,
    },
    DiskCreate {
        rel: String,
        abs: PathBuf,
        after: Vec<u8>,
    },
    DiskDelete {
        rel: String,
        abs: PathBuf,
        before: Vec<u8>,
    },
    DiskRename {
        from_rel: String,
        from_abs: PathBuf,
        to_rel: String,
        to_abs: PathBuf,
        bytes: Vec<u8>,
    },
    BufferEdit {
        rel: String,
        abs: PathBuf,
        revision: u64,
        edits: Vec<ByteEdit>,
        before_hash: ContentHash,
        after_hash: ContentHash,
    },
}

pub(crate) struct Analysis {
    pub report: ValidationReport,
    pub resolved: Vec<Resolved>,
}

/// Validate `proposal` against the workspace as `view` sees it and preview every change.
/// Reads files; never writes anything.
pub fn validate(proposal: &Proposal, view: &dyn WorkspaceView) -> ValidationReport {
    analyze(proposal, view).report
}

struct Ctx<'a> {
    view: &'a dyn WorkspaceView,
    problems: Vec<Problem>,
}

impl Ctx<'_> {
    fn problem(&mut self, path: &str, index: usize, kind: ProblemKind, detail: impl Into<String>) {
        self.problems.push(Problem {
            path: path.to_string(),
            change_index: Some(index),
            kind,
            detail: detail.into(),
        });
    }

    /// Normalise and resolve a path; record a problem and return `None` on failure.
    fn locate(&mut self, raw: &str, index: usize) -> Option<(String, PathBuf)> {
        let rel = match normalize_rel(raw) {
            Ok(rel) => rel,
            Err(reason) => {
                let kind = if Path::new(raw).is_absolute() {
                    ProblemKind::OutsideRoot
                } else {
                    ProblemKind::InvalidPath
                };
                self.problem(raw, index, kind, reason);
                return None;
            }
        };
        match self.view.resolve(Path::new(&rel)) {
            Ok(abs) => Some((rel, abs)),
            Err(e) => {
                let kind = match &e {
                    ViewError::InvalidPath { .. } => ProblemKind::InvalidPath,
                    ViewError::OutsideRoot { .. } => ProblemKind::OutsideRoot,
                    ViewError::Unreadable { .. } | ViewError::Io { .. } => ProblemKind::Unreadable,
                };
                self.problem(&rel, index, kind, e.to_string());
                None
            }
        }
    }

    fn current(&mut self, rel: &str, abs: &Path, index: usize) -> Option<Current> {
        match self.view.current(abs) {
            Ok(current) => Some(current),
            Err(e) => {
                self.problem(rel, index, ProblemKind::Unreadable, e.to_string());
                None
            }
        }
    }

    /// Check a disk expectation against disk content. `Some(())` when it matches.
    fn check_disk(
        &mut self,
        rel: &str,
        index: usize,
        expected_hash: &str,
        expected_len: u64,
        bytes: &[u8],
        hash: ContentHash,
    ) -> Option<()> {
        let Some(expected) = ContentHash::from_hex(expected_hash) else {
            self.problem(
                rel,
                index,
                ProblemKind::InvalidChange,
                format!("expected hash {expected_hash:?} is not 64 hex digits"),
            );
            return None;
        };
        if expected != hash || expected_len != bytes.len() as u64 {
            self.problem(
                rel,
                index,
                ProblemKind::Stale,
                format!(
                    "the file changed: expected {expected_len} bytes with blake3 {expected}, \
                     found {} bytes with blake3 {hash}",
                    bytes.len()
                ),
            );
            return None;
        }
        Some(())
    }
}

fn hex(hash: ContentHash) -> String {
    hash.to_hex()
}

fn check_edits(ctx: &mut Ctx<'_>, rel: &str, index: usize, text: &str, edits: &[ByteEdit]) -> bool {
    let before = ctx.problems.len();
    let mut prev: Option<(usize, usize)> = None;
    for (j, edit) in edits.iter().enumerate() {
        if edit.start > edit.end || edit.end > text.len() {
            ctx.problem(
                rel,
                index,
                ProblemKind::EditOutOfBounds,
                format!(
                    "edit {j}: range {}..{} is not within 0..{}",
                    edit.start,
                    edit.end,
                    text.len()
                ),
            );
            continue;
        }
        if !text.is_char_boundary(edit.start) || !text.is_char_boundary(edit.end) {
            ctx.problem(
                rel,
                index,
                ProblemKind::EditNotOnCharBoundary,
                format!(
                    "edit {j}: range {}..{} splits a UTF-8 character",
                    edit.start, edit.end
                ),
            );
            continue;
        }
        if let Some((prev_start, prev_end)) = prev {
            if edit.start < prev_start {
                ctx.problem(
                    rel,
                    index,
                    ProblemKind::EditsUnsorted,
                    format!(
                        "edit {j} starts at {} before the previous edit's start {prev_start}",
                        edit.start
                    ),
                );
            } else if edit.start < prev_end {
                ctx.problem(
                    rel,
                    index,
                    ProblemKind::EditsOverlap,
                    format!(
                        "edit {j} ({}..{}) overlaps the previous edit ({prev_start}..{prev_end})",
                        edit.start, edit.end
                    ),
                );
            }
        }
        prev = Some((edit.start, edit.end));
    }
    ctx.problems.len() == before
}

/// Apply validated edits. `None` only if a range is invalid (validation prevents that).
pub(crate) fn apply_edits(text: &str, edits: &[ByteEdit]) -> Option<String> {
    let inserted: usize = edits.iter().map(|e| e.text.len()).sum();
    let mut out = String::with_capacity(text.len() + inserted);
    let mut pos = 0;
    for edit in edits {
        out.push_str(text.get(pos..edit.start)?);
        out.push_str(&edit.text);
        pos = edit.end;
    }
    out.push_str(text.get(pos..)?);
    Some(out)
}

fn text_diff(old: &str, new: &str, old_label: &str, new_label: &str) -> (String, bool) {
    match unified_diff(old, new, old_label, new_label) {
        Some(diff) => (diff, false),
        None => (
            format!(
                "file replaced: {} lines -> {} lines (blake3 {} -> {}); the diff is too large \
                 to display\n",
                old.split_inclusive('\n').count(),
                new.split_inclusive('\n').count(),
                ContentHash::of(old.as_bytes()),
                ContentHash::of(new.as_bytes())
            ),
            true,
        ),
    }
}

pub(crate) fn analyze(proposal: &Proposal, view: &dyn WorkspaceView) -> Analysis {
    let mut ctx = Ctx {
        view,
        problems: Vec::new(),
    };
    let annotations: HashMap<&str, _> = proposal
        .annotations
        .iter()
        .map(|a| (a.id.as_str(), a))
        .collect();
    let mut preview = Vec::new();
    let mut resolved = Vec::new();
    // (absolute path, change index, relative path) of everything touched.
    let mut touched: Vec<(PathBuf, usize, String)> = Vec::new();

    for (index, change) in proposal.changes.iter().enumerate() {
        let problems_before = ctx.problems.len();
        for id in change.annotation_ids() {
            if !annotations.contains_key(id) {
                ctx.problem(
                    change.path(),
                    index,
                    ProblemKind::UnknownAnnotation,
                    format!("annotation {id:?} is not declared in the proposal"),
                );
            }
        }
        match change {
            FileChange::Edit {
                path,
                expected,
                edits,
            } => {
                let Some((rel, abs)) = ctx.locate(path, index) else {
                    continue;
                };
                touched.push((abs.clone(), index, rel.clone()));
                let Some(current) = ctx.current(&rel, &abs, index) else {
                    continue;
                };
                let base = match (expected, current) {
                    (ExpectedBase::Absent, _) => {
                        ctx.problem(
                            &rel,
                            index,
                            ProblemKind::InvalidChange,
                            "an edit needs an existing file or buffer; use `create` for new files",
                        );
                        None
                    }
                    (_, Current::Missing) => {
                        ctx.problem(&rel, index, ProblemKind::Stale, "the file does not exist");
                        None
                    }
                    (ExpectedBase::Disk { .. }, Current::OpenBuffer { revision, .. }) => {
                        ctx.problem(
                            &rel,
                            index,
                            ProblemKind::BufferIsOpen,
                            format!(
                                "the file is open in an editor buffer (revision {revision}) that \
                                 may hold unsaved edits; compute the change against the buffer"
                            ),
                        );
                        None
                    }
                    (ExpectedBase::Buffer { revision, .. }, Current::Disk { .. }) => {
                        ctx.problem(
                            &rel,
                            index,
                            ProblemKind::BufferNotOpen,
                            format!(
                                "the change was computed against buffer revision {revision}, but \
                                 the file is not open"
                            ),
                        );
                        None
                    }
                    (
                        ExpectedBase::Disk { hash, len },
                        Current::Disk {
                            bytes,
                            hash: actual,
                        },
                    ) => ctx
                        .check_disk(&rel, index, hash, *len, &bytes, actual)
                        .map(|()| (bytes, None)),
                    (
                        ExpectedBase::Buffer { revision, hash },
                        Current::OpenBuffer {
                            revision: actual_revision,
                            text,
                        },
                    ) => {
                        let actual = ContentHash::of(text.as_bytes());
                        match ContentHash::from_hex(hash) {
                            None => {
                                ctx.problem(
                                    &rel,
                                    index,
                                    ProblemKind::InvalidChange,
                                    format!("expected hash {hash:?} is not 64 hex digits"),
                                );
                                None
                            }
                            Some(_) if *revision != actual_revision => {
                                ctx.problem(
                                    &rel,
                                    index,
                                    ProblemKind::Stale,
                                    format!(
                                        "the buffer is at revision {actual_revision}, the change \
                                         was computed against revision {revision}"
                                    ),
                                );
                                None
                            }
                            Some(expected) if expected != actual => {
                                ctx.problem(
                                    &rel,
                                    index,
                                    ProblemKind::Stale,
                                    format!(
                                        "the buffer text has blake3 {actual}, expected {expected}"
                                    ),
                                );
                                None
                            }
                            Some(_) => Some((text.into_bytes(), Some(actual_revision))),
                        }
                    }
                };
                let Some((bytes, buffer_revision)) = base else {
                    continue;
                };
                let text = match String::from_utf8(bytes) {
                    Ok(text) => text,
                    Err(e) => {
                        ctx.problem(
                            &rel,
                            index,
                            ProblemKind::NotUtf8,
                            format!(
                                "the file is not UTF-8 text (invalid byte at offset {})",
                                e.utf8_error().valid_up_to()
                            ),
                        );
                        continue;
                    }
                };
                if !check_edits(&mut ctx, &rel, index, &text, edits)
                    || ctx.problems.len() != problems_before
                {
                    continue;
                }
                let Some(after) = apply_edits(&text, edits) else {
                    ctx.problem(
                        &rel,
                        index,
                        ProblemKind::EditOutOfBounds,
                        "edits do not apply",
                    );
                    continue;
                };
                let before_hash = ContentHash::of(text.as_bytes());
                let after_hash = ContentHash::of(after.as_bytes());
                let (unified, truncated) =
                    text_diff(&text, &after, &format!("a/{rel}"), &format!("b/{rel}"));
                preview.push(FilePreview {
                    path: rel.clone(),
                    change_index: index,
                    kind: PreviewKind::Edit,
                    target: if buffer_revision.is_some() {
                        Target::Buffer
                    } else {
                        Target::Disk
                    },
                    renamed_to: None,
                    before_hash: Some(hex(before_hash)),
                    after_hash: Some(hex(after_hash)),
                    after_len: Some(after.len() as u64),
                    unified_diff: unified,
                    diff_truncated: truncated,
                    edits_applied: edits.len(),
                });
                match buffer_revision {
                    Some(revision) => resolved.push(Resolved::BufferEdit {
                        rel,
                        abs,
                        revision,
                        edits: edits.clone(),
                        before_hash,
                        after_hash,
                    }),
                    None => resolved.push(Resolved::DiskEdit {
                        rel,
                        abs,
                        before: text.into_bytes(),
                        after: after.into_bytes(),
                    }),
                }
            }
            FileChange::Create { path, content, .. } => {
                let Some((rel, abs)) = ctx.locate(path, index) else {
                    continue;
                };
                touched.push((abs.clone(), index, rel.clone()));
                let Some(current) = ctx.current(&rel, &abs, index) else {
                    continue;
                };
                if !matches!(current, Current::Missing) {
                    let what = if matches!(current, Current::OpenBuffer { .. }) {
                        "an open buffer"
                    } else {
                        "a file"
                    };
                    ctx.problem(
                        &rel,
                        index,
                        ProblemKind::TargetExists,
                        format!("{what} already exists at this path; create never overwrites"),
                    );
                    continue;
                }
                if ctx.problems.len() != problems_before {
                    continue;
                }
                let (unified, truncated) = text_diff("", content, "/dev/null", &format!("b/{rel}"));
                preview.push(FilePreview {
                    path: rel.clone(),
                    change_index: index,
                    kind: PreviewKind::Create,
                    target: Target::Disk,
                    renamed_to: None,
                    before_hash: None,
                    after_hash: Some(hex(ContentHash::of(content.as_bytes()))),
                    after_len: Some(content.len() as u64),
                    unified_diff: unified,
                    diff_truncated: truncated,
                    edits_applied: 0,
                });
                resolved.push(Resolved::DiskCreate {
                    rel,
                    abs,
                    after: content.clone().into_bytes(),
                });
            }
            FileChange::Delete { path, expected, .. } => {
                let Some((rel, abs)) = ctx.locate(path, index) else {
                    continue;
                };
                touched.push((abs.clone(), index, rel.clone()));
                let Some(bytes) = disk_source(&mut ctx, &rel, &abs, index, expected, "delete")
                else {
                    continue;
                };
                if ctx.problems.len() != problems_before {
                    continue;
                }
                let (unified, truncated) = match std::str::from_utf8(&bytes) {
                    Ok(text) => text_diff(text, "", &format!("a/{rel}"), "/dev/null"),
                    Err(_) => (
                        format!("binary file a/{rel} deleted ({} bytes)\n", bytes.len()),
                        false,
                    ),
                };
                preview.push(FilePreview {
                    path: rel.clone(),
                    change_index: index,
                    kind: PreviewKind::Delete,
                    target: Target::Disk,
                    renamed_to: None,
                    before_hash: Some(hex(ContentHash::of(&bytes))),
                    after_hash: None,
                    after_len: None,
                    unified_diff: unified,
                    diff_truncated: truncated,
                    edits_applied: 0,
                });
                resolved.push(Resolved::DiskDelete {
                    rel,
                    abs,
                    before: bytes,
                });
            }
            FileChange::Rename {
                from, to, expected, ..
            } => {
                let source = ctx.locate(from, index);
                let dest = ctx.locate(to, index);
                let (Some((from_rel, from_abs)), Some((to_rel, to_abs))) = (source, dest) else {
                    continue;
                };
                if from_abs == to_abs {
                    ctx.problem(
                        &from_rel,
                        index,
                        ProblemKind::InvalidChange,
                        "a rename needs different source and destination paths",
                    );
                    continue;
                }
                touched.push((from_abs.clone(), index, from_rel.clone()));
                touched.push((to_abs.clone(), index, to_rel.clone()));
                let bytes = disk_source(&mut ctx, &from_rel, &from_abs, index, expected, "rename");
                let dest_current = ctx.current(&to_rel, &to_abs, index);
                if dest_current.is_some_and(|c| !matches!(c, Current::Missing)) {
                    ctx.problem(
                        &to_rel,
                        index,
                        ProblemKind::TargetExists,
                        "the rename destination already exists; rename never overwrites",
                    );
                }
                let Some(bytes) = bytes else {
                    continue;
                };
                if ctx.problems.len() != problems_before {
                    continue;
                }
                let hash = hex(ContentHash::of(&bytes));
                preview.push(FilePreview {
                    path: from_rel.clone(),
                    change_index: index,
                    kind: PreviewKind::Rename,
                    target: Target::Disk,
                    renamed_to: Some(to_rel.clone()),
                    before_hash: Some(hash.clone()),
                    after_hash: Some(hash),
                    after_len: Some(bytes.len() as u64),
                    unified_diff: format!("rename from {from_rel}\nrename to {to_rel}\n"),
                    diff_truncated: false,
                    edits_applied: 0,
                });
                resolved.push(Resolved::DiskRename {
                    from_rel,
                    from_abs,
                    to_rel,
                    to_abs,
                    bytes,
                });
            }
        }
    }

    // The same path (or a path and one of its ancestors) touched by two changes.
    touched.sort();
    for pair in touched.windows(2) {
        let (a, a_index, a_rel) = &pair[0];
        let (b, b_index, b_rel) = &pair[1];
        if a_index == b_index {
            continue;
        }
        if a == b {
            ctx.problem(
                b_rel,
                *b_index,
                ProblemKind::PathConflict,
                format!("change {a_index} also changes this path"),
            );
        } else if b.starts_with(a) {
            ctx.problem(
                b_rel,
                *b_index,
                ProblemKind::PathConflict,
                format!("the path is inside {a_rel:?}, which change {a_index} also changes"),
            );
        }
    }

    let mut needs_confirmation = Vec::new();
    for annotation in proposal.annotations.iter().filter(|a| a.needs_confirmation) {
        let paths: Vec<String> = proposal
            .changes
            .iter()
            .filter(|c| c.annotation_ids().contains(&annotation.id.as_str()))
            .map(|c| normalize_rel(c.path()).unwrap_or_else(|_| c.path().to_string()))
            .collect();
        if !paths.is_empty() {
            needs_confirmation.push(ConfirmationNeeded {
                annotation_id: annotation.id.clone(),
                label: annotation.label.clone(),
                paths,
            });
        }
    }

    let ok = ctx.problems.is_empty();
    Analysis {
        report: ValidationReport {
            ok,
            patch_id: patch_id(&proposal.changes),
            problems: ctx.problems,
            preview,
            needs_confirmation,
        },
        resolved: if ok { resolved } else { Vec::new() },
    }
}

/// The verified disk bytes of a delete or rename source.
fn disk_source(
    ctx: &mut Ctx<'_>,
    rel: &str,
    abs: &Path,
    index: usize,
    expected: &ExpectedBase,
    what: &str,
) -> Option<Vec<u8>> {
    let (hash, len) = match expected {
        ExpectedBase::Disk { hash, len } => (hash, *len),
        ExpectedBase::Buffer { .. } => {
            ctx.problem(
                rel,
                index,
                ProblemKind::InvalidChange,
                format!(
                    "a {what} acts on the disk file; save or close the buffer and expect the \
                     disk version"
                ),
            );
            return None;
        }
        ExpectedBase::Absent => {
            ctx.problem(
                rel,
                index,
                ProblemKind::InvalidChange,
                format!("a {what} needs the expected content of an existing file"),
            );
            return None;
        }
    };
    match ctx.current(rel, abs, index)? {
        Current::Missing => {
            ctx.problem(rel, index, ProblemKind::Stale, "the file does not exist");
            None
        }
        Current::OpenBuffer { revision, .. } => {
            ctx.problem(
                rel,
                index,
                ProblemKind::BufferIsOpen,
                format!(
                    "the file is open in an editor buffer (revision {revision}); close it before \
                     the {what}"
                ),
            );
            None
        }
        Current::Disk {
            bytes,
            hash: actual,
        } => {
            ctx.check_disk(rel, index, hash, len, &bytes, actual)?;
            Some(bytes)
        }
    }
}
