/// Errors produced by the text core. None of these leave the buffer modified.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum TextError {
    #[error("{unit} offset {offset} is beyond the end of the text (length {len})")]
    OutOfBounds {
        unit: &'static str,
        offset: usize,
        len: usize,
    },
    #[error("byte offset {offset} is not on a UTF-8 character boundary")]
    NotCharBoundary { offset: usize },
    #[error("UTF-16 offset {offset} falls inside a surrogate pair")]
    SplitsSurrogatePair { offset: usize },
    #[error("line {line} does not exist (the text has {line_count} lines)")]
    LineOutOfBounds { line: usize, line_count: usize },
    #[error("column {column} is past the end of line {line} (line length {line_len})")]
    ColumnOutOfBounds {
        line: usize,
        column: usize,
        line_len: usize,
    },
    #[error("range start {start} is after its end {end}")]
    InvertedRange { start: usize, end: usize },
    #[error("edits overlap at byte {offset}")]
    OverlappingEdits { offset: usize },
    #[error("expected revision {expected}, but the document is at revision {current}")]
    StaleRevision { expected: u64, current: u64 },
}
