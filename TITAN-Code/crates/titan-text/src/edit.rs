use crate::coords::ByteOffset;

/// Replace the bytes in `start..end` with `text`.
///
/// An insertion has `start == end`; a deletion has an empty `text`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextEdit {
    pub start: ByteOffset,
    pub end: ByteOffset,
    pub text: String,
}

impl TextEdit {
    pub fn replace(start: ByteOffset, end: ByteOffset, text: impl Into<String>) -> Self {
        Self {
            start,
            end,
            text: text.into(),
        }
    }

    pub fn insert(at: ByteOffset, text: impl Into<String>) -> Self {
        Self::replace(at, at, text)
    }

    pub fn delete(start: ByteOffset, end: ByteOffset) -> Self {
        Self::replace(start, end, String::new())
    }

    /// Number of bytes removed by this edit.
    pub fn removed_len(&self) -> usize {
        self.end.0.saturating_sub(self.start.0)
    }
}

/// The outcome of applying one transaction to a buffer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AppliedEdits {
    /// The applied edits, sorted by start, in coordinates of the text *before* the change.
    pub forward: Vec<TextEdit>,
    /// Edits that undo the change, sorted, in coordinates of the text *after* the change.
    pub inverse: Vec<TextEdit>,
}
