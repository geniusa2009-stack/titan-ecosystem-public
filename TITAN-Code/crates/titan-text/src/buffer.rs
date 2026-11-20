use std::ops::Range;

use ropey::Rope;
use unicode_segmentation::UnicodeSegmentation;

use crate::coords::{ByteOffset, CharOffset, Point, Utf16Offset, Utf16Point};
use crate::edit::{AppliedEdits, TextEdit};
use crate::error::TextError;

/// Rope-backed text buffer.
///
/// Complexity (ropey B-tree, n = text length): offset conversions, line lookups and
/// single-range insert/delete are O(log n). `clone` is O(1) and shares structure, so a clone
/// is an immutable snapshot that later edits do not affect.
///
/// Lines are separated by LF, CRLF or a lone CR (the LSP definition). A text ending in a
/// line break has an empty last line, so `"a\n"` has two lines.
#[derive(Clone, Debug, Default)]
pub struct TextBuffer {
    rope: Rope,
}

impl TextBuffer {
    pub fn new(text: &str) -> Self {
        Self {
            rope: Rope::from_str(text),
        }
    }

    pub fn len_bytes(&self) -> usize {
        self.rope.len_bytes()
    }

    pub fn len_chars(&self) -> usize {
        self.rope.len_chars()
    }

    pub fn len_utf16(&self) -> usize {
        self.rope.len_utf16_cu()
    }

    pub fn line_count(&self) -> usize {
        self.rope.len_lines()
    }

    pub fn is_empty(&self) -> bool {
        self.rope.len_bytes() == 0
    }

    /// The full text as a contiguous `String` (O(n); use for reads, not on edit paths).
    pub fn text(&self) -> String {
        let mut out = String::with_capacity(self.rope.len_bytes());
        for chunk in self.rope.chunks() {
            out.push_str(chunk);
        }
        out
    }

