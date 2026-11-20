use std::collections::VecDeque;

use crate::buffer::TextBuffer;
use crate::edit::TextEdit;
use crate::error::TextError;

/// Why a revision was produced.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChangeKind {
    Edit,
    Undo,
    Redo,
}

/// One committed change: what moved the document from `previous_revision` to `revision`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Change {
    pub kind: ChangeKind,
    pub previous_revision: u64,
    pub revision: u64,
    /// Applied edits, sorted, in coordinates of the text at `previous_revision`.
    pub edits: Vec<TextEdit>,
}

/// An immutable view of a document at one revision. Cheap to create (O(1)) and unaffected
/// by later edits, so background work (search, indexing) can hold one while edits continue.
#[derive(Clone, Debug)]
pub struct TextSnapshot {
    revision: u64,
    buffer: TextBuffer,
}

impl TextSnapshot {
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn buffer(&self) -> &TextBuffer {
        &self.buffer
    }
}

#[derive(Clone, Debug)]
struct HistoryEntry {
    forward: Vec<TextEdit>,
    inverse: Vec<TextEdit>,
    content_before: u64,
    content_after: u64,
    /// Inserted plus deleted text held by this entry, for the memory budget.
    bytes: usize,
}

/// Bounds on undo/redo memory. When an edit pushes the history over either bound, the
/// oldest undo entries are dropped (the newest entry is always kept).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HistoryLimits {
    pub max_entries: usize,
    pub max_bytes: usize,
}

impl Default for HistoryLimits {
    fn default() -> Self {
        Self {
            max_entries: 10_000,
            max_bytes: 64 * 1024 * 1024,
        }
    }
}

/// A text buffer with revision tracking and linear undo/redo.
///
/// * `revision` increases by exactly one for every committed edit, undo or redo and never
///   goes back. Every mutation must name the revision it was computed against.
/// * `content_id` identifies the *content state*: a new edit creates a fresh id, undo/redo
///   restore the id of the state they return to. Comparing it to the id recorded at save
///   time answers "are there unsaved changes" in O(1), including after undo back to the
///   saved state (the role of VS Code's `alternativeVersionId`).
#[derive(Clone, Debug)]
pub struct TextDocument {
    buffer: TextBuffer,
    revision: u64,
    content_id: u64,
    next_content_id: u64,
    undo_stack: VecDeque<HistoryEntry>,
    redo_stack: Vec<HistoryEntry>,
    history_bytes: usize,
    limits: HistoryLimits,
}

impl TextDocument {
    pub fn new(text: &str) -> Self {
        Self::with_limits(text, HistoryLimits::default())
    }

    pub fn with_limits(text: &str, limits: HistoryLimits) -> Self {
        Self {
            buffer: TextBuffer::new(text),
            revision: 0,
            content_id: 0,
            next_content_id: 1,
            undo_stack: VecDeque::new(),
            redo_stack: Vec::new(),
            history_bytes: 0,
            limits,
        }
    }

    /// Bytes of edit text currently held by undo and redo history.
    pub fn history_bytes(&self) -> usize {
        self.history_bytes
    }

    pub fn undo_depth(&self) -> usize {
        self.undo_stack.len()
    }

    fn enforce_limits(&mut self) {
        while self.undo_stack.len() > 1
            && (self.undo_stack.len() > self.limits.max_entries
                || self.history_bytes > self.limits.max_bytes)
        {
            if let Some(dropped) = self.undo_stack.pop_front() {
                self.history_bytes -= dropped.bytes;
            }
        }
    }

    pub fn buffer(&self) -> &TextBuffer {
        &self.buffer
    }

    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn content_id(&self) -> u64 {
        self.content_id
    }

    pub fn can_undo(&self) -> bool {
        !self.undo_stack.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo_stack.is_empty()
    }

    pub fn snapshot(&self) -> TextSnapshot {
        TextSnapshot {
            revision: self.revision,
            buffer: self.buffer.clone(),
        }
    }

    fn check_revision(&self, expected: u64) -> Result<(), TextError> {
        if expected != self.revision {
            return Err(TextError::StaleRevision {
                expected,
                current: self.revision,
            });
        }
        Ok(())
    }

