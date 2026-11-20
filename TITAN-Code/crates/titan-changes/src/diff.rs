//! A small line diff (Myers' O(ND) algorithm) and unified-diff output.
//!
//! Lines end after `\n`; a `\r` before it is part of the line, so CRLF/LF differences are
//! visible as changed lines (as with `diff -u`). A last line without a terminator is
//! followed by `\ No newline at end of file`. The edit distance is capped
//! ([`MAX_EDIT_LINES`]) and so is the input size ([`MAX_DIFF_LINES`]); beyond either,
//! [`unified_diff`] returns `None` and callers fall back to a "file replaced" summary.

/// Lines of context around each change.
pub const CONTEXT_LINES: usize = 3;
/// Maximum number of differing lines (inserted + deleted) the diff will search for.
pub const MAX_EDIT_LINES: usize = 1_000;
/// Maximum number of lines (old + new, after trimming the common prefix and suffix).
pub const MAX_DIFF_LINES: usize = 200_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Op {
    /// Line `a` of the old text equals line `b` of the new text.
    Equal(usize, usize),
    /// Old line deleted.
    Delete(usize),
    /// New line inserted.
    Insert(usize),
}

fn split_lines(text: &str) -> Vec<&str> {
    text.split_inclusive('\n').collect()
}

/// Shortest edit script from `a` to `b`, or `None` if it needs more than `max_d` edits.
fn myers(a: &[&str], b: &[&str], max_d: usize) -> Option<Vec<Op>> {
    let n = a.len();
    let m = b.len();
    let max = n + m;
    let limit = max.min(max_d);
    // v[k + offset] = furthest x on diagonal k.
    let offset = max + 1;
    let mut v = vec![0usize; 2 * max + 3];
    // trace[d] = the slice of v (diagonals -d-1..=d+1) as it was before step d.
    let mut trace: Vec<Vec<usize>> = Vec::new();
    let ni = n as isize;
    let mi = m as isize;
    let mut found = false;
    for d in 0..=limit {
        let di = d as isize;
        let lo = offset - d - 1;
        let hi = offset + d + 1;
        trace.push(v[lo..=hi].to_vec());
        let mut k = -di;
        while k <= di {
            let idx = (offset as isize + k) as usize;
            let mut x = if k == -di || (k != di && v[idx - 1] < v[idx + 1]) {
                v[idx + 1]
            } else {
                v[idx - 1] + 1
            };
            let mut y = (x as isize - k) as usize;
            while x < n && y < m && a[x] == b[y] {
                x += 1;
                y += 1;
            }
            v[idx] = x;
            if x as isize >= ni && y as isize >= mi {
                found = true;
                break;
            }
            k += 2;
        }
        if found {
            break;
        }
    }
    if !found {
        return None;
    }
    // Backtrack from (n, m).
    let mut ops = Vec::new();
    let mut x = ni;
    let mut y = mi;
    for d in (0..trace.len()).rev() {
        let di = d as isize;
        let snap = &trace[d];
        // snap index for diagonal k: k + d + 1.
        let at = |k: isize| snap[(k + di + 1) as usize] as isize;
        let k = x - y;
        let prev_k = if k == -di || (k != di && at(k - 1) < at(k + 1)) {
            k + 1
        } else {
            k - 1
        };
        let prev_x = at(prev_k);
        let prev_y = prev_x - prev_k;
        while x > prev_x && y > prev_y {
            ops.push(Op::Equal((x - 1) as usize, (y - 1) as usize));
            x -= 1;
            y -= 1;
        }
        if d > 0 {
            if x == prev_x {
                ops.push(Op::Insert((y - 1) as usize));
            } else {
                ops.push(Op::Delete((x - 1) as usize));
            }
        }
        x = prev_x;
        y = prev_y;
    }
    ops.reverse();
    Some(ops)
}

/// Edit script for the whole texts (common prefix and suffix trimmed first).
fn diff_ops(a: &[&str], b: &[&str]) -> Option<Vec<Op>> {
    let prefix = a.iter().zip(b).take_while(|(x, y)| x == y).count();
    let suffix = a[prefix..]
        .iter()
        .rev()
        .zip(b[prefix..].iter().rev())
        .take_while(|(x, y)| x == y)
        .count();
    let a_mid = &a[prefix..a.len() - suffix];
    let b_mid = &b[prefix..b.len() - suffix];
    if a_mid.len() + b_mid.len() > MAX_DIFF_LINES {
        return None;
    }
    let mid = myers(a_mid, b_mid, MAX_EDIT_LINES)?;
    let mut ops: Vec<Op> = (0..prefix).map(|i| Op::Equal(i, i)).collect();
    ops.extend(mid.into_iter().map(|op| match op {
        Op::Equal(x, y) => Op::Equal(x + prefix, y + prefix),
        Op::Delete(x) => Op::Delete(x + prefix),
        Op::Insert(y) => Op::Insert(y + prefix),
    }));
    let (a_tail, b_tail) = (a.len() - suffix, b.len() - suffix);
    ops.extend((0..suffix).map(|i| Op::Equal(a_tail + i, b_tail + i)));
    Some(ops)
}

