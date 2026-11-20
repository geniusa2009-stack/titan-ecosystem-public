//! How validation sees the workspace: disk files and, through the caller, open buffers.

use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

use titan_doc::{ContentHash, Limits, read_disk};

/// Why a [`WorkspaceView`] could not resolve or read a path.
#[derive(Debug, thiserror::Error)]
pub enum ViewError {
    /// The path is not an acceptable workspace-relative path.
    #[error("{path:?} is not a valid workspace path: {reason}")]
    InvalidPath { path: PathBuf, reason: String },
    /// The path resolves outside the workspace root (absolute path, `..`, or a symlink
    /// pointing outside).
    #[error("{path:?} resolves outside the workspace")]
    OutsideRoot { path: PathBuf },
    /// The content could not be read (I/O error, too large, ...).
    #[error("{path:?} could not be read: {reason}")]
    Unreadable { path: PathBuf, reason: String },
    /// An I/O error while resolving.
    #[error("I/O error on {path:?}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
}

/// What is currently at a path, as the editor sees it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Current {
    /// The file is open in an editor buffer. `text` is the buffer text (it may have unsaved
    /// changes). Changes to it are returned as [`crate::BufferEdits`] and never written to
    /// disk by this crate.
    OpenBuffer { revision: u64, text: String },
    /// A file on disk, not open in a buffer.
    Disk { bytes: Vec<u8>, hash: ContentHash },
    /// Nothing at the path.
    Missing,
}

/// The caller's view of the workspace. Implementations decide which paths are open
/// buffers; [`DiskView`] reports disk content only.
pub trait WorkspaceView {
    /// Canonical workspace root.
    fn root(&self) -> &Path;
    /// Resolve a workspace-relative path to an absolute path confined to the root (no
    /// absolute paths, no `..`, no symlink leading outside the root, final component not a
    /// symlink). The target need not exist. [`resolve_confined`] implements these rules.
    fn resolve(&self, rel: &Path) -> Result<PathBuf, ViewError>;
    /// What is at the resolved path `abs`.
    fn current(&self, abs: &Path) -> Result<Current, ViewError>;
}

/// Resolve `rel` under the canonical directory `root`, following the rules of
/// [`WorkspaceView::resolve`]. Existing intermediate symlinks are followed and must stay
/// inside the root; a symlink as the final component is refused (changes act on directory
/// entries, and a change through a link would modify a file other than the one named).
pub fn resolve_confined(root: &Path, rel: &Path) -> Result<PathBuf, ViewError> {
    let invalid = |reason: &str| ViewError::InvalidPath {
        path: rel.to_path_buf(),
        reason: reason.to_string(),
    };
    let mut parts = Vec::new();
    for component in rel.components() {
        match component {
            Component::Normal(part) => parts.push(part),
            Component::CurDir => {}
            Component::ParentDir => return Err(invalid("`..` components are not allowed")),
            Component::RootDir | Component::Prefix(_) => {
                return Err(ViewError::OutsideRoot {
                    path: rel.to_path_buf(),
                });
            }
        }
    }
    if parts.is_empty() {
        return Err(invalid("the path is empty"));
    }
    let last = parts.len() - 1;
    let mut current = root.to_path_buf();
    let mut missing_from: Option<usize> = None;
    for (i, part) in parts.iter().enumerate() {
        let candidate = current.join(part);
        match fs::symlink_metadata(&candidate) {
            Ok(meta) if meta.file_type().is_symlink() => {
                if i == last {
                    return Err(invalid(
                        "the path is a symbolic link; changes through symlinks are refused",
                    ));
                }
                let target = fs::canonicalize(&candidate).map_err(|_| ViewError::OutsideRoot {
                    path: rel.to_path_buf(),
                })?;
                if !target.starts_with(root) {
                    return Err(ViewError::OutsideRoot {
                        path: rel.to_path_buf(),
                    });
                }
                current = target;
            }
            Ok(meta) => {
                if i != last && !meta.is_dir() {
                    return Err(invalid("a parent component is not a directory"));
                }
                current = candidate;
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                missing_from = Some(i);
                break;
            }
            Err(source) => {
                return Err(ViewError::Io {
                    path: rel.to_path_buf(),
                    source,
                });
            }
        }
    }
    if let Some(i) = missing_from {
        for part in &parts[i..] {
            current.push(part);
        }
    }
    if !current.starts_with(root) {
        return Err(ViewError::OutsideRoot {
            path: rel.to_path_buf(),
        });
    }
    Ok(current)
}

/// A [`WorkspaceView`] that reports disk content only (no open buffers).
#[derive(Clone, Debug)]
pub struct DiskView {
    root: PathBuf,
    limits: Limits,
}

impl DiskView {
    /// View of the directory `root` (canonicalised here).
    pub fn new(root: &Path) -> io::Result<Self> {
        let root = fs::canonicalize(root)?;
        if !root.is_dir() {
            return Err(io::Error::other(format!(
                "{} is not a directory",
                root.display()
            )));
        }
        Ok(Self {
            root,
            limits: Limits::default(),
        })
    }

    /// Same, with explicit read limits.
    pub fn with_limits(root: &Path, limits: Limits) -> io::Result<Self> {
        let mut view = Self::new(root)?;
        view.limits = limits;
        Ok(view)
    }
}

impl WorkspaceView for DiskView {
    fn root(&self) -> &Path {
        &self.root
    }

    fn resolve(&self, rel: &Path) -> Result<PathBuf, ViewError> {
        resolve_confined(&self.root, rel)
    }

    fn current(&self, abs: &Path) -> Result<Current, ViewError> {
        match fs::symlink_metadata(abs) {
            Ok(meta) if !meta.is_file() => {
                return Err(ViewError::Unreadable {
                    path: abs.to_path_buf(),
                    reason: "not a regular file".to_string(),
                });
            }
            Ok(_) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Current::Missing),
            Err(source) => {
                return Err(ViewError::Io {
                    path: abs.to_path_buf(),
                    source,
                });
            }
        }
        let snapshot = read_disk(abs, &self.limits).map_err(|e| ViewError::Unreadable {
            path: abs.to_path_buf(),
            reason: e.to_string(),
        })?;
        if !snapshot.state.exists {
            return Ok(Current::Missing);
        }
        Ok(Current::Disk {
            hash: snapshot.state.hash,
            bytes: snapshot.bytes,
        })
    }
}
