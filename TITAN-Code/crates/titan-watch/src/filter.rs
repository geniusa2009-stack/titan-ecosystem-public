//! Path classification: workspace boundary checks and the built-in temp/noise filter.

use std::ffi::OsStr;
use std::path::{Component, Path, PathBuf};

/// Prefix of the temporary file `titan-doc` writes before renaming it over the target.
pub const TITAN_SAVE_PREFIX: &str = ".titan-save-";

/// True if `name` (a single path component) is an editor or tool temporary/noise file that
/// is never reported:
///
/// * `.titan-save-*` (TITAN's own atomic-save temp files),
/// * `.#*` (Emacs lock files) and `#*#` (Emacs auto-save files),
/// * `*~` (backup files), `*.swp`, `*.swx`, `*.swo` (Vim swap files), `4913` (Vim's
///   write-permission probe),
/// * `*.tmp`, `*.crswap` (Chrome/VS Code-style atomic-write temp files),
/// * `.DS_Store` (Finder metadata).
///
/// Names that are not valid UTF-8 are never treated as temp files.
pub fn is_temp_name(name: &OsStr) -> bool {
    let Some(name) = name.to_str() else {
        return false;
    };
    name.starts_with(TITAN_SAVE_PREFIX)
        || name.starts_with(".#")
        || (name.len() >= 2 && name.starts_with('#') && name.ends_with('#'))
        || name.ends_with('~')
        || name.ends_with(".swp")
        || name.ends_with(".swx")
        || name.ends_with(".swo")
        || name.ends_with(".tmp")
        || name.ends_with(".crswap")
        || name == "4913"
        || name == ".DS_Store"
}

/// Where an event path lies relative to the workspace root.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Verdict {
    /// Inside the root and not filtered: report it (absolute path).
    Report(PathBuf),
    /// The workspace root itself.
    Root,
    /// Filtered by the built-in temp filter or the caller's ignore predicate.
    Ignored,
    /// Not lexically inside the root (or not a clean absolute path).
    Outside,
}

/// Classifies `path` lexically against the canonical `root`. The caller's `ignore` predicate
/// receives the path **relative to the root**.
pub(crate) fn classify(path: &Path, root: &Path, ignore: &dyn Fn(&Path) -> bool) -> Verdict {
    if !path.is_absolute() {
        return Verdict::Outside;
    }
    let Ok(rel) = path.strip_prefix(root) else {
        return Verdict::Outside;
    };
    let mut empty = true;
    for component in rel.components() {
        match component {
            Component::Normal(name) => {
                empty = false;
                if is_temp_name(name) {
                    return Verdict::Ignored;
                }
            }
            // `..`, `.`, a second root or a prefix could escape the root lexically.
            _ => return Verdict::Outside,
        }
    }
    if empty {
        return Verdict::Root;
    }
    if ignore(rel) {
        return Verdict::Ignored;
    }
    Verdict::Report(path.to_path_buf())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn temp_names() {
        for name in [
            ".titan-save-a1B2c3",
            ".#main.rs",
            "#main.rs#",
            "main.rs~",
            ".main.rs.swp",
            ".main.rs.swx",
            ".main.rs.swo",
            "build.tmp",
            "4913",
            ".DS_Store",
            "file.txt.crswap",
        ] {
            assert!(is_temp_name(OsStr::new(name)), "{name} should be filtered");
        }
        for name in [
            "main.rs",
            "4914",
            "#",
            "tmp",
            "swp",
            "DS_Store",
            ".gitignore",
        ] {
            assert!(!is_temp_name(OsStr::new(name)), "{name} should be kept");
        }
    }

    #[test]
    fn classification() {
        let root = Path::new("/ws");
        let no_ignore = |_: &Path| false;
        assert_eq!(
            classify(Path::new("/ws/src/a.rs"), root, &no_ignore),
            Verdict::Report(PathBuf::from("/ws/src/a.rs"))
        );
        assert_eq!(classify(Path::new("/ws"), root, &no_ignore), Verdict::Root);
        assert_eq!(
            classify(Path::new("/other/a.rs"), root, &no_ignore),
            Verdict::Outside
        );
        assert_eq!(
            classify(Path::new("/wsx/a.rs"), root, &no_ignore),
            Verdict::Outside
        );
        assert_eq!(
            classify(Path::new("/ws/../etc/passwd"), root, &no_ignore),
            Verdict::Outside
        );
        assert_eq!(
            classify(Path::new("relative/a.rs"), root, &no_ignore),
            Verdict::Outside
        );
        assert_eq!(
            classify(Path::new("/ws/src/.titan-save-xyz"), root, &no_ignore),
            Verdict::Ignored
        );
        assert_eq!(
            classify(Path::new("/ws/.#lockdir/inner.rs"), root, &no_ignore),
            Verdict::Ignored
        );
    }

    #[test]
    fn ignore_predicate_sees_relative_paths() {
        let root = Path::new("/ws");
        let ignore = |rel: &Path| {
            assert!(rel.is_relative(), "{rel:?}");
            rel.starts_with("target")
        };
        assert_eq!(
            classify(Path::new("/ws/target/debug/x"), root, &ignore),
            Verdict::Ignored
        );
        assert_eq!(
            classify(Path::new("/ws/target"), root, &ignore),
            Verdict::Ignored
        );
        assert_eq!(
            classify(Path::new("/ws/src/target.rs"), root, &ignore),
            Verdict::Report(PathBuf::from("/ws/src/target.rs"))
        );
    }
}
