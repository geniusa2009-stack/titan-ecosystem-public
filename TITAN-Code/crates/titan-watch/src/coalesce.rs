//! Coalescing of raw `notify` events into one change per path per window.
//!
//! This module is pure bookkeeping plus `stat` calls at flush time; it owns no threads and
//! is driven by the aggregator thread in `lib.rs` (and directly by the unit tests below).

use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use notify::event::{CreateKind, ModifyKind, RemoveKind, RenameMode};
use notify::{Event, EventKind};

use crate::filter::{Verdict, classify};
use crate::{ChangeKind, Counters, FsChange, IgnoreFn};

/// What the first event seen for a path in the current window said.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum First {
    /// Created or moved in.
    Appeared,
    /// Removed or moved away.
    Vanished,
    /// Content or metadata changed.
    Changed,
}

#[derive(Debug)]
struct Entry {
    first: First,
    /// Set when this path is the target of a paired rename whose source is reportable.
    renamed_from: Option<PathBuf>,
    /// Set when this path was the source of a paired rename.
    moved_away: bool,
    /// Directory hint from the backend (`CreateKind::Folder`, `RemoveKind::Folder`).
    is_dir: Option<bool>,
}

/// One half of an unpaired FSEvents-style rename (`RenameMode::Any`).
#[derive(Debug)]
struct Half {
    path: PathBuf,
    exists: bool,
    /// The path of the next FSEvents record, once its leading `Create`/`Remove` flags were
    /// seen: its own rename flag (expanded after them) may still pair with this half.
    next: Option<PathBuf>,
}

/// What a window produced.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Flushed {
    Changes(Vec<FsChange>),
    Rescan(String),
}

pub(crate) struct Coalescer {
    root: PathBuf,
    max_pending: usize,
    ignore: IgnoreFn,
    counters: Arc<Counters>,
    entries: HashMap<PathBuf, Entry>,
    rescan: Option<String>,
    half: Option<Half>,
}

impl Coalescer {
    pub(crate) fn new(
        root: PathBuf,
        max_pending: usize,
        ignore: IgnoreFn,
        counters: Arc<Counters>,
    ) -> Self {
        Self {
            root,
            max_pending,
            ignore,
            counters,
            entries: HashMap::new(),
            rescan: None,
            half: None,
        }
    }

    /// True if a flush would produce something (changes or a rescan).
    pub(crate) fn has_pending(&self) -> bool {
        self.rescan.is_some() || !self.entries.is_empty()
    }

    /// Marks the window as lossy: buffered detail is dropped and the window ends in a single
    /// `Rescan`. Further events in the same window are discarded. Only the first reason of a
    /// window is kept.
    pub(crate) fn request_rescan(&mut self, reason: String) {
        self.entries.clear();
        self.half = None;
        if self.rescan.is_none() {
            self.rescan = Some(reason);
        }
    }

    /// Records a lost-events condition that counts as an overflow (our bound, the raw queue,
    /// or the kernel queue).
    pub(crate) fn overflow(&mut self, reason: String) {
        if self.rescan.is_none() {
            self.counters.overflows.fetch_add(1, Ordering::Relaxed);
        }
        self.request_rescan(reason);
    }

    /// Feeds one raw backend result. Returns an error message to forward to the consumer
    /// when the backend reported an error.
    pub(crate) fn push(&mut self, result: notify::Result<Event>) -> Option<String> {
        let event = match result {
            Ok(event) => event,
            Err(error) => {
                self.counters.errors.fetch_add(1, Ordering::Relaxed);
                let message = describe_error(&error);
                self.request_rescan(format!("backend error: {message}"));
                return Some(message);
            }
        };
        self.counters.raw_events.fetch_add(1, Ordering::Relaxed);

        if event.need_rescan() {
            let info = event.info().unwrap_or("events were dropped");
            self.overflow(format!("backend requested a rescan ({info})"));
            return None;
        }
        if self.rescan.is_some() {
            // The window is already lossy; detail is pointless until it is flushed.
            return None;
        }

        self.age_half(&event);

        match event.kind {
            EventKind::Access(_) | EventKind::Other => {}
            EventKind::Create(kind) => {
                let is_dir = match kind {
                    CreateKind::Folder => Some(true),
                    CreateKind::File => Some(false),
                    _ => None,
                };
                for path in &event.paths {
                    self.appear(path, is_dir);
                }
            }
            EventKind::Remove(kind) => {
                let is_dir = match kind {
                    RemoveKind::Folder => Some(true),
                    RemoveKind::File => Some(false),
                    _ => None,
                };
                for path in &event.paths {
                    self.vanish(path, is_dir);
                }
            }
            EventKind::Modify(ModifyKind::Name(mode)) => match mode {
                RenameMode::From => {
                    for path in &event.paths {
                        self.vanish(path, None);
                    }
                }
                RenameMode::To => {
                    for path in &event.paths {
                        self.appear(path, None);
                    }
                }
                RenameMode::Both => {
                    if let [from, to] = event.paths.as_slice() {
                        self.link(from, to);
                    }
                }
                RenameMode::Any | RenameMode::Other => {
                    for path in &event.paths {
                        self.rename_half(path);
                    }
                }
            },
            EventKind::Modify(_) | EventKind::Any => {
                for path in &event.paths {
                    self.change(path);
                }
            }
        }
        None
    }

