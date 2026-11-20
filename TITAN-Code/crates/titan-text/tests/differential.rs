//! Differential tests: TITAN's rope-based document against a naive `String` reference.
//! Seeds are fixed so every failure is reproducible.

use titan_text::{ByteOffset, TextDocument, TextEdit, Utf16Point};

/// xorshift64*: tiny deterministic PRNG so the test needs no extra dependency.
struct Rng(u64);

impl Rng {
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            (self.next_u64() % n as u64) as usize
        }
    }
}

const PIECES: &[&str] = &[
    "a",
    "Z",
    " ",
    "\n",
    "\r\n",
    "\r",
    "é",
    "e\u{301}",
    "سلام",
    "مرحبا ",
    "😀",
    "👨\u{200d}👩\u{200d}👧",
    "fn ",
    "{}",
    "日本",
    "\t",
];

fn random_text(rng: &mut Rng, max_pieces: usize) -> String {
    let count = rng.below(max_pieces + 1);
    (0..count)
        .map(|_| PIECES[rng.below(PIECES.len())])
        .collect()
}

fn char_boundaries(text: &str) -> Vec<usize> {
    text.char_indices()
        .map(|(i, _)| i)
        .chain(std::iter::once(text.len()))
        .collect()
}

/// Apply non-overlapping edits to a plain string (reference model).
fn reference_apply(text: &str, edits: &[TextEdit]) -> String {
    let mut sorted: Vec<&TextEdit> = edits.iter().collect();
    sorted.sort_by_key(|e| (e.start, e.end));
    let mut out = String::new();
    let mut cursor = 0;
    for e in sorted {
        out.push_str(&text[cursor..e.start.0]);
        out.push_str(&e.text);
        cursor = e.end.0;
    }
    out.push_str(&text[cursor..]);
    out
}

fn random_transaction(rng: &mut Rng, text: &str) -> Vec<TextEdit> {
    let bounds = char_boundaries(text);
    let mut points: Vec<usize> = (0..rng.below(4) * 2 + 2)
        .map(|_| bounds[rng.below(bounds.len())])
        .collect();
    points.sort_unstable();
    points.dedup();
    let mut edits = Vec::new();
    for pair in points.chunks(2) {
        let (start, end) = if pair.len() == 2 {
            (pair[0], pair[1])
        } else {
            (pair[0], pair[0])
        };
        edits.push(TextEdit::replace(
            ByteOffset(start),
            ByteOffset(end),
            random_text(rng, 3),
        ));
    }
    // Shuffle so the implementation has to sort.
    for i in (1..edits.len()).rev() {
        let j = rng.below(i + 1);
        edits.swap(i, j);
    }
    edits
}

#[test]
fn random_edits_match_reference_and_undo_restores() {
    for seed in 1..=40u64 {
        let mut rng = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
        let initial = random_text(&mut rng, 40);
        let mut doc = TextDocument::new(&initial);
        let mut model = initial.clone();
        let mut history = vec![initial.clone()];

        for step in 0..60 {
            let edits = random_transaction(&mut rng, &model);
            let expected = reference_apply(&model, &edits);
            let rev = doc.revision();
            if let Err(e) = doc.apply(rev, edits) {
                panic!("seed {seed} step {step}: {e}");
            }
            model = expected;
            assert_eq!(doc.buffer().text(), model, "seed {seed} step {step}");
            history.push(model.clone());
        }

        // Undo everything, checking every intermediate state.
        for expected in history.iter().rev().skip(1) {
            let rev = doc.revision();
            doc.undo(rev).unwrap().expect("undo available");
            assert_eq!(&doc.buffer().text(), expected, "seed {seed} undo");
        }
        assert_eq!(doc.buffer().text(), initial);

        // Redo everything.
        for expected in history.iter().skip(1) {
            let rev = doc.revision();
            doc.redo(rev).unwrap().expect("redo available");
            assert_eq!(&doc.buffer().text(), expected, "seed {seed} redo");
        }
    }
}

/// Reference line split matching the LSP definition (LF, CRLF, lone CR).
fn reference_line_starts(text: &str) -> Vec<usize> {
    let bytes = text.as_bytes();
    let mut starts = vec![0];
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\n' => starts.push(i + 1),
            b'\r' => {
                if bytes.get(i + 1) == Some(&b'\n') {
                    i += 1;
                }
                starts.push(i + 1);
            }
            _ => {}
        }
        i += 1;
    }
    starts
}

#[test]
fn coordinate_conversions_match_reference() {
    for seed in 1..=30u64 {
        let mut rng = Rng(seed.wrapping_mul(0xD1B5_4A32_D192_ED03) | 1);
        let text = random_text(&mut rng, 80);
        let doc = TextDocument::new(&text);
        let buf = doc.buffer();
        let starts = reference_line_starts(&text);
        assert_eq!(buf.line_count(), starts.len(), "seed {seed}: line count");

        for offset in char_boundaries(&text) {
            let line = starts.iter().rposition(|&s| s <= offset).unwrap_or(0);
            let col_bytes = offset - starts[line];
            let col_utf16 = text[starts[line]..offset].encode_utf16().count();

            let point = buf.byte_to_point(ByteOffset(offset)).unwrap();
            assert_eq!(
                (point.line, point.column),
                (line, col_bytes),
                "seed {seed} offset {offset}"
            );
            let u16p = buf.byte_to_utf16_point(ByteOffset(offset)).unwrap();
            assert_eq!(u16p, Utf16Point::new(line, col_utf16));
            assert_eq!(
                buf.byte_to_utf16(ByteOffset(offset)).unwrap().0,
                text[..offset].encode_utf16().count()
            );
            assert_eq!(
                buf.byte_to_char(ByteOffset(offset)).unwrap().0,
                text[..offset].chars().count()
            );
        }
    }
}
