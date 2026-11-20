//! Streaming parser for libtest's human-readable output.
//!
//! It sees the *whole* stdout of `cargo test` (not the bounded log), line by line, and sums
//! every `test result: ...` line (one per test binary, including doc tests). Failing tests
//! are taken from `test <name> ... FAILED` lines.

use serde::{Deserialize, Serialize};

/// Most failing test names recorded per check; further names are counted only.
pub const MAX_FAILING_NAMES: usize = 1000;
/// Longest line the parser looks at; longer lines are skipped.
const MAX_LINE: usize = 16 * 1024;

/// Test counts summed over every test binary of one `cargo test` run.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TestSummary {
    /// Number of `test result:` lines seen (test binaries, including doc tests).
    pub binaries: u32,
    /// Tests that passed.
    pub passed: u64,
    /// Tests that failed.
    pub failed: u64,
    /// Tests that were ignored.
    pub ignored: u64,
    /// Benchmarks reported as measured.
    pub measured: u64,
    /// Tests excluded by the filter.
    pub filtered: u64,
    /// Names from `test <name> ... FAILED` lines, in output order (at most
    /// [`MAX_FAILING_NAMES`]).
    pub failing_names: Vec<String>,
    /// True when more failing names were seen than recorded.
    pub failing_names_truncated: bool,
}

/// Incremental line parser; feed it stdout chunks, then call [`Self::finish`].
#[derive(Debug, Default)]
pub(crate) struct LibtestParser {
    line: Vec<u8>,
    overlong: bool,
    summary: TestSummary,
}

impl LibtestParser {
    pub(crate) fn feed(&mut self, data: &[u8]) {
        for &b in data {
            if b == b'\n' {
                if !self.overlong {
                    let line = String::from_utf8_lossy(&self.line).into_owned();
                    self.parse_line(&line);
                }
                self.line.clear();
                self.overlong = false;
            } else if self.line.len() < MAX_LINE {
                self.line.push(b);
            } else {
                self.overlong = true;
            }
        }
    }

    /// The summary, or `None` when no `test result:` line was seen (nothing ran, e.g. the
    /// build failed).
    pub(crate) fn finish(mut self) -> Option<TestSummary> {
        if !self.line.is_empty() && !self.overlong {
            let line = String::from_utf8_lossy(&self.line).into_owned();
            self.parse_line(&line);
        }
        (self.summary.binaries > 0).then_some(self.summary)
    }

    fn parse_line(&mut self, raw: &str) {
        let line = raw.trim_end_matches('\r').trim();
        if let Some(rest) = line.strip_prefix("test result:") {
            self.parse_result(rest);
        } else if let Some(name) = line
            .strip_prefix("test ")
            .and_then(|l| l.strip_suffix(" ... FAILED"))
        {
            if self.summary.failing_names.len() < MAX_FAILING_NAMES {
                self.summary.failing_names.push(name.to_string());
            } else {
                self.summary.failing_names_truncated = true;
            }
        }
    }

    /// ` ok. 3 passed; 0 failed; 1 ignored; 0 measured; 2 filtered out; finished in 0.00s`
    fn parse_result(&mut self, rest: &str) {
        let Some((verdict, counts)) = rest.split_once(". ") else {
            return;
        };
        if !matches!(verdict.trim(), "ok" | "FAILED") {
            return;
        }
        self.summary.binaries += 1;
        for part in counts.split(';') {
            let mut words = part.split_whitespace();
            let (Some(number), Some(word)) = (words.next(), words.next()) else {
                continue;
            };
            let Ok(n) = number.parse::<u64>() else {
                continue;
            };
            let slot = match word {
                "passed" => &mut self.summary.passed,
                "failed" => &mut self.summary.failed,
                "ignored" => &mut self.summary.ignored,
                "measured" => &mut self.summary.measured,
                "filtered" => &mut self.summary.filtered,
                _ => continue,
            };
            *slot = slot.saturating_add(n);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OUTPUT: &str = "\
running 3 tests
test tests::other ... ok
test tests::slow ... ignored
test tests::adds ... FAILED

failures:

---- tests::adds stdout ----
thread 'tests::adds' panicked at src/lib.rs:9:9:
test result: this line is inside a panic message and is not a summary
assertion `left == right` failed

failures:
    tests::adds

test result: FAILED. 1 passed; 1 failed; 1 ignored; 0 measured; 0 filtered out; finished in 0.00s

running 1 test
test it_works ... ok

test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 2 filtered out; finished in 0.01s

   Doc-tests demo

running 0 tests

test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s
";

    #[test]
    fn sums_every_binary_and_collects_failing_names() {
        let mut parser = LibtestParser::default();
        // Feed in awkward chunks to exercise line reassembly.
        for chunk in OUTPUT.as_bytes().chunks(7) {
            parser.feed(chunk);
        }
        let summary = parser.finish();
        assert_eq!(
            summary,
            Some(TestSummary {
                binaries: 3,
                passed: 2,
                failed: 1,
                ignored: 1,
                measured: 0,
                filtered: 2,
                failing_names: vec!["tests::adds".to_string()],
                failing_names_truncated: false,
            })
        );
    }

    #[test]
    fn no_result_line_means_no_summary() {
        let mut parser = LibtestParser::default();
        parser.feed(b"error[E0308]: mismatched types\nerror: could not compile `demo`\n");
        assert_eq!(parser.finish(), None);
    }

    #[test]
    fn last_line_without_newline_and_crlf_are_parsed() {
        let mut parser = LibtestParser::default();
        parser.feed(b"test a::b ... FAILED\r\ntest result: FAILED. 0 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s");
        let summary = parser.finish().unwrap_or_default();
        assert_eq!(summary.failed, 1);
        assert_eq!(summary.failing_names, ["a::b"]);
    }

    #[test]
    fn failing_names_are_bounded() {
        let mut parser = LibtestParser::default();
        for i in 0..(MAX_FAILING_NAMES + 5) {
            parser.feed(format!("test t{i} ... FAILED\n").as_bytes());
        }
        parser.feed(
            b"test result: FAILED. 0 passed; 1005 failed; 0 ignored; 0 measured; 0 filtered out\n",
        );
        let summary = parser.finish().unwrap_or_default();
        assert_eq!(summary.failing_names.len(), MAX_FAILING_NAMES);
        assert!(summary.failing_names_truncated);
        assert_eq!(summary.failed, 1005);
    }
}