    /// Decides whether a pending FSEvents rename half survives `event`. One FSEvents record
    /// expands into several notify events in the order Create, Remove, Name, Metadata,
    /// Data, and FSEvents flags are cumulative (a freshly created file's rename record may
    /// also carry "created"). The half therefore survives further events for its own path
    /// and the leading Create/Remove events of exactly one following path; anything else
    /// ends it.
    fn age_half(&mut self, event: &Event) {
        if matches!(
            event.kind,
            EventKind::Modify(ModifyKind::Name(RenameMode::Any | RenameMode::Other))
        ) {
            return;
        }
        let Some(half) = self.half.as_mut() else {
            return;
        };
        if event.paths.iter().all(|p| p == &half.path) {
            return;
        }
        if let [path] = event.paths.as_slice() {
            if half.next.as_ref() == Some(path) {
                return;
            }
            if half.next.is_none()
                && matches!(event.kind, EventKind::Create(_) | EventKind::Remove(_))
            {
                half.next = Some(path.clone());
                return;
            }
        }
        self.half = None;
    }

    fn verdict(&self, path: &Path) -> Verdict {
        let verdict = classify(path, &self.root, &*self.ignore);
        match verdict {
            Verdict::Ignored => {
                self.counters.ignored.fetch_add(1, Ordering::Relaxed);
            }
            Verdict::Outside => {
                self.counters.outside_root.fetch_add(1, Ordering::Relaxed);
            }
            Verdict::Report(_) | Verdict::Root => {}
        }
        verdict
    }

    /// Returns the entry for `path`, creating it with `first` if needed. `None` when the
    /// window overflowed (or already had).
    fn entry(&mut self, path: PathBuf, first: First, is_dir: Option<bool>) -> Option<&mut Entry> {
        if self.rescan.is_some() {
            return None;
        }
        if self.entries.contains_key(&path) {
            self.counters.coalesced.fetch_add(1, Ordering::Relaxed);
        } else if self.entries.len() >= self.max_pending {
            self.overflow(format!(
                "more than {} distinct paths changed within one window",
                self.max_pending
            ));
            return None;
        }
        let entry = self.entries.entry(path).or_insert(Entry {
            first,
            renamed_from: None,
            moved_away: false,
            is_dir: None,
        });
        if is_dir.is_some() {
            entry.is_dir = is_dir;
        }
        Some(entry)
    }

    fn appear(&mut self, path: &Path, is_dir: Option<bool>) {
        match self.verdict(path) {
            Verdict::Report(path) => {
                self.entry(path, First::Appeared, is_dir);
            }
            // A `Create` for the root itself carries no information about its contents.
            Verdict::Root | Verdict::Ignored | Verdict::Outside => {}
        }
    }

    fn vanish(&mut self, path: &Path, is_dir: Option<bool>) {
        match self.verdict(path) {
            Verdict::Report(path) => {
                self.entry(path, First::Vanished, is_dir);
            }
            Verdict::Root => {
                self.request_rescan("the workspace root was removed or renamed".to_string());
            }
            Verdict::Ignored | Verdict::Outside => {}
        }
    }

    fn change(&mut self, path: &Path) {
        if let Verdict::Report(path) = self.verdict(path) {
            self.entry(path, First::Changed, None);
        }
        // Metadata changes of the root directory itself are not interesting.
    }