    /// Iterate the text as borrowed chunks without copying it.
    pub fn chunks(&self) -> impl Iterator<Item = &str> + '_ {
        self.rope.chunks()
    }

    /// The chunk that contains `byte` and the byte offset where it starts. Returns `("", len)`
    /// at or past the end. Lets parsers read the rope without copying the document.
    pub fn chunk_at_byte(&self, byte: usize) -> (&str, usize) {
        let len = self.rope.len_bytes();
        if byte >= len {
            return ("", len);
        }
        let (chunk, start, _, _) = self.rope.chunk_at_byte(byte);
        (chunk, start)
    }

    /// Copy out the text in a validated byte range.
    pub fn slice(&self, range: Range<ByteOffset>) -> Result<String, TextError> {
        self.check_range(range.start, range.end)?;
        let start = self.rope.byte_to_char(range.start.0);
        let end = self.rope.byte_to_char(range.end.0);
        let mut out = String::with_capacity(range.end.0 - range.start.0);
        for chunk in self.rope.slice(start..end).chunks() {
            out.push_str(chunk);
        }
        Ok(out)
    }

    /// Text of one line, without its line terminator.
    pub fn line_text(&self, line: usize) -> Result<String, TextError> {
        let (start, end) = self.line_bounds(line)?;
        self.slice(ByteOffset(start)..ByteOffset(end))
    }

    // ----- validation -------------------------------------------------------------------

    /// Ok if `offset` is within the text and on a UTF-8 character boundary.
    pub fn check_offset(&self, offset: ByteOffset) -> Result<(), TextError> {
        let len = self.rope.len_bytes();
        if offset.0 > len {
            return Err(TextError::OutOfBounds {
                unit: "byte",
                offset: offset.0,
                len,
            });
        }
        let char_idx = self.rope.byte_to_char(offset.0);
        if self.rope.char_to_byte(char_idx) != offset.0 {
            return Err(TextError::NotCharBoundary { offset: offset.0 });
        }
        Ok(())
    }

    pub fn check_range(&self, start: ByteOffset, end: ByteOffset) -> Result<(), TextError> {
        if start > end {
            return Err(TextError::InvertedRange {
                start: start.0,
                end: end.0,
            });
        }
        self.check_offset(start)?;
        self.check_offset(end)
    }

    // ----- unit conversions -------------------------------------------------------------

    pub fn byte_to_char(&self, offset: ByteOffset) -> Result<CharOffset, TextError> {
        self.check_offset(offset)?;
        Ok(CharOffset(self.rope.byte_to_char(offset.0)))
    }

    pub fn char_to_byte(&self, offset: CharOffset) -> Result<ByteOffset, TextError> {
        let len = self.rope.len_chars();
        if offset.0 > len {
            return Err(TextError::OutOfBounds {
                unit: "char",
                offset: offset.0,
                len,
            });
        }
        Ok(ByteOffset(self.rope.char_to_byte(offset.0)))
    }

    pub fn byte_to_utf16(&self, offset: ByteOffset) -> Result<Utf16Offset, TextError> {
        self.check_offset(offset)?;
        let char_idx = self.rope.byte_to_char(offset.0);
        Ok(Utf16Offset(self.rope.char_to_utf16_cu(char_idx)))
    }

    pub fn utf16_to_byte(&self, offset: Utf16Offset) -> Result<ByteOffset, TextError> {
        let len = self.rope.len_utf16_cu();
        if offset.0 > len {
            return Err(TextError::OutOfBounds {
                unit: "UTF-16",
                offset: offset.0,
                len,
            });
        }
        let char_idx = self.rope.utf16_cu_to_char(offset.0);
        if self.rope.char_to_utf16_cu(char_idx) != offset.0 {
            return Err(TextError::SplitsSurrogatePair { offset: offset.0 });
        }
        Ok(ByteOffset(self.rope.char_to_byte(char_idx)))
    }

    pub fn byte_to_point(&self, offset: ByteOffset) -> Result<Point, TextError> {
        self.check_offset(offset)?;
        let line = self.rope.byte_to_line(offset.0);
        let line_start = self.rope.line_to_byte(line);
        Ok(Point {
            line,
            column: offset.0 - line_start,
        })
    }

    pub fn point_to_byte(&self, point: Point) -> Result<ByteOffset, TextError> {
        let (start, end) = self.line_bounds(point.line)?;
        let line_len = end - start;
        if point.column > line_len {
            return Err(TextError::ColumnOutOfBounds {
                line: point.line,
                column: point.column,
                line_len,
            });
        }
        let offset = ByteOffset(start + point.column);
        self.check_offset(offset)?;
        Ok(offset)
    }

    pub fn byte_to_utf16_point(&self, offset: ByteOffset) -> Result<Utf16Point, TextError> {
        self.check_offset(offset)?;
        let line = self.rope.byte_to_line(offset.0);
        let line_start_char = self.rope.line_to_char(line);
        let char_idx = self.rope.byte_to_char(offset.0);
        let column =
            self.rope.char_to_utf16_cu(char_idx) - self.rope.char_to_utf16_cu(line_start_char);
        Ok(Utf16Point { line, column })
    }

    pub fn utf16_point_to_byte(&self, point: Utf16Point) -> Result<ByteOffset, TextError> {
        let (start, end) = self.line_bounds(point.line)?;
        let start_u16 = self.byte_to_utf16(ByteOffset(start))?.0;
        let end_u16 = self.byte_to_utf16(ByteOffset(end))?.0;
        let line_len = end_u16 - start_u16;
        if point.column > line_len {
            return Err(TextError::ColumnOutOfBounds {
                line: point.line,
                column: point.column,
                line_len,
            });
        }
        self.utf16_to_byte(Utf16Offset(start_u16 + point.column))
    }

    /// Byte range of `line`'s content, excluding its terminator.
    fn line_bounds(&self, line: usize) -> Result<(usize, usize), TextError> {
        let line_count = self.rope.len_lines();
        if line >= line_count {
            return Err(TextError::LineOutOfBounds { line, line_count });
        }
        let start = self.rope.line_to_byte(line);
        let mut end = self.line_end_with_terminator(line, line_count);
        if end > start && self.rope.byte(end - 1) == b'\n' {
            end -= 1;
        }
        if end > start && self.rope.byte(end - 1) == b'\r' {
            end -= 1;
        }
        Ok((start, end))
    }

    fn line_end_with_terminator(&self, line: usize, line_count: usize) -> usize {
        if line + 1 < line_count {
            self.rope.line_to_byte(line + 1)
        } else {
            self.rope.len_bytes()
        }
    }

    // ----- grapheme clusters ------------------------------------------------------------

    /// True if `offset` is an extended grapheme cluster boundary.
    ///
    /// Cost is proportional to the length of the containing line.
    pub fn is_grapheme_boundary(&self, offset: ByteOffset) -> Result<bool, TextError> {
        let (line_start, line_text) = self.line_with_terminator(offset)?;
        let rel = offset.0 - line_start;
        if rel == 0 || rel == line_text.len() {
            return Ok(true);
        }
        Ok(line_text.grapheme_indices(true).any(|(i, _)| i == rel))
    }

    /// The next grapheme boundary strictly after `offset` (or the end of the text).
    pub fn next_grapheme_boundary(&self, offset: ByteOffset) -> Result<ByteOffset, TextError> {
        let (line_start, line_text) = self.line_with_terminator(offset)?;
        let rel = offset.0 - line_start;
        let next = line_text
            .grapheme_indices(true)
            .map(|(i, g)| i + g.len())
            .find(|&end| end > rel)
            .unwrap_or(line_text.len());
        Ok(ByteOffset(line_start + next))
    }

    /// The previous grapheme boundary strictly before `offset` (or the start of the text).
    pub fn prev_grapheme_boundary(&self, offset: ByteOffset) -> Result<ByteOffset, TextError> {
        self.check_offset(offset)?;
        if offset.0 == 0 {
            return Ok(offset);
        }
        let (line_start, line_text) = self.line_with_terminator(offset)?;
        let rel = offset.0 - line_start;
        if rel == 0 {
            // Step back into the previous line and take its last boundary.
            let prev_line_end = ByteOffset(offset.0 - 1);
            let (prev_start, prev_text) = self.line_with_terminator(prev_line_end)?;
            let last = prev_text
                .grapheme_indices(true)
                .map(|(i, _)| i)
                .next_back()
                .unwrap_or(0);
            return Ok(ByteOffset(prev_start + last));
        }
        let prev = line_text
            .grapheme_indices(true)
            .map(|(i, _)| i)
            .take_while(|&i| i < rel)
            .last()
            .unwrap_or(0);
        Ok(ByteOffset(line_start + prev))
    }

    /// Start byte and full text (including terminator) of the line containing `offset`.
    fn line_with_terminator(&self, offset: ByteOffset) -> Result<(usize, String), TextError> {
        self.check_offset(offset)?;
        let line = self.rope.byte_to_line(offset.0);
        let start = self.rope.line_to_byte(line);
        let end = self.line_end_with_terminator(line, self.rope.len_lines());
        let text = self.slice(ByteOffset(start)..ByteOffset(end))?;
        Ok((start, text))
    }

    // ----- editing ----------------------------------------------------------------------

    /// Apply a set of edits atomically. All ranges refer to the text before the change.
    ///
    /// Edits are validated (bounds, UTF-8 boundaries, ordering) before anything is
    /// modified; on error the buffer is unchanged. Edits are sorted by start; edits that
    /// overlap are rejected. Several insertions at the same offset are kept in request order.
    /// Application runs from the highest offset down so earlier ranges stay valid.
    pub fn apply(&mut self, mut edits: Vec<TextEdit>) -> Result<AppliedEdits, TextError> {
        for edit in &edits {
            self.check_range(edit.start, edit.end)?;
        }
        // Stable sort: equal keys (e.g. two inserts at one offset) keep request order.
        edits.sort_by_key(|e| (e.start, e.end));
        for pair in edits.windows(2) {
            if pair[0].end > pair[1].start {
                return Err(TextError::OverlappingEdits {
                    offset: pair[1].start.0,
                });
            }
        }

        let deleted: Vec<String> = edits
            .iter()
            .map(|e| self.slice(e.start..e.end))
            .collect::<Result<_, _>>()?;

        let mut inverse = Vec::with_capacity(edits.len());
        let mut delta: i64 = 0;
        for (edit, old_text) in edits.iter().zip(deleted) {
            let new_start = shift(edit.start.0, delta);
            inverse.push(TextEdit::replace(
                ByteOffset(new_start),
                ByteOffset(new_start + edit.text.len()),
                old_text,
            ));
            delta += edit.text.len() as i64 - edit.removed_len() as i64;
        }

        for edit in edits.iter().rev() {
            let start = self.rope.byte_to_char(edit.start.0);
            let end = self.rope.byte_to_char(edit.end.0);
            if end > start {
                self.rope.remove(start..end);
            }
            if !edit.text.is_empty() {
                self.rope.insert(start, &edit.text);
            }
        }

        Ok(AppliedEdits {
            forward: edits,
            inverse,
        })
    }
}

