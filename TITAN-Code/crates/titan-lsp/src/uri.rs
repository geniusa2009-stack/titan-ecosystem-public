//! `file://` URIs for absolute paths (RFC 8089), percent-encoding everything outside the
//! unreserved set and `/`.

use std::path::{Path, PathBuf};

/// Converts a path into a `file://` URI. Relative paths are resolved against the current
/// directory first (without touching the filesystem otherwise; symlinks are kept).
pub fn path_to_uri(path: &Path) -> String {
    let absolute: PathBuf = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map(|cwd| cwd.join(path))
            .unwrap_or_else(|_| path.to_path_buf())
    };
    let mut uri = String::from("file://");
    let bytes = path_bytes(&absolute);
    if bytes.first() != Some(&b'/') {
        // Windows-style `C:/...`: the path part of the URI must start with `/`.
        uri.push('/');
    }
    for &byte in &bytes {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~' | b'/') {
            uri.push(char::from(byte));
        } else {
            uri.push_str(&format!("%{byte:02X}"));
        }
    }
    uri
}

#[cfg(unix)]
fn path_bytes(path: &Path) -> Vec<u8> {
    use std::os::unix::ffi::OsStrExt;
    path.as_os_str().as_bytes().to_vec()
}

#[cfg(not(unix))]
fn path_bytes(path: &Path) -> Vec<u8> {
    path.to_string_lossy().replace('\\', "/").into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(unix)]
    #[test]
    fn encodes_reserved_and_non_ascii_bytes() {
        assert_eq!(
            path_to_uri(Path::new("/tmp/a b/c.rs")),
            "file:///tmp/a%20b/c.rs"
        );
        assert_eq!(
            path_to_uri(Path::new("/w/كل/x#y?.rs")),
            "file:///w/%D9%83%D9%84/x%23y%3F.rs"
        );
        assert_eq!(
            path_to_uri(Path::new("/a/b_c-d~e.f")),
            "file:///a/b_c-d~e.f"
        );
    }

    #[test]
    fn relative_paths_become_absolute() {
        let uri = path_to_uri(Path::new("relative.rs"));
        assert!(uri.starts_with("file:///"), "{uri}");
        assert!(uri.ends_with("/relative.rs"), "{uri}");
    }
}