    /// Pairs a rename whose two sides are both known (inotify cookie, Windows, or two
    /// FSEvents halves matched in `rename_half`).
    fn link(&mut self, from: &Path, to: &Path) {
        let from_verdict = self.verdict(from);
        let to_verdict = self.verdict(to);
        match (from_verdict, to_verdict) {
            (Verdict::Report(from), Verdict::Report(to)) => {
                if from == to {
                    return;
                }
                let origin = match self.entry(from.clone(), First::Vanished, None) {
                    Some(entry) => {
                        entry.moved_away = true;
                        entry.renamed_from.take().unwrap_or_else(|| from.clone())
                    }
                    None => return,
                };
                if let Some(entry) = self.entry(to.clone(), First::Appeared, None) {
                    // A -> B -> A collapses to a change of A.
                    entry.renamed_from = if origin == to { None } else { Some(origin) };
                }
            }
            // Moved in from an ignored (e.g. atomic-save temp file) or outside path: the
            // target simply appeared.
            (Verdict::Ignored | Verdict::Outside, Verdict::Report(to)) => {
                self.entry(to, First::Appeared, None);
            }
            // Moved to an ignored or outside path: the source is gone.
            (Verdict::Report(from), Verdict::Ignored | Verdict::Outside) => {
                self.entry(from, First::Vanished, None);
            }
            (Verdict::Root, _) | (_, Verdict::Root) => {
                self.request_rescan("the workspace root was removed or renamed".to_string());
            }
            _ => {}
        }
    }

    /// FSEvents reports each side of a rename separately as `RenameMode::Any` with no cookie.
    /// Two back-to-back halves where exactly one side exists are paired (missing -> existing);
    /// anything else degrades to `Vanished`/`Appeared` by existence.
    fn rename_half(&mut self, path: &Path) {
        let path = match self.verdict(path) {
            Verdict::Report(path) => path,
            Verdict::Root => {
                self.request_rescan("the workspace root was removed or renamed".to_string());
                return;
            }
            // Filtered halves neither pair nor break a pending pair.
            Verdict::Ignored | Verdict::Outside => return,
        };
        let exists = fs::symlink_metadata(&path).is_ok();
        if exists {
            self.entry(path.clone(), First::Appeared, None);
        } else {
            self.entry(path.clone(), First::Vanished, None);
        }
        match self.half.take() {
            Some(half) if half.path == path => {
                self.half = Some(Half { next: None, ..half });
            }
            Some(half) if half.exists != exists => {
                let (from, to) = if exists {
                    (half.path, path)
                } else {
                    (path, half.path)
                };
                self.link(&from, &to);
            }
            _ => {
                self.half = Some(Half {
                    path,
                    exists,
                    next: None,
                });
            }
        }
    }

