//! Errors of the verification pipeline.
//!
//! A [`VerifyError`] means the pipeline itself could not do its job (invalid plan, missing
//! tool, git failure, unsafe patch path). A check that ran and failed, timed out or was not
//! allowed to run is *not* an error: it is recorded in the report as a
//! [`crate::CheckStatus`].

use std::io;
use std::path::PathBuf;

/// Why a verification could not be carried out.
#[derive(Debug, thiserror::Error)]
pub enum VerifyError {
    /// A plan argument (package name, test filter, revision, timeout) is not acceptable.
    #[error("invalid {what} {value:?}: {reason}")]
    InvalidArgument {
        /// Which argument (`"package name"`, `"test filter"`, `"revision"`, ...).
        what: &'static str,
        /// The rejected value (as given).
        value: String,
        /// Why it was rejected.
        reason: String,
    },
    /// A candidate file path is not a safe path inside the worktree.
    #[error("invalid candidate path {path:?}: {reason}")]
    InvalidPath {
        /// The rejected path (as given).
        path: String,
        /// Why it was rejected.
        reason: String,
    },
    /// A required program (`git`, `cargo`) was not found.
    #[error("program `{tool}` not found ({detail})")]
    ToolNotFound {
        /// The program name.
        tool: &'static str,
        /// Where it was looked for.
        detail: String,
    },
    /// The workspace path is not inside a git repository.
    #[error("{path} is not a git repository: {message}")]
    NotARepository {
        /// The path that was opened.
        path: PathBuf,
        /// What git said.
        message: String,
    },
    /// The base revision does not name a commit.
    #[error("revision {rev:?} does not name a commit: {message}")]
    RevisionNotFound {
        /// The requested revision.
        rev: String,
        /// What git said.
        message: String,
    },
    /// A controlled git invocation failed.
    #[error("git {action} failed: {message}")]
    Git {
        /// What was attempted (`"worktree add"`, ...).
        action: &'static str,
        /// Exit status and the end of git's output.
        message: String,
    },
    /// A file-system operation failed.
    #[error("{context}: {source}")]
    Io {
        /// What was being done.
        context: String,
        /// The underlying error.
        #[source]
        source: io::Error,
    },
}

impl VerifyError {
    pub(crate) fn io(context: impl Into<String>, source: io::Error) -> Self {
        Self::Io {
            context: context.into(),
            source,
        }
    }

    pub(crate) fn path(path: &str, reason: impl Into<String>) -> Self {
        Self::InvalidPath {
            path: path.to_string(),
            reason: reason.into(),
        }
    }
}
