//! Candidate patches: file operations applied to the candidate worktree.
//!
//! Paths are relative, `/`-separated and validated twice: lexically before anything is
//! created (no absolute paths, no `.`/`..`, no empty components, no `\`, no NUL, no `.git`
//! component in any letter case) and against the worktree while applying (no existing
//! component may be a symbolic link, and a written file may not replace a symbolic link).
//! The worktree is private to one run, so nothing else changes it between check and use.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::error::VerifyError;

/// Longest accepted candidate path, in bytes.
pub const MAX_PATH_LEN: usize = 4096;

/// One file operation of a candidate change. Paths are relative to the repository root.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum FileOp {
    /// Create or replace a file with this UTF-8 content (parent directories are created).
    Write {
        /// Relative path.
        path: String,
        /// The full new content.
        content: String,
    },
    /// Delete a file.
    Delete {
        /// Relative path.
        path: String,
    },
    /// Rename (move) a file; an existing regular file at `to` is replaced.
    Rename {
        /// Current relative path.
        from: String,
        /// New relative path.
        to: String,
    },
}

impl FileOp {
    /// Lexical validation of every path of this operation.
    ///
    /// # Errors
    /// [`VerifyError::InvalidPath`] with the reason.
    pub fn validate(&self) -> Result<(), VerifyError> {
        match self {
            Self::Write { path, .. } | Self::Delete { path } => validate_relative_path(path),
            Self::Rename { from, to } => {
                validate_relative_path(from)?;
                validate_relative_path(to)
            }
        }
    }
}

/// What applying one operation did, with the BLAKE3 hash of every written file.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AppliedChange {
    /// `"write"`, `"delete"` or `"rename"`.
    pub op: String,
    /// The path written or deleted (the destination of a rename).
    pub path: String,
    /// The source of a rename.
    pub from: Option<String>,
    /// BLAKE3 (hex) of the file content after the operation; `None` for a delete.
    pub blake3: Option<String>,
    /// Size in bytes after the operation; `None` for a delete.
    pub bytes: Option<u64>,
}

/// Lexically validates a candidate path.
///
/// # Errors
/// [`VerifyError::InvalidPath`] when the path is empty, too long, absolute, contains
/// `\` or NUL, has an empty, `.` or `..` component, or a `.git` component.
pub fn validate_relative_path(path: &str) -> Result<(), VerifyError> {
    if path.is_empty() {
        return Err(VerifyError::path(path, "empty path"));
    }
    if path.len() > MAX_PATH_LEN {
        return Err(VerifyError::path(path, "path is too long"));
    }
    if path.contains('\0') {
        return Err(VerifyError::path(path, "path contains NUL"));
    }
    if path.contains('\\') {
        return Err(VerifyError::path(path, "path contains '\\'"));
    }
    if path.starts_with('/') || Path::new(path).is_absolute() || has_drive_prefix(path) {
        return Err(VerifyError::path(path, "absolute path"));
    }
    for component in path.split('/') {
        match component {
            "" => return Err(VerifyError::path(path, "empty path component")),
            "." | ".." => {
                return Err(VerifyError::path(path, "'.' or '..' component"));
            }
            c if c.eq_ignore_ascii_case(".git") => {
                return Err(VerifyError::path(path, "'.git' component"));
            }
            _ => {}
        }
    }
    Ok(())
}

fn field(hasher: &mut blake3::Hasher, bytes: &[u8]) {
    hasher.update(&(bytes.len() as u64).to_le_bytes());
    hasher.update(bytes);
}

fn has_drive_prefix(path: &str) -> bool {
    let b = path.as_bytes();
    b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':'
}

/// Stable identifier of a candidate: BLAKE3 over a canonical encoding of the operations.
#[must_use]
pub fn patch_id(ops: &[FileOp]) -> String {
    let mut hasher = blake3::Hasher::new();
    hasher.update(b"titan-verify candidate patch v1\0");
    for op in ops {
        match op {
            FileOp::Write { path, content } => {
                field(&mut hasher, b"write");
                field(&mut hasher, path.as_bytes());
                field(&mut hasher, content.as_bytes());
            }
            FileOp::Delete { path } => {
                field(&mut hasher, b"delete");
                field(&mut hasher, path.as_bytes());
            }
            FileOp::Rename { from, to } => {
                field(&mut hasher, b"rename");
                field(&mut hasher, from.as_bytes());
                field(&mut hasher, to.as_bytes());
            }
        }
    }
    hasher.finalize().to_hex().to_string()
}

