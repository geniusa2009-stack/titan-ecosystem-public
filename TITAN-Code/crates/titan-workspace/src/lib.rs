//! TITAN Code workspace services.
//!
//! * The workspace root is canonicalised once. Every path a client supplies is resolved and
//!   canonicalised, then checked to still be inside the root. That rejects `..` traversal,
//!   absolute paths elsewhere, and symlinks that point outside the workspace.
//! * File enumeration uses the `ignore` crate (ripgrep's walker): `.gitignore`, `.ignore`
//!   and `.git/info/exclude` are honoured even outside a git repository, hidden entries are
//!   skipped, and symlinks are not followed. Results are sorted for determinism.
//! * Search is a literal substring search. Open documents can be passed as overlays so
//!   unsaved in-memory text is searched instead of the stale file on disk; every match
//!   records whether it came from disk or from a buffer at a specific revision.

use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

use ignore::WalkBuilder;

#[derive(Debug, thiserror::Error)]
pub enum WorkspaceError {
    #[error("I/O error on {path:?}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("{path:?} is not a directory")]
    NotADirectory { path: PathBuf },
    #[error("{path:?} is outside the workspace")]
    OutsideWorkspace { path: PathBuf },
    #[error("the search query is empty")]
    EmptyQuery,
}

/// In-memory text of an open document, used instead of the file on disk.
#[derive(Clone, Debug)]
pub struct Overlay {
    pub text: String,
    pub revision: u64,
}