fn shift(value: usize, delta: i64) -> usize {
    (value as i64 + delta) as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b(n: usize) -> ByteOffset {
        ByteOffset(n)
    }

    #[test]
    fn empty_buffer() {
        let buf = TextBuffer::new("");
        assert!(buf.is_empty());
        assert_eq!(buf.line_count(), 1);
        assert_eq!(buf.line_text(0).unwrap(), "");
        assert_eq!(buf.point_to_byte(Point::new(0, 0)).unwrap(), b(0));
        assert!(buf.point_to_byte(Point::new(0, 1)).is_err());
        assert!(buf.point_to_byte(Point::new(1, 0)).is_err());
    }

    #[test]
    fn lines_and_terminators() {
        let buf = TextBuffer::new("a\r\nbc\rd\n");
        assert_eq!(buf.line_count(), 4);
        assert_eq!(buf.line_text(0).unwrap(), "a");
        assert_eq!(buf.line_text(1).unwrap(), "bc");
        assert_eq!(buf.line_text(2).unwrap(), "d");
        assert_eq!(buf.line_text(3).unwrap(), "");
        assert_eq!(buf.byte_to_point(b(3)).unwrap(), Point::new(1, 0));
        assert_eq!(buf.point_to_byte(Point::new(2, 1)).unwrap(), b(7));
        // Column past the content (into the terminator) is rejected.
        assert!(matches!(
            buf.point_to_byte(Point::new(0, 2)),
            Err(TextError::ColumnOutOfBounds { .. })
        ));
    }

    #[test]
    fn arabic_and_emoji_units() {
        // "سلام" = 4 chars x 2 bytes; "😀" = 4 bytes, 2 UTF-16 units.
        let text = "x سلام 😀 y";
        let buf = TextBuffer::new(text);
        assert_eq!(buf.len_bytes(), text.len());
        assert_eq!(buf.len_chars(), text.chars().count());
        assert_eq!(buf.len_utf16(), text.encode_utf16().count());

        let emoji_byte = text.find('😀').unwrap();
        assert_eq!(
            buf.byte_to_utf16(b(emoji_byte)).unwrap().0,
            text[..emoji_byte].encode_utf16().count()
        );
        // Inside the 4-byte emoji is not a boundary.
        assert_eq!(
            buf.check_offset(b(emoji_byte + 1)),
            Err(TextError::NotCharBoundary {
                offset: emoji_byte + 1
            })
        );
        // Inside the surrogate pair is rejected.
        let emoji_u16 = buf.byte_to_utf16(b(emoji_byte)).unwrap().0;
        assert_eq!(
            buf.utf16_to_byte(Utf16Offset(emoji_u16 + 1)),
            Err(TextError::SplitsSurrogatePair {
                offset: emoji_u16 + 1
            })
        );
        assert_eq!(
            buf.utf16_to_byte(Utf16Offset(emoji_u16 + 2)).unwrap(),
            b(emoji_byte + 4)
        );
    }

    #[test]
    fn utf16_points_round_trip() {
        let text = "line one\nمرحبا 👋🏽 world\nend";
        let buf = TextBuffer::new(text);
        for (offset, _) in text.char_indices().chain([(text.len(), ' ')]) {
            let p = buf.byte_to_utf16_point(b(offset)).unwrap();
            assert_eq!(buf.utf16_point_to_byte(p).unwrap(), b(offset));
            let q = buf.byte_to_point(b(offset)).unwrap();
            assert_eq!(buf.point_to_byte(q).unwrap(), b(offset));
        }
    }

    #[test]
    fn graphemes() {
        // e + combining acute, then a ZWJ family emoji, then CRLF.
        let text = "e\u{301}👨\u{200d}👩\u{200d}👧!\r\nz";
        let buf = TextBuffer::new(text);
        assert!(buf.is_grapheme_boundary(b(0)).unwrap());
        assert!(!buf.is_grapheme_boundary(b(1)).unwrap());
        let family = 3;
        assert!(buf.is_grapheme_boundary(b(family)).unwrap());
        assert_eq!(buf.next_grapheme_boundary(b(0)).unwrap(), b(family));
        let bang = text.find('!').unwrap();
        assert_eq!(buf.next_grapheme_boundary(b(family)).unwrap(), b(bang));
        assert_eq!(buf.prev_grapheme_boundary(b(bang)).unwrap(), b(family));
        // CR LF is one grapheme cluster.
        assert!(!buf.is_grapheme_boundary(b(bang + 2)).unwrap());
        // Moving back from the start of line 1 lands before the CR LF cluster.
        assert_eq!(
            buf.prev_grapheme_boundary(b(bang + 3)).unwrap(),
            b(bang + 1)
        );
    }

    #[test]
    fn multi_edit_apply_and_inverse() {
        let mut buf = TextBuffer::new("hello world");
        let applied = buf
            .apply(vec![
                TextEdit::replace(b(6), b(11), "TITAN"),
                TextEdit::insert(b(0), ">> "),
            ])
            .unwrap();
        assert_eq!(buf.text(), ">> hello TITAN");
        assert_eq!(applied.forward[0].start, b(0));
        buf.apply(applied.inverse).unwrap();
        assert_eq!(buf.text(), "hello world");
    }

    #[test]
    fn inserts_at_same_offset_keep_request_order() {
        let mut buf = TextBuffer::new("x");
        buf.apply(vec![
            TextEdit::insert(b(0), "a"),
            TextEdit::insert(b(0), "b"),
        ])
        .unwrap();
        assert_eq!(buf.text(), "abx");
    }

    #[test]
    fn rejects_bad_edits_without_modifying() {
        let mut buf = TextBuffer::new("héllo");
        let before = buf.text();
        assert!(matches!(
            buf.apply(vec![TextEdit::delete(b(0), b(10))]),
            Err(TextError::OutOfBounds { .. })
        ));
        assert!(matches!(
            buf.apply(vec![TextEdit::delete(b(2), b(3))]),
            Err(TextError::NotCharBoundary { .. })
        ));
        assert!(matches!(
            buf.apply(vec![TextEdit::delete(b(3), b(1))]),
            Err(TextError::InvertedRange { .. })
        ));
        assert!(matches!(
            buf.apply(vec![
                TextEdit::delete(b(0), b(3)),
                TextEdit::delete(b(1), b(4))
            ]),
            Err(TextError::OverlappingEdits { .. })
        ));
        assert_eq!(buf.text(), before);
    }

    #[test]
    fn clone_is_an_independent_snapshot() {
        let mut buf = TextBuffer::new("abc");
        let snap = buf.clone();
        buf.apply(vec![TextEdit::insert(b(3), "def")]).unwrap();
        assert_eq!(snap.text(), "abc");
        assert_eq!(buf.text(), "abcdef");
    }

    #[test]
    fn very_long_line() {
        let line = "ab".repeat(200_000);
        let mut buf = TextBuffer::new(&line);
        assert_eq!(buf.line_count(), 1);
        buf.apply(vec![TextEdit::insert(b(200_000), "\n")]).unwrap();
        assert_eq!(buf.line_count(), 2);
        assert_eq!(buf.byte_to_point(b(200_001)).unwrap(), Point::new(1, 0));
    }
}