fn push_line(out: &mut String, prefix: char, line: &str) {
    out.push(prefix);
    match line.strip_suffix('\n') {
        Some(body) => {
            out.push_str(body);
            out.push('\n');
        }
        None => {
            out.push_str(line);
            out.push_str("\n\\ No newline at end of file\n");
        }
    }
}

fn range(start: usize, len: usize) -> String {
    // GNU/BSD convention: an empty range names the line before it; a length of 1 is
    // omitted.
    let first = if len == 0 { start } else { start + 1 };
    if len == 1 {
        format!("{first}")
    } else {
        format!("{first},{len}")
    }
}

/// Standard unified diff of two texts with [`CONTEXT_LINES`] lines of context, headed by
/// `--- {old_label}` / `+++ {new_label}`. Empty when the texts are equal. `None` when the
/// diff exceeds the size caps.
pub fn unified_diff(old: &str, new: &str, old_label: &str, new_label: &str) -> Option<String> {
    if old == new {
        return Some(String::new());
    }
    let a = split_lines(old);
    let b = split_lines(new);
    let ops = diff_ops(&a, &b)?;
    let changes: Vec<usize> = ops
        .iter()
        .enumerate()
        .filter(|(_, op)| !matches!(op, Op::Equal(..)))
        .map(|(i, _)| i)
        .collect();
    let mut out = format!("--- {old_label}\n+++ {new_label}\n");
    let mut group_start = 0;
    while group_start < changes.len() {
        let mut group_end = group_start;
        while group_end + 1 < changes.len()
            && changes[group_end + 1] - changes[group_end] - 1 <= 2 * CONTEXT_LINES
        {
            group_end += 1;
        }
        let first = changes[group_start].saturating_sub(CONTEXT_LINES);
        let last = (changes[group_end] + CONTEXT_LINES).min(ops.len() - 1);
        // Old/new line numbers at the start of the hunk.
        let mut old_start = 0;
        let mut new_start = 0;
        for op in &ops[..first] {
            match op {
                Op::Equal(..) => {
                    old_start += 1;
                    new_start += 1;
                }
                Op::Delete(_) => old_start += 1,
                Op::Insert(_) => new_start += 1,
            }
        }
        let hunk = &ops[first..=last];
        let old_len = hunk
            .iter()
            .filter(|op| !matches!(op, Op::Insert(_)))
            .count();
        let new_len = hunk
            .iter()
            .filter(|op| !matches!(op, Op::Delete(_)))
            .count();
        out.push_str(&format!(
            "@@ -{} +{} @@\n",
            range(old_start, old_len),
            range(new_start, new_len)
        ));
        for op in hunk {
            match *op {
                Op::Equal(x, _) => push_line(&mut out, ' ', a[x]),
                Op::Delete(x) => push_line(&mut out, '-', a[x]),
                Op::Insert(y) => push_line(&mut out, '+', b[y]),
            }
        }
        group_start = group_end + 1;
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn diff(a: &str, b: &str) -> String {
        unified_diff(a, b, "a/f", "b/f").unwrap_or_else(|| "<too large>".to_string())
    }

    /// Apply an edit script to check that it really transforms `a` into `b`.
    fn replay(a: &[&str], b: &[&str], ops: &[Op]) -> Vec<String> {
        let mut out = Vec::new();
        let (mut ia, mut ib) = (0, 0);
        for op in ops {
            match *op {
                Op::Equal(x, y) => {
                    assert_eq!((x, y), (ia, ib));
                    assert_eq!(a[x], b[y]);
                    out.push(a[x].to_string());
                    ia += 1;
                    ib += 1;
                }
                Op::Delete(x) => {
                    assert_eq!(x, ia);
                    ia += 1;
                }
                Op::Insert(y) => {
                    assert_eq!(y, ib);
                    out.push(b[y].to_string());
                    ib += 1;
                }
            }
        }
        assert_eq!((ia, ib), (a.len(), b.len()));
        out
    }

    #[test]
    fn equal_texts_give_an_empty_diff() {
        assert_eq!(diff("a\nb\n", "a\nb\n"), "");
        assert_eq!(diff("", ""), "");
    }

    #[test]
    fn golden_single_line_change_with_context() {
        let old = "1\n2\n3\n4\n5\n6\n7\n8\n9\n";
        let new = "1\n2\n3\n4\nfive\n6\n7\n8\n9\n";
        assert_eq!(
            diff(old, new),
            "--- a/f\n+++ b/f\n@@ -2,7 +2,7 @@\n 2\n 3\n 4\n-5\n+five\n 6\n 7\n 8\n"
        );
    }

    #[test]
    fn golden_insertion_and_deletion() {
        let old = "a\nb\nc\n";
        assert_eq!(
            diff(old, "a\nb\nx\nc\n"),
            "--- a/f\n+++ b/f\n@@ -1,3 +1,4 @@\n a\n b\n+x\n c\n"
        );
        assert_eq!(
            diff(old, "a\nc\n"),
            "--- a/f\n+++ b/f\n@@ -1,3 +1,2 @@\n a\n-b\n c\n"
        );
        // Insertion into an empty file and deletion of everything.
        assert_eq!(
            diff("", "x\ny\n"),
            "--- a/f\n+++ b/f\n@@ -0,0 +1,2 @@\n+x\n+y\n"
        );
        assert_eq!(diff("x\n", ""), "--- a/f\n+++ b/f\n@@ -1 +0,0 @@\n-x\n");
    }

    #[test]
    fn golden_two_hunks_and_merged_hunks() {
        let lines = |changed: &[(usize, &str)]| -> String {
            (1..=20usize)
                .map(|i| match changed.iter().find(|(at, _)| *at == i) {
                    Some((_, text)) => format!("{text}\n"),
                    None => format!("{i}\n"),
                })
                .collect()
        };
        let old = lines(&[]);
        let far = lines(&[(2, "two"), (19, "nineteen")]);
        assert_eq!(
            diff(&old, &far),
            "--- a/f\n+++ b/f\n@@ -1,5 +1,5 @@\n 1\n-2\n+two\n 3\n 4\n 5\n\
             @@ -16,5 +16,5 @@\n 16\n 17\n 18\n-19\n+nineteen\n 20\n"
        );
        // Changes 6 lines apart share one hunk (gap <= 2 * context).
        let near = lines(&[(3, "three"), (10, "ten")]);
        assert_eq!(
            diff(&old, &near),
            "--- a/f\n+++ b/f\n@@ -1,13 +1,13 @@\n 1\n 2\n-3\n+three\n 4\n 5\n 6\n 7\n 8\n 9\n\
             -10\n+ten\n 11\n 12\n 13\n"
        );
    }

    #[test]
    fn golden_crlf_lines_keep_their_carriage_returns() {
        assert_eq!(
            diff("a\r\nb\r\nc\r\n", "a\r\nB\r\nc\r\n"),
            "--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n a\r\n-b\r\n+B\r\n c\r\n"
        );
        // Converting line endings changes every line.
        assert_eq!(
            diff("a\nb\n", "a\r\nb\r\n"),
            "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n-a\n-b\n+a\r\n+b\r\n"
        );
    }

    #[test]
    fn golden_missing_trailing_newline() {
        assert_eq!(
            diff("a\nb", "a\nb\n"),
            "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n"
        );
        assert_eq!(
            diff("a\nb\n", "a\nc"),
            "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n a\n-b\n+c\n\\ No newline at end of file\n"
        );
    }

    #[test]
    fn golden_unicode_lines() {
        assert_eq!(
            diff("مرحبا\nこんにちは\n👋🏽\n", "مرحبا\nこんばんは\n👋🏽\n"),
            "--- a/f\n+++ b/f\n@@ -1,3 +1,3 @@\n مرحبا\n-こんにちは\n+こんばんは\n 👋🏽\n"
        );
    }

    #[test]
    fn edit_scripts_are_minimal_and_replay_exactly() {
        // Deterministic pseudo-random line sequences over a small alphabet.
        let mut state = 0x2545_f491_4f6c_dd1du64;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        for _ in 0..300 {
            let len_a = (next() % 12) as usize;
            let len_b = (next() % 12) as usize;
            let a_owned: Vec<String> = (0..len_a).map(|_| format!("{}\n", next() % 4)).collect();
            let b_owned: Vec<String> = (0..len_b).map(|_| format!("{}\n", next() % 4)).collect();
            let a: Vec<&str> = a_owned.iter().map(String::as_str).collect();
            let b: Vec<&str> = b_owned.iter().map(String::as_str).collect();
            let ops = myers(&a, &b, usize::MAX).unwrap_or_default();
            assert_eq!(replay(&a, &b, &ops), b_owned);
            // Minimality: the number of edits equals n + m - 2 * LCS.
            let edits = ops.iter().filter(|op| !matches!(op, Op::Equal(..))).count();
            assert_eq!(edits, a.len() + b.len() - 2 * lcs(&a, &b));
            let full = diff_ops(&a, &b).unwrap_or_default();
            assert_eq!(replay(&a, &b, &full), b_owned);
        }
    }

    fn lcs(a: &[&str], b: &[&str]) -> usize {
        let mut table = vec![vec![0usize; b.len() + 1]; a.len() + 1];
        for i in (0..a.len()).rev() {
            for j in (0..b.len()).rev() {
                table[i][j] = if a[i] == b[j] {
                    table[i + 1][j + 1] + 1
                } else {
                    table[i + 1][j].max(table[i][j + 1])
                };
            }
        }
        table[0][0]
    }

    #[test]
    fn the_edit_distance_cap_falls_back() {
        let old: String = (0..3000).map(|i| format!("old {i}\n")).collect();
        let new: String = (0..3000).map(|i| format!("new {i}\n")).collect();
        assert!(unified_diff(&old, &new, "a", "b").is_none());
        // Large but similar files are fine.
        let similar = old.replace("old 1500\n", "changed\n");
        let out = unified_diff(&old, &similar, "a", "b").unwrap_or_default();
        assert!(out.contains("-old 1500\n+changed\n"), "{out}");
    }
}
