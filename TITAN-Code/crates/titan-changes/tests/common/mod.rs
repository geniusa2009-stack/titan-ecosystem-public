//! Shared fixtures: a temporary workspace and state directory on the real file system.
#![allow(dead_code)]

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

use titan_changes::{
    BaseSnapshot, ByteEdit, Current, DiskView, ExpectedBase, FileChange, Proposal, ProposalSource,
    ViewError, WorkspaceView,
};

/// Disk view plus open buffers (absolute path -> revision, text).
pub struct BufferView {
    pub disk: DiskView,
    pub buffers: HashMap<PathBuf, (u64, String)>,
}

impl WorkspaceView for BufferView {
    fn root(&self) -> &Path {
        self.disk.root()
    }
    fn resolve(&self, rel: &Path) -> Result<PathBuf, ViewError> {
        self.disk.resolve(rel)
    }
    fn current(&self, abs: &Path) -> Result<Current, ViewError> {
        match self.buffers.get(abs) {
            Some((revision, text)) => Ok(Current::OpenBuffer {
                revision: *revision,
                text: text.clone(),
            }),
            None => self.disk.current(abs),
        }
    }
}

pub struct Ws {
    _tmp: tempfile::TempDir,
    pub root: PathBuf,
    pub state: PathBuf,
    pub view: DiskView,
}

impl Ws {
    pub fn new(files: &[(&str, &[u8])]) -> Self {
        let tmp = tempfile::tempdir().expect("tempdir");
        let base = fs::canonicalize(tmp.path()).expect("canonicalize");
        let root = base.join("ws");
        let state = base.join("state");
        fs::create_dir_all(&root).expect("mkdir ws");
        fs::create_dir_all(&state).expect("mkdir state");
        for (rel, bytes) in files {
            let path = root.join(rel);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).expect("mkdir");
            }
            fs::write(&path, bytes).expect("write fixture");
        }
        let view = DiskView::new(&root).expect("view");
        Self {
            _tmp: tmp,
            root,
            state,
            view,
        }
    }

    pub fn path(&self, rel: &str) -> PathBuf {
        self.root.join(rel)
    }

    /// Bytes at `rel`, `None` if absent.
    pub fn read(&self, rel: &str) -> Option<Vec<u8>> {
        fs::read(self.path(rel)).ok()
    }

    pub fn write(&self, rel: &str, bytes: &[u8]) {
        fs::write(self.path(rel), bytes).expect("write");
    }

    pub fn expected(&self, rel: &str) -> ExpectedBase {
        ExpectedBase::disk(&self.read(rel).expect("fixture exists"))
    }

    /// Replace the whole content of `rel`.
    pub fn rewrite(&self, rel: &str, new: &str) -> FileChange {
        let old = self.read(rel).expect("fixture exists");
        FileChange::Edit {
            path: rel.to_string(),
            expected: ExpectedBase::disk(&old),
            edits: vec![ByteEdit::new(0, old.len(), new)],
        }
    }

    pub fn edit(&self, rel: &str, edits: Vec<ByteEdit>) -> FileChange {
        FileChange::Edit {
            path: rel.to_string(),
            expected: self.expected(rel),
            edits,
        }
    }

    pub fn delete(&self, rel: &str) -> FileChange {
        FileChange::Delete {
            path: rel.to_string(),
            expected: self.expected(rel),
            annotation: None,
        }
    }

    pub fn rename(&self, from: &str, to: &str) -> FileChange {
        FileChange::Rename {
            from: from.to_string(),
            to: to.to_string(),
            expected: self.expected(from),
            annotation: None,
        }
    }

    pub fn proposal(&self, changes: Vec<FileChange>) -> Proposal {
        Proposal::new(
            ProposalSource {
                kind: "developer".to_string(),
                detail: serde_json::json!({"test": true}),
            },
            BaseSnapshot {
                workspace_root: self.root.clone(),
                git_head: None,
            },
            changes,
        )
    }

    /// Every regular file under the workspace, relative, sorted (hidden ones included).
    pub fn files(&self) -> Vec<String> {
        let mut out = Vec::new();
        walk(&self.root, &self.root, &mut out);
        out.sort();
        out
    }
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
    for entry in fs::read_dir(dir).expect("read_dir") {
        let entry = entry.expect("entry");
        let path = entry.path();
        if path.is_dir() {
            walk(root, &path, out);
        } else {
            let rel = path.strip_prefix(root).expect("inside root");
            out.push(rel.to_string_lossy().replace('\\', "/"));
        }
    }
}

pub fn create(rel: &str, content: &str) -> FileChange {
    FileChange::Create {
        path: rel.to_string(),
        content: content.to_string(),
        annotation: None,
    }
}

/// Every file (recursively) under `dir`, with its bytes.
pub fn all_files(dir: &Path) -> Vec<(PathBuf, Vec<u8>)> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(entries) = fs::read_dir(&d) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if let Ok(bytes) = fs::read(&path) {
                out.push((path, bytes));
            }
        }
    }
    out
}