    /// Apply a transaction computed against `expected_revision`.
    ///
    /// An empty edit list is accepted and changes nothing (no new revision).
    pub fn apply(
        &mut self,
        expected_revision: u64,
        edits: Vec<TextEdit>,
    ) -> Result<Change, TextError> {
        self.check_revision(expected_revision)?;
        let previous_revision = self.revision;
        if edits.is_empty() {
            return Ok(Change {
                kind: ChangeKind::Edit,
                previous_revision,
                revision: previous_revision,
                edits,
            });
        }
        let applied = self.buffer.apply(edits)?;
        let content_before = self.content_id;
        self.content_id = self.next_content_id;
        self.next_content_id += 1;
        self.revision += 1;
        let redo_bytes: usize = self.redo_stack.iter().map(|e| e.bytes).sum();
        self.history_bytes -= redo_bytes;
        self.redo_stack.clear();
        let bytes = applied
            .forward
            .iter()
            .chain(&applied.inverse)
            .map(|e| e.text.len())
            .sum();
        self.history_bytes += bytes;
        self.undo_stack.push_back(HistoryEntry {
            forward: applied.forward.clone(),
            inverse: applied.inverse,
            content_before,
            content_after: self.content_id,
            bytes,
        });
        self.enforce_limits();
        Ok(Change {
            kind: ChangeKind::Edit,
            previous_revision,
            revision: self.revision,
            edits: applied.forward,
        })
    }

    /// Undo the most recent transaction. `Ok(None)` if there is nothing to undo.
    pub fn undo(&mut self, expected_revision: u64) -> Result<Option<Change>, TextError> {
        self.check_revision(expected_revision)?;
        let Some(entry) = self.undo_stack.pop_back() else {
            return Ok(None);
        };
        let applied = match self.buffer.apply(entry.inverse.clone()) {
            Ok(applied) => applied,
            Err(err) => {
                self.undo_stack.push_back(entry);
                return Err(err);
            }
        };
        let previous_revision = self.revision;
        self.revision += 1;
        self.content_id = entry.content_before;
        self.redo_stack.push(entry);
        Ok(Some(Change {
            kind: ChangeKind::Undo,
            previous_revision,
            revision: self.revision,
            edits: applied.forward,
        }))
    }

