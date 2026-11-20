//! Late writers: programs that opened a file before `titand` saved it and keep writing to
//! the old version afterwards.
//!
//! A save keeps the replaced version open (unlinked, invisible in the directory). Every
//! tick checks it with `fstat`; when it changed — or at the end of the grace period, by
//! content — the old version is read through the descriptor and, if it differs from what
//! was replaced, preserved: in `<state-dir>/displaced/` (or next to the file when recovery
//! is disabled) and reported with `document/lateWrite`. After the grace period the
//! descriptor is closed; later writes through old descriptors are not seen (the documented
//! guarantee boundary, ADR 0010).

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::json;
use titan_doc::DisplacedFile;
use titan_protocol::messages::notifications;

use super::{Session, display_path};

/// How long a replaced version stays open after a save.
pub(super) const LATE_WRITE_GRACE: Duration = Duration::from_secs(30);
/// At most this many replaced versions are watched at once (the oldest is checked and
/// released first).
const MAX_WATCHED: usize = 64;

pub(super) struct Watched {
    pub doc_id: u64,
    pub path: PathBuf,
    pub file: DisplacedFile,
}

fn unix_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0)
}

impl Session {
    pub(super) fn watch_displaced(&mut self, doc_id: u64, path: &Path, file: DisplacedFile) {
        if self.displaced.len() >= MAX_WATCHED {
            let oldest = self.displaced.remove(0);
            self.finish_displaced(oldest, true);
        }
        self.displaced.push(Watched {
            doc_id,
            path: path.to_path_buf(),
            file,
        });
    }

    pub(super) fn check_displaced(&mut self) {
        if self.displaced.is_empty() {
            return;
        }
        let watched = std::mem::take(&mut self.displaced);
        for w in watched {
            let touched = w.file.touched().unwrap_or(true);
            let expired = w.file.age() >= LATE_WRITE_GRACE;
            if touched || expired {
                self.finish_displaced(w, expired);
            } else {
                self.displaced.push(w);
            }
        }
    }

    /// Final check of every watched version (shutdown).
    pub(super) fn release_displaced(&mut self) {
        for w in std::mem::take(&mut self.displaced) {
            self.finish_displaced(w, true);
        }
    }

    /// Read the old version; preserve and report it if it changed. `release`: stop watching
    /// even when unchanged.
    fn finish_displaced(&mut self, mut w: Watched, release: bool) {
        let limit = self.options.limits.max_file_bytes;
        match w.file.late_write(limit) {
            Ok(Some(bytes)) => {
                self.counters.late_writes += 1;
                let preserved = self.preserve_late_write(&w.path, &bytes);
                let detected_after_ms = u64::try_from(w.file.age().as_millis()).unwrap_or(0);
                eprintln!(
                    "titand: late write to the replaced version of {} detected; preserved at {:?}",
                    display_path(&w.path),
                    preserved
                );
                if self.caps.disk_notifications {
                    let path = self.relative_display(&w.path);
                    self.push_notification(
                        notifications::DOCUMENT_LATE_WRITE,
                        json!({
                            "documentId": w.doc_id,
                            "path": path,
                            "preservedCopy": preserved.as_deref().map(display_path),
                            "bytes": bytes.len(),
                            "detectedAfterMs": detected_after_ms,
                        }),
                    );
                }
                // Keep watching: the writer may continue within the grace period.
                if !release && w.file.age() < LATE_WRITE_GRACE {
                    self.displaced.push(w);
                }
            }
            Ok(None) if !release => self.displaced.push(w),
            Ok(None) => {}
            Err(e) => eprintln!("titand: cannot check a replaced version: {e}"),
        }
    }

    fn preserve_late_write(&self, path: &Path, bytes: &[u8]) -> Option<PathBuf> {
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| "file".to_string());
        let target = match &self.options.state_dir {
            Some(state) => {
                let dir = state.join("displaced");
                fs::create_dir_all(&dir).ok()?;
                dir.join(format!(
                    "{:016x}-{name}.{}.late",
                    titan_doc::stable_hash(path.to_string_lossy().as_bytes()),
                    unix_ms()
                ))
            }
            None => path.with_file_name(format!("{name}.titan-latewrite-{}", unix_ms())),
        };
        let mut tmp = tempfile::Builder::new()
            .prefix(".late-")
            .tempfile_in(target.parent()?)
            .ok()?;
        use std::io::Write as _;
        tmp.write_all(bytes).ok()?;
        tmp.as_file().sync_all().ok()?;
        tmp.persist_noclobber(&target).ok()?;
        Some(target)
    }
}