    /// Ends the window: resolves every buffered path against the filesystem and returns one
    /// change per path, or a single rescan if the window was lossy.
    pub(crate) fn flush(&mut self) -> Option<Flushed> {
        self.half = None;
        if let Some(reason) = self.rescan.take() {
            self.entries.clear();
            return Some(Flushed::Rescan(reason));
        }
        if self.entries.is_empty() {
            return None;
        }
        let entries = std::mem::take(&mut self.entries);
        let mut parents: HashMap<PathBuf, bool> = HashMap::new();
        let mut resolved: Vec<(PathBuf, Entry, Option<fs::Metadata>)> =
            Vec::with_capacity(entries.len());
        for (path, entry) in entries {
            if !self.parent_inside_root(&path, &mut parents) {
                self.counters.outside_root.fetch_add(1, Ordering::Relaxed);
                continue;
            }
            let meta = fs::symlink_metadata(&path).ok();
            resolved.push((path, entry, meta));
        }

        // Pass 1: renames whose target still exists.
        let mut referenced: HashSet<PathBuf> = HashSet::new();
        let mut changes: Vec<FsChange> = Vec::with_capacity(resolved.len());
        let mut rest = Vec::with_capacity(resolved.len());
        for (path, mut entry, meta) in resolved {
            match (&meta, entry.renamed_from.take()) {
                (Some(meta), Some(from)) => {
                    let from = if self.parent_inside_root(&from, &mut parents) {
                        Some(from)
                    } else {
                        None
                    };
                    let kind = match from {
                        Some(from) => {
                            referenced.insert(from.clone());
                            ChangeKind::Renamed { from }
                        }
                        None => ChangeKind::Created,
                    };
                    changes.push(FsChange {
                        path,
                        kind,
                        is_dir: Some(meta.file_type().is_dir()),
                    });
                }
                (_, from) => {
                    entry.renamed_from = from;
                    rest.push((path, entry, meta));
                }
            }
        }

        // Pass 2: everything else, resolved by existence.
        for (path, entry, meta) in rest {
            match meta {
                Some(meta) => {
                    let kind = match entry.first {
                        First::Appeared => ChangeKind::Created,
                        // Existed, went away, came back: replaced.
                        First::Vanished | First::Changed => ChangeKind::Modified,
                    };
                    changes.push(FsChange {
                        path,
                        kind,
                        is_dir: Some(meta.file_type().is_dir()),
                    });
                }
                None => {
                    if entry.moved_away && referenced.contains(&path) {
                        // Covered by the `Renamed` change of the target.
                        continue;
                    }
                    changes.push(FsChange {
                        path,
                        kind: ChangeKind::Removed,
                        is_dir: entry.is_dir,
                    });
                }
            }
        }

        changes.sort_by(|a, b| a.path.cmp(&b.path));

        // A removed path with descendants in the same batch is a directory. Paths sort
        // component-wise, so descendants directly follow their ancestor.
        let dir_like: Vec<usize> = changes
            .windows(2)
            .enumerate()
            .filter(|(_, pair)| {
                pair[0].kind == ChangeKind::Removed
                    && pair[0].is_dir.is_none()
                    && pair[1].path.starts_with(&pair[0].path)
            })
            .map(|(i, _)| i)
            .collect();
        for i in dir_like {
            if let Some(change) = changes.get_mut(i) {
                change.is_dir = Some(true);
            }
        }
        // Collapse removed descendants of a removed directory into the directory itself.
        let removed_dirs: HashSet<PathBuf> = changes
            .iter()
            .filter(|c| c.kind == ChangeKind::Removed && c.is_dir == Some(true))
            .map(|c| c.path.clone())
            .collect();
        if !removed_dirs.is_empty() {
            let before = changes.len();
            changes.retain(|c| {
                c.kind != ChangeKind::Removed
                    || !c
                        .path
                        .ancestors()
                        .skip(1)
                        .take_while(|a| a.starts_with(&self.root) && *a != self.root)
                        .any(|a| removed_dirs.contains(a))
            });
            let collapsed = before - changes.len();
            self.counters
                .coalesced
                .fetch_add(collapsed as u64, Ordering::Relaxed);
        }

        if changes.is_empty() {
            None
        } else {
            Some(Flushed::Changes(changes))
        }
    }

    /// True unless the canonical parent directory of `path` exists and is outside the root
    /// (a path reached through a symlink that leaves the workspace).
    fn parent_inside_root(&self, path: &Path, cache: &mut HashMap<PathBuf, bool>) -> bool {
        let Some(parent) = path.parent() else {
            return false;
        };
        if parent == self.root {
            return true;
        }
        if let Some(inside) = cache.get(parent) {
            return *inside;
        }
        let inside = match fs::canonicalize(parent) {
            Ok(canonical) => canonical.starts_with(&self.root),
            // The parent is gone (deleted directory): keep the lexical verdict.
            Err(_) => true,
        };
        cache.insert(parent.to_path_buf(), inside);
        inside
    }
}