/// Applies `ops` in order to the worktree at `root`.
///
/// # Errors
/// [`VerifyError::InvalidPath`] for an unsafe path (checked before each operation touches
/// the file system), [`VerifyError::Io`] when an operation fails. Operations before the
/// failing one stay applied; the caller discards the worktree.
pub fn apply(root: &Path, ops: &[FileOp]) -> Result<Vec<AppliedChange>, VerifyError> {
    for op in ops {
        op.validate()?;
    }
    let mut applied = Vec::with_capacity(ops.len());
    for op in ops {
        applied.push(apply_one(root, op)?);
    }
    Ok(applied)
}

fn apply_one(root: &Path, op: &FileOp) -> Result<AppliedChange, VerifyError> {
    match op {
        FileOp::Write { path, content } => {
            let target = prepare_parent(root, path, true)?;
            refuse_symlink_target(&target, path)?;
            fs::write(&target, content.as_bytes())
                .map_err(|e| VerifyError::io(format!("cannot write {path}"), e))?;
            Ok(AppliedChange {
                op: "write".into(),
                path: path.clone(),
                from: None,
                blake3: Some(blake3::hash(content.as_bytes()).to_hex().to_string()),
                bytes: Some(content.len() as u64),
            })
        }
        FileOp::Delete { path } => {
            let target = prepare_parent(root, path, false)?;
            let meta = fs::symlink_metadata(&target)
                .map_err(|e| VerifyError::io(format!("cannot delete {path}"), e))?;
            if meta.is_dir() {
                return Err(VerifyError::path(path, "is a directory"));
            }
            // Removes a symbolic link itself, never its target.
            fs::remove_file(&target)
                .map_err(|e| VerifyError::io(format!("cannot delete {path}"), e))?;
            Ok(AppliedChange {
                op: "delete".into(),
                path: path.clone(),
                from: None,
                blake3: None,
                bytes: None,
            })
        }
        FileOp::Rename { from, to } => {
            let source = prepare_parent(root, from, false)?;
            let meta = fs::symlink_metadata(&source)
                .map_err(|e| VerifyError::io(format!("cannot rename {from}"), e))?;
            if !meta.is_file() {
                return Err(VerifyError::path(from, "is not a regular file"));
            }
            let target = prepare_parent(root, to, true)?;
            refuse_symlink_target(&target, to)?;
            if target.is_dir() {
                return Err(VerifyError::path(to, "is a directory"));
            }
            fs::rename(&source, &target)
                .map_err(|e| VerifyError::io(format!("cannot rename {from} to {to}"), e))?;
            let content =
                fs::read(&target).map_err(|e| VerifyError::io(format!("cannot read {to}"), e))?;
            Ok(AppliedChange {
                op: "rename".into(),
                path: to.clone(),
                from: Some(from.clone()),
                blake3: Some(blake3::hash(&content).to_hex().to_string()),
                bytes: Some(content.len() as u64),
            })
        }
    }
}

