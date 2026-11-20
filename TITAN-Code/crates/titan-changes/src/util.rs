//! Small file-system helpers shared by the journal, the applier and the evidence store.

use std::fs::{self, File};
use std::io::{self, Write};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use titan_doc::{ContentHash, hash_reader};

/// Milliseconds since the Unix epoch (0 if the clock is before it).
pub(crate) fn unix_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
        .unwrap_or(0)
}

/// `fsync` a directory so entries created or renamed in it are durable.
#[cfg(unix)]
pub(crate) fn sync_dir(dir: &Path) -> io::Result<()> {
    File::open(dir)?.sync_all()
}

#[cfg(not(unix))]
pub(crate) fn sync_dir(_dir: &Path) -> io::Result<()> {
    Ok(())
}

/// Create `path` (which must not exist), write `bytes`, `fsync` it.
pub(crate) fn write_new_synced(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)?;
    file.write_all(bytes)?;
    file.sync_all()
}

/// Write `bytes` to `path` atomically: temporary file in the same directory, `fsync`,
/// rename over `path`, `fsync` the directory.
pub(crate) fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let dir = match path.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent,
        _ => Path::new("."),
    };
    let mut tmp = tempfile::Builder::new()
        .prefix(".titan-evidence-")
        .tempfile_in(dir)?;
    tmp.write_all(bytes)?;
    tmp.flush()?;
    tmp.as_file().sync_all()?;
    tmp.persist(path).map_err(|e| e.error)?;
    sync_dir(dir)
}

/// What is at a path, by content.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DiskVersion {
    /// Nothing at the path.
    Absent,
    /// A regular file with this content.
    File { hash: ContentHash, len: u64 },
    /// Something that is not a regular file (directory, socket, ...).
    Other,
}

/// Hash whatever is at `path` (symlinks are followed by `open`).
pub(crate) fn disk_version(path: &Path) -> io::Result<DiskVersion> {
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.file_type().is_symlink() => return Ok(DiskVersion::Other),
        Ok(meta) if !meta.is_file() => return Ok(DiskVersion::Other),
        Ok(_) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(DiskVersion::Absent),
        Err(e) => return Err(e),
    }
    let mut file = match File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(DiskVersion::Absent),
        Err(e) => return Err(e),
    };
    let (hash, len) = hash_reader(&mut file)?;
    Ok(DiskVersion::File { hash, len })
}

/// Read `path` and check it has exactly this content. Never returns unverified bytes.
pub(crate) fn read_verified(path: &Path, hash: ContentHash, len: u64) -> io::Result<Vec<u8>> {
    let bytes = fs::read(path)?;
    if bytes.len() as u64 != len || ContentHash::of(&bytes) != hash {
        return Err(io::Error::other(format!(
            "{} does not have the recorded content (expected {len} bytes, blake3 {hash})",
            path.display()
        )));
    }
    Ok(bytes)
}

/// Permission bits of a file, where the platform has them.
#[cfg(unix)]
pub(crate) fn file_mode(path: &Path) -> Option<u32> {
    use std::os::unix::fs::PermissionsExt;
    fs::metadata(path)
        .ok()
        .map(|m| m.permissions().mode() & 0o7777)
}

#[cfg(not(unix))]
pub(crate) fn file_mode(_path: &Path) -> Option<u32> {
    None
}

/// Set permission bits (no-op where the platform has none).
#[cfg(unix)]
pub(crate) fn set_mode(path: &Path, mode: Option<u32>) -> io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    match mode {
        Some(mode) => fs::set_permissions(path, fs::Permissions::from_mode(mode)),
        None => Ok(()),
    }
}

#[cfg(not(unix))]
pub(crate) fn set_mode(_path: &Path, _mode: Option<u32>) -> io::Result<()> {
    Ok(())
}