fn describe_error(error: &notify::Error) -> String {
    let base = match &error.kind {
        notify::ErrorKind::MaxFilesWatch => {
            "the inotify watch limit was reached (raise fs.inotify.max_user_watches); \
             changes under unwatched directories are not reported"
                .to_string()
        }
        _ => error.to_string(),
    };
    if error.paths.is_empty() {
        base
    } else {
        format!("{base} (paths: {:?})", error.paths)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::File;
    use std::io::Write;

    struct Fixture {
        _dir: tempfile::TempDir,
        root: PathBuf,
        counters: Arc<Counters>,
        c: Coalescer,
    }

    fn fixture(max_pending: usize) -> Fixture {
        fixture_with(max_pending, Arc::new(|_: &Path| false))
    }

    fn fixture_with(max_pending: usize, ignore: IgnoreFn) -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap().join("ws");
        fs::create_dir(&root).unwrap();
        let counters = Arc::new(Counters::default());
        let c = Coalescer::new(root.clone(), max_pending, ignore, counters.clone());
        Fixture {
            _dir: dir,
            root,
            counters,
            c,
        }
    }

    fn touch(path: &Path) {
        File::create(path).unwrap().write_all(b"x").unwrap();
    }

    fn ev(kind: EventKind, paths: &[&Path]) -> notify::Result<Event> {
        let mut event = Event::new(kind);
        for p in paths {
            event = event.add_path(p.to_path_buf());
        }
        Ok(event)
    }

    fn changes(c: &mut Coalescer) -> Vec<FsChange> {
        match c.flush() {
            Some(Flushed::Changes(changes)) => changes,
            other => panic!("expected changes, got {other:?}"),
        }
    }

    const CREATE: EventKind = EventKind::Create(CreateKind::File);
    const MODIFY: EventKind = EventKind::Modify(ModifyKind::Data(notify::event::DataChange::Any));
    const REMOVE: EventKind = EventKind::Remove(RemoveKind::File);
    const FROM: EventKind = EventKind::Modify(ModifyKind::Name(RenameMode::From));
    const TO: EventKind = EventKind::Modify(ModifyKind::Name(RenameMode::To));
    const BOTH: EventKind = EventKind::Modify(ModifyKind::Name(RenameMode::Both));
    const ANY_NAME: EventKind = EventKind::Modify(ModifyKind::Name(RenameMode::Any));

    #[test]
    fn created_then_modified_is_created() {
        let mut f = fixture(100);
        let a = f.root.join("a.rs");
        touch(&a);
        for kind in [CREATE, MODIFY, MODIFY, MODIFY] {
            f.c.push(ev(kind, &[&a]));
        }
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: a,
                kind: ChangeKind::Created,
                is_dir: Some(false)
            }]
        );
        assert_eq!(f.counters.coalesced.load(Ordering::Relaxed), 3);
        assert_eq!(f.c.flush(), None);
    }

    #[test]
    fn modified_then_removed_is_removed_and_removed_then_created_is_modified() {
        let mut f = fixture(100);
        let a = f.root.join("a.rs");
        let b = f.root.join("b.rs");
        touch(&b);
        f.c.push(ev(MODIFY, &[&a]));
        f.c.push(ev(REMOVE, &[&a]));
        f.c.push(ev(REMOVE, &[&b]));
        f.c.push(ev(CREATE, &[&b]));
        let out = changes(&mut f.c);
        assert_eq!(out[0].path, a);
        assert_eq!(out[0].kind, ChangeKind::Removed);
        assert_eq!(out[0].is_dir, Some(false));
        assert_eq!(out[1].path, b);
        assert_eq!(out[1].kind, ChangeKind::Modified);
    }

    #[test]
    fn created_then_removed_is_reported_as_removed() {
        let mut f = fixture(100);
        let a = f.root.join("short-lived");
        f.c.push(ev(CREATE, &[&a]));
        f.c.push(ev(REMOVE, &[&a]));
        let out = changes(&mut f.c);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].kind, ChangeKind::Removed);
    }

    #[test]
    fn inotify_rename_is_paired() {
        let mut f = fixture(100);
        let a = f.root.join("a.rs");
        let b = f.root.join("b.rs");
        touch(&b);
        f.c.push(ev(FROM, &[&a]));
        f.c.push(ev(TO, &[&b]));
        f.c.push(ev(BOTH, &[&a, &b]));
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: b,
                kind: ChangeKind::Renamed { from: a },
                is_dir: Some(false)
            }]
        );
    }

    #[test]
    fn rename_chain_and_swap_back() {
        let mut f = fixture(100);
        let a = f.root.join("a");
        let b = f.root.join("b");
        let c = f.root.join("c");
        touch(&c);
        f.c.push(ev(BOTH, &[&a, &b]));
        f.c.push(ev(BOTH, &[&b, &c]));
        let out = changes(&mut f.c);
        assert!(out.contains(&FsChange {
            path: c.clone(),
            kind: ChangeKind::Renamed { from: a.clone() },
            is_dir: Some(false)
        }));
        assert!(!out.iter().any(|ch| ch.path == a), "{out:?}");

        // a -> b -> a: a was changed, b came and went.
        fs::remove_file(&c).unwrap();
        touch(&a);
        f.c.push(ev(BOTH, &[&a, &b]));
        f.c.push(ev(BOTH, &[&b, &a]));
        let out = changes(&mut f.c);
        assert!(out.contains(&FsChange {
            path: a.clone(),
            kind: ChangeKind::Modified,
            is_dir: Some(false)
        }));
        assert!(
            out.iter()
                .all(|ch| !matches!(ch.kind, ChangeKind::Renamed { .. }))
        );
    }

    #[test]
    fn renamed_then_removed_reports_both_sides_removed() {
        let mut f = fixture(100);
        let a = f.root.join("a");
        let b = f.root.join("b");
        f.c.push(ev(BOTH, &[&a, &b]));
        f.c.push(ev(REMOVE, &[&b]));
        let out = changes(&mut f.c);
        let removed: Vec<_> = out
            .iter()
            .filter(|c| c.kind == ChangeKind::Removed)
            .map(|c| c.path.clone())
            .collect();
        assert_eq!(removed, vec![a, b]);
    }

    #[test]
    fn fsevents_halves_are_paired_by_existence() {
        let mut f = fixture(100);
        let a = f.root.join("old.rs");
        let b = f.root.join("new.rs");
        touch(&b);
        f.c.push(ev(ANY_NAME, &[&a]));
        // Extra flags for the same record do not break the pair.
        f.c.push(ev(MODIFY, &[&a]));
        f.c.push(ev(ANY_NAME, &[&b]));
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: b,
                kind: ChangeKind::Renamed { from: a },
                is_dir: Some(false)
            }]
        );
    }

    #[test]
    fn fsevents_halves_pair_across_cumulative_created_flags() {
        let mut f = fixture(100);
        let a = f.root.join("old.rs");
        let b = f.root.join("new.rs");
        touch(&b);
        // Both records carry a sticky "created" flag before their rename flag.
        f.c.push(ev(CREATE, &[&a]));
        f.c.push(ev(ANY_NAME, &[&a]));
        f.c.push(ev(CREATE, &[&b]));
        f.c.push(ev(ANY_NAME, &[&b]));
        f.c.push(ev(MODIFY, &[&b]));
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: b,
                kind: ChangeKind::Renamed { from: a },
                is_dir: Some(false)
            }]
        );
    }

    #[test]
    fn fsevents_unpaired_halves_degrade_to_removed_and_created() {
        let mut f = fixture(100);
        let a = f.root.join("old.rs");
        let b = f.root.join("new.rs");
        let other = f.root.join("other.rs");
        touch(&b);
        touch(&other);
        f.c.push(ev(ANY_NAME, &[&a]));
        f.c.push(ev(MODIFY, &[&other]));
        f.c.push(ev(ANY_NAME, &[&b]));
        let out = changes(&mut f.c);
        let kinds: Vec<_> = out
            .iter()
            .map(|c| (c.path.clone(), c.kind.clone()))
            .collect();
        assert_eq!(
            kinds,
            vec![
                (b, ChangeKind::Created),
                (a, ChangeKind::Removed),
                (other, ChangeKind::Modified)
            ]
        );
    }

    #[test]
    fn atomic_save_reports_target_only() {
        let mut f = fixture(100);
        let tmp = f.root.join(".titan-save-Ab12Cd");
        let target = f.root.join("a.rs");
        touch(&target);
        // inotify shape
        f.c.push(ev(CREATE, &[&tmp]));
        f.c.push(ev(MODIFY, &[&tmp]));
        f.c.push(ev(FROM, &[&tmp]));
        f.c.push(ev(TO, &[&target]));
        f.c.push(ev(BOTH, &[&tmp, &target]));
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: target.clone(),
                kind: ChangeKind::Created,
                is_dir: Some(false)
            }]
        );
        // FSEvents shape
        f.c.push(ev(CREATE, &[&tmp]));
        f.c.push(ev(ANY_NAME, &[&tmp]));
        f.c.push(ev(ANY_NAME, &[&target]));
        let out = changes(&mut f.c);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].path, target);
        assert!(f.counters.ignored.load(Ordering::Relaxed) >= 6);
    }

    #[test]
    fn overflow_yields_one_rescan_and_drops_detail() {
        let mut f = fixture(10);
        for i in 0..50 {
            f.c.push(ev(CREATE, &[&f.root.join(format!("f{i}"))]));
        }
        assert!(f.c.has_pending());
        match f.c.flush() {
            Some(Flushed::Rescan(reason)) => assert!(reason.contains("10"), "{reason}"),
            other => panic!("expected rescan, got {other:?}"),
        }
        assert_eq!(f.counters.overflows.load(Ordering::Relaxed), 1);
        assert_eq!(f.c.flush(), None);
        // The next window works normally.
        let x = f.root.join("x");
        touch(&x);
        f.c.push(ev(CREATE, &[&x]));
        assert_eq!(changes(&mut f.c).len(), 1);
    }

    #[test]
    fn backend_rescan_flag_and_errors_map_to_rescan() {
        let mut f = fixture(100);
        let a = f.root.join("a");
        f.c.push(ev(MODIFY, &[&a]));
        f.c.push(Ok(Event::new(EventKind::Other)
            .set_flag(notify::event::Flag::Rescan)
            .set_info("rescan: kernel dropped")));
        match f.c.flush() {
            Some(Flushed::Rescan(reason)) => assert!(reason.contains("kernel dropped")),
            other => panic!("expected rescan, got {other:?}"),
        }
        let message =
            f.c.push(Err(notify::Error::new(notify::ErrorKind::MaxFilesWatch)));
        assert!(
            message
                .as_deref()
                .is_some_and(|m| m.contains("max_user_watches"))
        );
        assert!(matches!(f.c.flush(), Some(Flushed::Rescan(_))));
        assert_eq!(f.counters.errors.load(Ordering::Relaxed), 1);
        assert_eq!(f.counters.overflows.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn outside_and_ignored_paths_are_dropped() {
        let ignore: IgnoreFn = Arc::new(|rel: &Path| rel.starts_with("target"));
        let mut f = fixture_with(100, ignore);
        let outside = f.root.parent().unwrap().join("sibling/x.rs");
        let sneaky = f.root.join("../sibling/y.rs");
        let ignored = f.root.join("target/debug/out");
        let swap = f.root.join(".a.rs.swp");
        for p in [&outside, &sneaky, &ignored, &swap] {
            f.c.push(ev(CREATE, &[p]));
        }
        assert!(!f.c.has_pending());
        assert_eq!(f.c.flush(), None);
        assert_eq!(f.counters.outside_root.load(Ordering::Relaxed), 2);
        assert_eq!(f.counters.ignored.load(Ordering::Relaxed), 2);
    }

    #[cfg(unix)]
    #[test]
    fn paths_through_symlinks_leaving_the_root_are_dropped() {
        let mut f = fixture(100);
        let outside = f.root.parent().unwrap().join("outside");
        fs::create_dir(&outside).unwrap();
        touch(&outside.join("secret.txt"));
        std::os::unix::fs::symlink(&outside, f.root.join("link")).unwrap();
        let via_link = f.root.join("link/secret.txt");
        let link = f.root.join("link");
        f.c.push(ev(MODIFY, &[&via_link]));
        f.c.push(ev(CREATE, &[&link]));
        let out = changes(&mut f.c);
        // The link itself is a workspace entry; nothing behind it is reported.
        assert_eq!(
            out,
            vec![FsChange {
                path: link,
                kind: ChangeKind::Created,
                is_dir: Some(false)
            }]
        );
        assert_eq!(f.counters.outside_root.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn removed_directory_collapses_children() {
        let mut f = fixture(100);
        let d = f.root.join("d");
        let x = d.join("x");
        let y = d.join("sub/y");
        f.c.push(ev(REMOVE, &[&x]));
        f.c.push(ev(REMOVE, &[&y]));
        f.c.push(ev(EventKind::Remove(RemoveKind::Any), &[&d.join("sub")]));
        f.c.push(ev(EventKind::Remove(RemoveKind::Any), &[&d]));
        let out = changes(&mut f.c);
        assert_eq!(
            out,
            vec![FsChange {
                path: d,
                kind: ChangeKind::Removed,
                is_dir: Some(true)
            }]
        );
    }

    #[test]
    fn root_removal_requests_rescan() {
        let mut f = fixture(100);
        let root = f.root.clone();
        f.c.push(ev(EventKind::Remove(RemoveKind::Folder), &[&root]));
        assert!(matches!(f.c.flush(), Some(Flushed::Rescan(_))));
        // Root metadata changes are ignored.
        f.c.push(ev(MODIFY, &[&root]));
        assert_eq!(f.c.flush(), None);
    }
}
