//! Bounded capture of a check's combined stdout and stderr.
//!
//! The first `head_bytes` and the last `tail_bytes` are kept; everything in between is
//! counted but dropped. Chunks from stdout and stderr are appended in the order the reader
//! threads deliver them, so the interleaving is approximate (pipe-buffer granularity).

use std::collections::VecDeque;

use serde::{Deserialize, Serialize};

/// How much output to keep per check.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct LogLimits {
    /// Bytes kept from the start of the output.
    pub head_bytes: usize,
    /// Bytes kept from the end of the output.
    pub tail_bytes: usize,
}

impl Default for LogLimits {
    fn default() -> Self {
        Self {
            head_bytes: 64 * 1024,
            tail_bytes: 64 * 1024,
        }
    }
}

/// The captured output of one process: head and tail, with the total size.
///
/// When `truncated` is false, `head` is the whole output and `tail` is empty. When it is
/// true, `omitted_bytes` bytes between `head` and `tail` were dropped. Invalid UTF-8 is
/// replaced with U+FFFD (a cut may also split a multi-byte character).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct BoundedLog {
    /// The start of the output.
    pub head: String,
    /// The end of the output (empty unless truncated).
    pub tail: String,
    /// Total bytes the process wrote to stdout and stderr.
    pub total_bytes: u64,
    /// Bytes dropped between `head` and `tail`.
    pub omitted_bytes: u64,
    /// True when output was dropped.
    pub truncated: bool,
}

impl BoundedLog {
    /// A log holding only a note written by the pipeline (e.g. why a check did not run).
    #[must_use]
    pub fn note(text: &str) -> Self {
        Self {
            head: text.to_string(),
            ..Self::default()
        }
    }

    /// True when `needle` occurs in the head or the tail.
    #[must_use]
    pub fn contains(&self, needle: &str) -> bool {
        self.head.contains(needle) || self.tail.contains(needle)
    }
}

/// Accumulates output into a [`BoundedLog`].
#[derive(Debug)]
pub(crate) struct LogCapture {
    limits: LogLimits,
    head: Vec<u8>,
    tail: VecDeque<u8>,
    total: u64,
}

impl LogCapture {
    pub(crate) fn new(limits: LogLimits) -> Self {
        Self {
            limits,
            head: Vec::with_capacity(limits.head_bytes.min(64 * 1024)),
            tail: VecDeque::with_capacity(limits.tail_bytes.min(64 * 1024)),
            total: 0,
        }
    }

    pub(crate) fn push(&mut self, mut data: &[u8]) {
        self.total = self.total.saturating_add(data.len() as u64);
        let room = self.limits.head_bytes.saturating_sub(self.head.len());
        if room > 0 {
            let take = room.min(data.len());
            self.head.extend_from_slice(&data[..take]);
            data = &data[take..];
        }
        let cap = self.limits.tail_bytes;
        if data.is_empty() || cap == 0 {
            return;
        }
        if data.len() >= cap {
            self.tail.clear();
            self.tail.extend(&data[data.len() - cap..]);
            return;
        }
        let overflow = (self.tail.len() + data.len()).saturating_sub(cap);
        self.tail.drain(..overflow);
        self.tail.extend(data);
    }

    pub(crate) fn finish(self) -> BoundedLog {
        let kept = (self.head.len() + self.tail.len()) as u64;
        let omitted = self.total.saturating_sub(kept);
        let (front, back) = self.tail.as_slices();
        let mut tail = Vec::with_capacity(self.tail.len());
        tail.extend_from_slice(front);
        tail.extend_from_slice(back);
        if omitted == 0 {
            // Nothing dropped: head followed by tail is the whole output.
            let mut all = self.head;
            all.extend_from_slice(&tail);
            return BoundedLog {
                head: String::from_utf8_lossy(&all).into_owned(),
                tail: String::new(),
                total_bytes: self.total,
                omitted_bytes: 0,
                truncated: false,
            };
        }
        BoundedLog {
            head: String::from_utf8_lossy(&self.head).into_owned(),
            tail: String::from_utf8_lossy(&tail).into_owned(),
            total_bytes: self.total,
            omitted_bytes: omitted,
            truncated: true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits(head: usize, tail: usize) -> LogLimits {
        LogLimits {
            head_bytes: head,
            tail_bytes: tail,
        }
    }

    #[test]
    fn short_output_is_kept_whole() {
        let mut cap = LogCapture::new(limits(8, 8));
        cap.push(b"hello ");
        cap.push(b"world");
        let log = cap.finish();
        assert_eq!(log.head, "hello world");
        assert_eq!(log.tail, "");
        assert!(!log.truncated);
        assert_eq!(log.total_bytes, 11);
        assert_eq!(log.omitted_bytes, 0);
    }

    #[test]
    fn long_output_keeps_head_and_tail_and_reports_truncation() {
        let mut cap = LogCapture::new(limits(10, 6));
        cap.push(b"HEAD-START");
        for _ in 0..1000 {
            cap.push(b"middle middle middle\n");
        }
        cap.push(b"xx");
        cap.push(b"-END\n");
        let log = cap.finish();
        assert_eq!(log.head, "HEAD-START");
        assert_eq!(log.tail, "x-END\n");
        assert!(log.truncated);
        assert_eq!(log.total_bytes, 10 + 21 * 1000 + 2 + 5);
        assert_eq!(log.omitted_bytes, log.total_bytes - 16);
        assert!(log.contains("HEAD-START") && log.contains("-END"));
        assert!(!log.contains("middle middle"));
    }

    #[test]
    fn one_huge_chunk_is_split_between_head_and_tail() {
        let mut cap = LogCapture::new(limits(4, 4));
        let mut data = b"abcd".to_vec();
        data.extend(std::iter::repeat_n(b'.', 100));
        data.extend(b"wxyz");
        cap.push(&data);
        let log = cap.finish();
        assert_eq!((log.head.as_str(), log.tail.as_str()), ("abcd", "wxyz"));
        assert_eq!(log.omitted_bytes, 100);
        assert!(log.truncated);
    }

    #[test]
    fn exactly_full_buffers_are_not_truncated() {
        let mut cap = LogCapture::new(limits(3, 3));
        cap.push(b"abc");
        cap.push(b"def");
        let log = cap.finish();
        assert_eq!(log.head, "abcdef");
        assert!(!log.truncated);
    }
}