/// Walks the parent directories of `rel` below `root`, refusing any symbolic link, and
/// creates missing ones when `create` is set. Returns the full target path.
fn prepare_parent(root: &Path, rel: &str, create: bool) -> Result<PathBuf, VerifyError> {
    let mut components: Vec<&str> = rel.split('/').collect();
    let Some(file_name) = components.pop() else {
        return Err(VerifyError::path(rel, "empty path"));
    };
    let mut dir = root.to_path_buf();
    for component in components {
        dir.push(component);
        match fs::symlink_metadata(&dir) {
            Ok(meta) if meta.file_type().is_symlink() => {
                return Err(VerifyError::path(
                    rel,
                    format!("'{component}' is a symbolic link (could escape the worktree)"),
                ));
            }
            Ok(meta) if meta.is_dir() => {}
            Ok(_) => {
                return Err(VerifyError::path(
                    rel,
                    format!("'{component}' is not a directory"),
                ));
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound && create => {
                fs::create_dir(&dir).map_err(|e| {
                    VerifyError::io(format!("cannot create directory for {rel}"), e)
                })?;
            }
            Err(e) => {
                return Err(VerifyError::io(format!("cannot inspect {rel}"), e));
            }
        }
    }
    Ok(dir.join(file_name))
}

fn refuse_symlink_target(target: &Path, rel: &str) -> Result<(), VerifyError> {
    match fs::symlink_metadata(target) {
        Ok(meta) if meta.file_type().is_symlink() => Err(VerifyError::path(
            rel,
            "target is a symbolic link (writing would follow it)",
        )),
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsafe_paths_are_rejected_lexically() {
        for bad in [
            "",
            "../x",
            "a/../../x",
            "a/..",
            "./a",
            "/etc/passwd",
            "//x",
            "a//b",
            "a/",
            "C:/x",
            "a\\..\\b",
            ".git/config",
            "sub/.GIT/hooks/pre-commit",
            ".Git",
            "a\0b",
        ] {
            assert!(validate_relative_path(bad).is_err(), "{bad:?} was accepted");
        }
        for good in [
            "src/lib.rs",
            "Cargo.toml",
            ".github/ci.yml",
            "a/b/c.txt",
            ".gitignore",
        ] {
            assert!(
                validate_relative_path(good).is_ok(),
                "{good:?} was rejected"
            );
        }
        assert!(validate_relative_path(&"a/".repeat(3000)).is_err());
    }

    #[test]
    fn writes_deletes_and_renames_with_hashes() {
        let dir = tempfile::tempdir().unwrap_or_else(|e| panic!("{e}"));
        let root = dir.path();
        let ops = vec![
            FileOp::Write {
                path: "src/new/a.txt".into(),
                content: "alpha".into(),
            },
            FileOp::Write {
                path: "b.txt".into(),
                content: "beta".into(),
            },
            FileOp::Rename {
                from: "b.txt".into(),
                to: "src/c.txt".into(),
            },
            FileOp::Delete {
                path: "src/new/a.txt".into(),
            },
        ];
        let applied = apply(root, &ops).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(applied.len(), 4);
        assert_eq!(
            applied[0].blake3.as_deref(),
            Some(blake3::hash(b"alpha").to_hex().as_str())
        );
        assert_eq!(applied[2].from.as_deref(), Some("b.txt"));
        assert_eq!(
            applied[2].blake3.as_deref(),
            Some(blake3::hash(b"beta").to_hex().as_str())
        );
        assert!(!root.join("src/new/a.txt").exists());
        assert!(!root.join("b.txt").exists());
        assert_eq!(
            fs::read_to_string(root.join("src/c.txt")).ok().as_deref(),
            Some("beta")
        );
        assert_ne!(patch_id(&ops), patch_id(&ops[..3]));
        assert_eq!(patch_id(&ops), patch_id(&ops.clone()));
    }

    #[cfg(unix)]
    #[test]
    fn symbolic_links_cannot_be_used_to_escape() {
        let outside = tempfile::tempdir().unwrap_or_else(|e| panic!("{e}"));
        let dir = tempfile::tempdir().unwrap_or_else(|e| panic!("{e}"));
        let root = dir.path();
        std::os::unix::fs::symlink(outside.path(), root.join("escape"))
            .unwrap_or_else(|e| panic!("{e}"));
        std::os::unix::fs::symlink(outside.path().join("f"), root.join("file-link"))
            .unwrap_or_else(|e| panic!("{e}"));
        for op in [
            FileOp::Write {
                path: "escape/pwned".into(),
                content: "x".into(),
            },
            FileOp::Write {
                path: "file-link".into(),
                content: "x".into(),
            },
            FileOp::Rename {
                from: "escape/f".into(),
                to: "g".into(),
            },
        ] {
            let result = apply(root, std::slice::from_ref(&op));
            assert!(
                matches!(result, Err(VerifyError::InvalidPath { .. })),
                "{op:?}: {result:?}"
            );
        }
        assert!(!outside.path().join("pwned").exists());
        assert!(!outside.path().join("f").exists());
        // Deleting the link removes the link only.
        fs::write(outside.path().join("keep"), "k").unwrap_or_else(|e| panic!("{e}"));
        std::os::unix::fs::symlink(outside.path().join("keep"), root.join("keep-link"))
            .unwrap_or_else(|e| panic!("{e}"));
        apply(
            root,
            &[FileOp::Delete {
                path: "keep-link".into(),
            }],
        )
        .unwrap_or_else(|e| panic!("{e}"));
        assert!(outside.path().join("keep").exists());
    }
}