/// Where a search match was found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MatchSource {
    Disk,
    Buffer { revision: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SearchMatch {
    /// Path relative to the workspace root.
    pub path: PathBuf,
    /// Zero-based line (LF, CRLF and lone CR all end a line).
    pub line: usize,
    /// UTF-8 byte columns of the match within the line (end exclusive).
    pub column: usize,
    pub end_column: usize,
    /// The same columns in UTF-16 code units.
    pub column_utf16: usize,
    pub end_column_utf16: usize,
    /// The line's text without its terminator, truncated to [`MAX_LINE_PREVIEW`] bytes.
    pub line_text: String,
    pub source: MatchSource,
}

pub const MAX_LINE_PREVIEW: usize = 400;

#[derive(Clone, Debug)]
pub struct SearchQuery {
    pub pattern: String,
    pub case_sensitive: bool,
    pub max_results: usize,
    /// Files larger than this are skipped (counted in `files_skipped`).
    pub max_file_bytes: u64,
}

impl SearchQuery {
    pub fn literal(pattern: impl Into<String>) -> Self {
        Self {
            pattern: pattern.into(),
            case_sensitive: true,
            max_results: 1000,
            max_file_bytes: 8 * 1024 * 1024,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SearchResult {
    pub matches: Vec<SearchMatch>,
    pub truncated: bool,
    pub files_searched: usize,
    /// Files skipped because they were too large, binary, not UTF-8 or unreadable.
    pub files_skipped: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FileList {
    /// Paths relative to the workspace root, sorted.
    pub files: Vec<PathBuf>,
    pub truncated: bool,
}

#[derive(Clone, Debug)]
pub struct Workspace {
    root: PathBuf,
}

impl Workspace {
    pub fn open(root: &Path) -> Result<Self, WorkspaceError> {
        let root = fs::canonicalize(root).map_err(|source| WorkspaceError::Io {
            path: root.to_path_buf(),
            source,
        })?;
        if !root.is_dir() {
            return Err(WorkspaceError::NotADirectory { path: root });
        }
        Ok(Self { root })
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Resolve a client-supplied path (relative to the root, or absolute) to the canonical
    /// path of an existing entry inside the workspace.
    pub fn resolve_existing(&self, path: &Path) -> Result<PathBuf, WorkspaceError> {
        let joined = if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.root.join(path)
        };
        let canonical = fs::canonicalize(&joined).map_err(|source| WorkspaceError::Io {
            path: path.to_path_buf(),
            source,
        })?;
        if !canonical.starts_with(&self.root) {
            return Err(WorkspaceError::OutsideWorkspace {
                path: path.to_path_buf(),
            });
        }
        Ok(canonical)
    }

    /// True if a relative path stays inside the root lexically (no `..` escape, not
    /// absolute). Useful before the target exists.
    pub fn is_lexically_contained(path: &Path) -> bool {
        let mut depth: usize = 0;
        for component in path.components() {
            match component {
                Component::Normal(_) => depth += 1,
                Component::CurDir => {}
                Component::ParentDir => {
                    if depth == 0 {
                        return false;
                    }
                    depth -= 1;
                }
                Component::RootDir | Component::Prefix(_) => return false,
            }
        }
        true
    }

    /// Path relative to the workspace root, if `path` is inside it.
    pub fn relative(&self, path: &Path) -> Option<PathBuf> {
        path.strip_prefix(&self.root).ok().map(Path::to_path_buf)
    }

    fn walker(&self) -> ignore::Walk {
        WalkBuilder::new(&self.root)
            .hidden(true)
            .git_ignore(true)
            .git_exclude(true)
            .git_global(false)
            .ignore(true)
            .parents(false)
            .require_git(false)
            .follow_links(false)
            .sort_by_file_name(|a, b| a.cmp(b))
            .build()
    }

    /// Regular files in the workspace, respecting ignore rules, up to `limit` entries.
    pub fn list_files(&self, limit: usize) -> FileList {
        let mut list = FileList::default();
        for entry in self.walker().flatten() {
            if !entry.file_type().is_some_and(|t| t.is_file()) {
                continue;
            }
            if list.files.len() == limit {
                list.truncated = true;
                break;
            }
            if let Some(rel) = self.relative(entry.path()) {
                list.files.push(rel);
            }
        }
        list
    }

    /// Literal search over the workspace. `overlays` maps canonical absolute paths of open
    /// documents to their in-memory text.
    pub fn search(
        &self,
        query: &SearchQuery,
        overlays: &HashMap<PathBuf, Overlay>,
    ) -> Result<SearchResult, WorkspaceError> {
        self.search_cancellable(query, overlays, &|| false)
    }

    /// Like [`Workspace::search`], but checks `cancelled` before each file and stops early
    /// (returning what was found so far, marked `truncated`) when it returns true.
    pub fn search_cancellable(
        &self,
        query: &SearchQuery,
        overlays: &HashMap<PathBuf, Overlay>,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<SearchResult, WorkspaceError> {
        if query.pattern.is_empty() {
            return Err(WorkspaceError::EmptyQuery);
        }
        let needle = if query.case_sensitive {
            query.pattern.clone()
        } else {
            query.pattern.to_ascii_lowercase()
        };
        let mut result = SearchResult::default();
        for entry in self.walker() {
            if cancelled() {
                result.truncated = true;
                break;
            }
            let Ok(entry) = entry else {
                result.files_skipped += 1;
                continue;
            };
            if !entry.file_type().is_some_and(|t| t.is_file()) {
                continue;
            }
            let Some(rel) = self.relative(entry.path()) else {
                continue;
            };
            let (text, source) = if let Some(overlay) = overlays.get(entry.path()) {
                (
                    overlay.text.clone(),
                    MatchSource::Buffer {
                        revision: overlay.revision,
                    },
                )
            } else {
                match read_searchable(entry.path(), query.max_file_bytes) {
                    Some(text) => (text, MatchSource::Disk),
                    None => {
                        result.files_skipped += 1;
                        continue;
                    }
                }
            };
            result.files_searched += 1;
            if search_text(&text, &needle, query, &rel, source, &mut result) {
                result.truncated = true;
                break;
            }
        }
        Ok(result)
    }
}

/// Read a file for search, or `None` if it is too large, binary, unreadable or not UTF-8.
fn read_searchable(path: &Path, max_bytes: u64) -> Option<String> {
    let meta = fs::metadata(path).ok()?;
    if meta.len() > max_bytes {
        return None;
    }
    let bytes = fs::read(path).ok()?;
    if bytes.contains(&0) {
        return None;
    }
    String::from_utf8(bytes).ok()
}

/// Append matches in `text`; returns true if `max_results` was reached.
fn search_text(
    text: &str,
    needle: &str,
    query: &SearchQuery,
    rel: &Path,
    source: MatchSource,
    result: &mut SearchResult,
) -> bool {
    for (line_index, line) in split_lines(text).enumerate() {
        let haystack = if query.case_sensitive {
            std::borrow::Cow::Borrowed(line)
        } else {
            std::borrow::Cow::Owned(line.to_ascii_lowercase())
        };
        for (column, _) in haystack.match_indices(needle) {
            if result.matches.len() == query.max_results {
                return true;
            }
            // ASCII lower-casing keeps byte positions, so columns apply to `line` too.
            let end_column = column + needle.len();
            let column_utf16 = line[..column].encode_utf16().count();
            let end_column_utf16 = column_utf16 + line[column..end_column].encode_utf16().count();
            result.matches.push(SearchMatch {
                path: rel.to_path_buf(),
                line: line_index,
                column,
                end_column,
                column_utf16,
                end_column_utf16,
                line_text: preview(line),
                source,
            });
        }
    }
    false
}

/// Split into lines on LF, CRLF or lone CR, without terminators.
fn split_lines(text: &str) -> impl Iterator<Item = &str> {
    let mut rest = Some(text);
    std::iter::from_fn(move || {
        let current = rest?;
        match current.find(['\n', '\r']) {
            Some(i) => {
                let after = if current[i..].starts_with("\r\n") {
                    i + 2
                } else {
                    i + 1
                };
                rest = Some(&current[after..]);
                Some(&current[..i])
            }
            None => {
                rest = None;
                Some(current)
            }
        }
    })
}

fn preview(line: &str) -> String {
    if line.len() <= MAX_LINE_PREVIEW {
        return line.to_string();
    }
    let mut end = MAX_LINE_PREVIEW;
    while !line.is_char_boundary(end) {
        end -= 1;
    }
    line[..end].to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn setup() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("target/debug")).unwrap();
        fs::write(root.join(".gitignore"), "target/\n*.log\n").unwrap();
        fs::write(
            root.join("src/main.rs"),
            "fn main() {\r\n    titan();\r\n}\r\n",
        )
        .unwrap();
        fs::write(
            root.join("src/lib.rs"),
            "pub fn titan() {}\n// TITAN مرحبا titan\n",
        )
        .unwrap();
        fs::write(root.join("target/debug/out.rs"), "titan").unwrap();
        fs::write(root.join("debug.log"), "titan").unwrap();
        fs::write(root.join("blob.bin"), b"titan\0").unwrap();
        dir
    }

    #[test]
    fn list_respects_ignore_rules_and_hidden() {
        let dir = setup();
        let ws = Workspace::open(dir.path()).unwrap();
        let list = ws.list_files(100);
        let names: Vec<String> = list
            .files
            .iter()
            .map(|p| p.to_string_lossy().replace('\\', "/"))
            .collect();
        assert_eq!(names, vec!["blob.bin", "src/lib.rs", "src/main.rs"]);
        assert!(!list.truncated);
        let limited = ws.list_files(1);
        assert_eq!(limited.files.len(), 1);
        assert!(limited.truncated);
    }

    #[test]
    fn search_case_sensitivity_columns_and_skips() {
        let dir = setup();
        let ws = Workspace::open(dir.path()).unwrap();
        let result = ws
            .search(&SearchQuery::literal("titan"), &HashMap::new())
            .unwrap();
        let found: Vec<(String, usize, usize)> = result
            .matches
            .iter()
            .map(|m| {
                (
                    m.path.to_string_lossy().replace('\\', "/"),
                    m.line,
                    m.column,
                )
            })
            .collect();
        let tail = "// TITAN مرحبا ".len();
        assert_eq!(
            found,
            vec![
                ("src/lib.rs".to_string(), 0, 7),
                ("src/lib.rs".to_string(), 1, tail),
                ("src/main.rs".to_string(), 1, 4),
            ]
        );
        assert_eq!(result.files_skipped, 1, "blob.bin is binary");
        assert_eq!(result.matches[2].line_text, "    titan();");
        // UTF-16 columns after Arabic text: "// TITAN " is 9 units, "مرحبا " is 6.
        assert_eq!(result.matches[1].column_utf16, 15);
        assert_eq!(result.matches[1].end_column_utf16, 20);
        assert_eq!(result.matches[1].end_column, tail + 5);

        let mut insensitive = SearchQuery::literal("TITAN");
        insensitive.case_sensitive = false;
        let result = ws.search(&insensitive, &HashMap::new()).unwrap();
        assert_eq!(result.matches.len(), 4);

        let mut capped = SearchQuery::literal("titan");
        capped.max_results = 2;
        let result = ws.search(&capped, &HashMap::new()).unwrap();
        assert_eq!(result.matches.len(), 2);
        assert!(result.truncated);

        assert!(matches!(
            ws.search(&SearchQuery::literal(""), &HashMap::new()),
            Err(WorkspaceError::EmptyQuery)
        ));
    }

    #[test]
    fn overlays_replace_disk_content() {
        let dir = setup();
        let ws = Workspace::open(dir.path()).unwrap();
        let main = ws.resolve_existing(Path::new("src/main.rs")).unwrap();
        let mut overlays = HashMap::new();
        overlays.insert(
            main,
            Overlay {
                text: "unsaved titan edit\n".to_string(),
                revision: 7,
            },
        );
        let result = ws
            .search(&SearchQuery::literal("unsaved"), &overlays)
            .unwrap();
        assert_eq!(result.matches.len(), 1);
        assert_eq!(
            result.matches[0].source,
            MatchSource::Buffer { revision: 7 }
        );
    }

    #[test]
    fn path_resolution_rejects_escapes() {
        let dir = setup();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("secret.txt"), "x").unwrap();
        let ws = Workspace::open(dir.path()).unwrap();

        assert!(ws.resolve_existing(Path::new("src/main.rs")).is_ok());
        assert!(ws.resolve_existing(Path::new("src/../src/lib.rs")).is_ok());
        assert!(matches!(
            ws.resolve_existing(&outside.path().join("secret.txt")),
            Err(WorkspaceError::OutsideWorkspace { .. })
        ));
        let escape = Path::new("..").join(outside.path().file_name().unwrap());
        assert!(ws.resolve_existing(&escape.join("secret.txt")).is_err());
        assert!(matches!(
            ws.resolve_existing(Path::new("missing.rs")),
            Err(WorkspaceError::Io { .. })
        ));

        assert!(Workspace::is_lexically_contained(Path::new("a/b/../c")));
        assert!(!Workspace::is_lexically_contained(Path::new("a/../../c")));
        assert!(!Workspace::is_lexically_contained(Path::new("/etc/passwd")));
    }

    #[cfg(unix)]
    #[test]
    fn symlink_pointing_outside_is_rejected() {
        let dir = setup();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("secret.txt"), "x").unwrap();
        std::os::unix::fs::symlink(
            outside.path().join("secret.txt"),
            dir.path().join("link.txt"),
        )
        .unwrap();
        let ws = Workspace::open(dir.path()).unwrap();
        assert!(matches!(
            ws.resolve_existing(Path::new("link.txt")),
            Err(WorkspaceError::OutsideWorkspace { .. })
        ));
    }

    #[test]
    fn split_lines_handles_all_terminators() {
        let lines: Vec<&str> = split_lines("a\nb\r\nc\rd").collect();
        assert_eq!(lines, vec!["a", "b", "c", "d"]);
        let lines: Vec<&str> = split_lines("x\n").collect();
        assert_eq!(lines, vec!["x", ""]);
    }

    #[test]
    fn open_rejects_files() {
        let dir = setup();
        assert!(matches!(
            Workspace::open(&dir.path().join("src/main.rs")),
            Err(WorkspaceError::NotADirectory { .. })
        ));
    }
}
