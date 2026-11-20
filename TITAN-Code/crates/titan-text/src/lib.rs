//! TITAN Code text core.
//!
//! * [`TextBuffer`]: a rope (ropey) with validated conversions between explicit coordinate
//!   units. Cloning a buffer is O(1) and clones share structure, which is what makes
//!   snapshots cheap.
//! * [`TextEdit`]: a byte-range replacement. A transaction is a list of non-overlapping
//!   edits expressed against the same base revision.
//! * [`TextDocument`]: buffer + monotonic revision + undo/redo history. Every mutation names
//!   the revision it was computed against; stale mutations are rejected.
//!
//! This crate performs no I/O and has no async runtime.

mod buffer;
mod coords;
mod document;
mod edit;
mod error;

pub use buffer::TextBuffer;
pub use coords::{ByteOffset, CharOffset, Point, Utf16Offset, Utf16Point};
pub use document::{Change, ChangeKind, HistoryLimits, TextDocument, TextSnapshot};
pub use edit::{AppliedEdits, TextEdit};
pub use error::TextError;
