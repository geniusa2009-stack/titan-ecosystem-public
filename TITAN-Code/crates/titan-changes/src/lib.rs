//! TITAN Code verified changes.
//!
//! One model for every multi-file change, whoever produces it: a developer command, an LSP
//! refactoring (for example a rust-analyzer rename `WorkspaceEdit`) or an agent. No AI
//! provider, network or language server is needed.
//!
//! 1. **Propose**: a [`Proposal`] lists [`FileChange`]s (edit, create, delete, rename),
//!    each naming the exact base it was computed against ([`ExpectedBase`]: an open buffer
//!    revision + hash, a disk hash + length, or "absent").
//! 2. **Validate and preview**: [`validate`] checks paths (confined to the workspace, no
//!    `..`, no symlink escapes), bases (stale revisions and hashes are refused), edit ranges,
//!    targets of creates and renames, conflicting changes to one path, and unsaved buffers;
//!    it returns [`Problem`]s and a [`FilePreview`] with a unified diff per change.
//!    [`patch_id`] identifies the change set independently of who proposed it and when.
//! 3. **Apply**: [`apply`] stages every original file in a transaction directory, journals
//!    the plan, writes the disk files one by one with conflict-checked replacement, and on
//!    any failure puts back what it wrote. **Multi-file changes are not atomic**; see
//!    [`apply`](mod@apply) for what is guaranteed instead. Changes to open buffers are
//!    returned as [`BufferEdits`] for the caller to apply in memory.
//! 4. **Recover / restore**: [`recover`] finishes transactions interrupted by a crash;
//!    [`restore`] puts back the originals of an applied change if nothing changed since.
//! 5. **Record**: an [`EvidenceRecord`] stores what was changed, the validation, the checks
//!    that ran (and those that did not, with the reason), the approval and the outcome.
//!
//! The crate never mutates editor buffers, never runs checks itself, and never decides
//! that a change is correct; it records what was observed.

pub mod apply;
pub mod diff;
pub mod evidence;
mod journal;
pub mod model;
mod util;
pub mod validate;
pub mod view;

pub use apply::{
    ApplyError, ApplyPoint, ApplyReport, BufferEdits, CHANGES_DIR, ChangeError, HookAction,
    OpReport, OpState, Outcome, RecoveredTxn, RecoveryAction, RecoveryReport, RestoreRefusal,
    RestoreReport, RollbackConflict, apply, apply_with_hook, recover, restore, restore_with_hook,
};
pub use diff::unified_diff;
pub use evidence::{
    AffectedFile, CheckRecord, CheckStatus, EVIDENCE_FORMAT_VERSION, EvidenceListing,
    EvidenceRecord, RestoreInfo, ValidationSummary, list_evidence, load_evidence, write_evidence,
};
pub use journal::OpKind;
pub use model::{
    Approval, BaseSnapshot, ByteEdit, ChangeAnnotation, ExpectedBase, FileChange, Proposal,
    ProposalId, ProposalSource, ProposalStatus, is_valid_id, normalize_rel, patch_id,
};
pub use titan_doc::SavePhase;
pub use validate::{
    ConfirmationNeeded, FilePreview, PreviewKind, Problem, ProblemKind, Target, ValidationReport,
    validate,
};
pub use view::{Current, DiskView, ViewError, WorkspaceView, resolve_confined};