    /// Redo the most recently undone transaction. `Ok(None)` if there is nothing to redo.
    pub fn redo(&mut self, expected_revision: u64) -> Result<Option<Change>, TextError> {
        self.check_revision(expected_revision)?;
        let Some(entry) = self.redo_stack.pop() else {
            return Ok(None);
        };
        let applied = match self.buffer.apply(entry.forward.clone()) {
            Ok(applied) => applied,
            Err(err) => {
                self.redo_stack.push(entry);
                return Err(err);
            }
        };
        let previous_revision = self.revision;
        self.revision += 1;
        self.content_id = entry.content_after;
        self.undo_stack.push_back(entry);
        Ok(Some(Change {
            kind: ChangeKind::Redo,
            previous_revision,
            revision: self.revision,
            edits: applied.forward,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::coords::ByteOffset;

    #[test]
    fn edit_undo_redo_round_trip() {
        let mut doc = TextDocument::new("fn main() {}\n");
        let saved = doc.content_id();
        let change = doc
            .apply(
                0,
                vec![TextEdit::insert(ByteOffset(11), " println!(\"مرحبا\"); ")],
            )
            .unwrap();
        assert_eq!(change.revision, 1);
        assert_eq!(doc.buffer().text(), "fn main() { println!(\"مرحبا\"); }\n");
        assert_ne!(doc.content_id(), saved);

        let undo = doc.undo(1).unwrap().unwrap();
        assert_eq!(undo.kind, ChangeKind::Undo);
        assert_eq!(doc.revision(), 2);
        assert_eq!(doc.buffer().text(), "fn main() {}\n");
        assert_eq!(doc.content_id(), saved);

        doc.redo(2).unwrap().unwrap();
        assert_eq!(doc.revision(), 3);
        assert_eq!(doc.buffer().text(), "fn main() { println!(\"مرحبا\"); }\n");
        assert_eq!(doc.redo(3).unwrap(), None);
    }

    #[test]
    fn stale_revision_is_rejected_without_change() {
        let mut doc = TextDocument::new("abc");
        doc.apply(0, vec![TextEdit::insert(ByteOffset(3), "d")])
            .unwrap();
        let err = doc
            .apply(0, vec![TextEdit::delete(ByteOffset(0), ByteOffset(1))])
            .unwrap_err();
        assert_eq!(
            err,
            TextError::StaleRevision {
                expected: 0,
                current: 1
            }
        );
        assert_eq!(doc.buffer().text(), "abcd");
        assert!(doc.undo(0).is_err());
        assert!(doc.redo(5).is_err());
    }

    #[test]
    fn new_edit_clears_redo() {
        let mut doc = TextDocument::new("");
        doc.apply(0, vec![TextEdit::insert(ByteOffset(0), "a")])
            .unwrap();
        doc.undo(1).unwrap();
        assert!(doc.can_redo());
        doc.apply(2, vec![TextEdit::insert(ByteOffset(0), "b")])
            .unwrap();
        assert!(!doc.can_redo());
        assert_eq!(doc.buffer().text(), "b");
    }

    #[test]
    fn snapshot_survives_later_edits() {
        let mut doc = TextDocument::new("one");
        let snap = doc.snapshot();
        doc.apply(0, vec![TextEdit::insert(ByteOffset(3), " two")])
            .unwrap();
        assert_eq!(snap.revision(), 0);
        assert_eq!(snap.buffer().text(), "one");
        assert_eq!(doc.snapshot().buffer().text(), "one two");
    }

    #[test]
    fn history_is_bounded_by_entries_and_bytes() {
        let limits = HistoryLimits {
            max_entries: 3,
            max_bytes: 1_000,
        };
        let mut doc = TextDocument::with_limits("", limits);
        for i in 0..10 {
            let rev = doc.revision();
            doc.apply(rev, vec![TextEdit::insert(ByteOffset(0), i.to_string())])
                .unwrap();
        }
        assert_eq!(doc.undo_depth(), 3);
        assert_eq!(doc.history_bytes(), 3);
        for _ in 0..3 {
            let rev = doc.revision();
            assert!(doc.undo(rev).unwrap().is_some());
        }
        let rev = doc.revision();
        assert_eq!(doc.undo(rev).unwrap(), None);
        assert_eq!(doc.buffer().text(), "6543210");

        // One huge edit over the byte budget is still undoable on its own.
        let mut doc = TextDocument::with_limits("", limits);
        doc.apply(0, vec![TextEdit::insert(ByteOffset(0), "small")])
            .unwrap();
        doc.apply(1, vec![TextEdit::insert(ByteOffset(0), "x".repeat(5_000))])
            .unwrap();
        assert_eq!(doc.undo_depth(), 1);
        assert_eq!(doc.history_bytes(), 5_000);
        doc.undo(2).unwrap();
        assert_eq!(doc.buffer().text(), "small");
        // A new edit after undo releases the redo entry's bytes.
        doc.apply(3, vec![TextEdit::insert(ByteOffset(0), "!")])
            .unwrap();
        assert_eq!(doc.history_bytes(), 1);
    }

    #[test]
    fn empty_transaction_keeps_revision() {
        let mut doc = TextDocument::new("x");
        let change = doc.apply(0, Vec::new()).unwrap();
        assert_eq!(change.revision, 0);
        assert!(!doc.can_undo());
    }

    #[test]
    fn repeated_undo_redo_is_stable() {
        let mut doc = TextDocument::new("0");
        for i in 0..50 {
            let rev = doc.revision();
            let end = ByteOffset(doc.buffer().len_bytes());
            doc.apply(rev, vec![TextEdit::insert(end, format!(",{i}"))])
                .unwrap();
        }
        let full = doc.buffer().text();
        for _ in 0..50 {
            let rev = doc.revision();
            doc.undo(rev).unwrap().unwrap();
        }
        assert_eq!(doc.buffer().text(), "0");
        for _ in 0..50 {
            let rev = doc.revision();
            doc.redo(rev).unwrap().unwrap();
        }
        assert_eq!(doc.buffer().text(), full);
        assert_eq!(doc.revision(), 150);
    }
}
