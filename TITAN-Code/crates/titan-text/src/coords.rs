//! Explicit coordinate units.
//!
//! A UTF-8 byte, a Unicode scalar value (char), a UTF-16 code unit and a grapheme cluster
//! are different units. Each gets its own type so they cannot be mixed by accident.
//! `ByteOffset` is the canonical internal unit.

/// Offset in UTF-8 bytes from the start of the text.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ByteOffset(pub usize);

/// Offset in Unicode scalar values (Rust `char`s) from the start of the text.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CharOffset(pub usize);

/// Offset in UTF-16 code units from the start of the text (LSP's default unit).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Utf16Offset(pub usize);

/// Zero-based line and UTF-8 byte column within that line.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Point {
    pub line: usize,
    pub column: usize,
}

/// Zero-based line and UTF-16 code-unit column within that line.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Utf16Point {
    pub line: usize,
    pub column: usize,
}

impl Point {
    pub fn new(line: usize, column: usize) -> Self {
        Self { line, column }
    }
}

impl Utf16Point {
    pub fn new(line: usize, column: usize) -> Self {
        Self { line, column }
    }
}
