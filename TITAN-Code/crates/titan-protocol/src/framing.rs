//! `Content-Length` framing (the LSP base protocol):
//!
//! ```text
//! Content-Length: <n>\r\n
//! \r\n
//! <n bytes of UTF-8 JSON>
//! ```
//!
//! Other headers are accepted and ignored. Header lines are capped at
//! [`MAX_HEADER_LINE`] bytes and bodies at a caller-supplied maximum, so a misbehaving peer
//! cannot make the reader allocate without bound. Any framing error leaves the stream in an
//! unknown position, so the connection must be closed after it.

use std::io::{self, BufRead, Read, Write};

/// Default maximum body size: 64 MiB.
pub const DEFAULT_MAX_MESSAGE_BYTES: usize = 64 * 1024 * 1024;
/// Maximum length of one header line, terminator included.
pub const MAX_HEADER_LINE: usize = 8 * 1024;
/// Maximum number of header lines in one message.
pub const MAX_HEADERS: usize = 32;

#[derive(Debug, thiserror::Error)]
pub enum FramingError {
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("malformed header: {0}")]
    MalformedHeader(String),
    #[error("message has no Content-Length header")]
    MissingContentLength,
    #[error("message body of {len} bytes exceeds the {max}-byte limit")]
    TooLarge { len: usize, max: usize },
    #[error("stream ended in the middle of a message")]
    UnexpectedEof,
}

/// Read one framed message. `Ok(None)` means the stream ended cleanly between messages.
pub fn read_message<R: BufRead>(
    reader: &mut R,
    max_body: usize,
) -> Result<Option<Vec<u8>>, FramingError> {
    let mut content_length: Option<usize> = None;
    let mut saw_header = false;
    let mut header_count = 0usize;
    let mut line = String::new();
    loop {
        line.clear();
        let read = reader
            .by_ref()
            .take(MAX_HEADER_LINE as u64)
            .read_line(&mut line)?;
        if read == 0 {
            return if saw_header {
                Err(FramingError::UnexpectedEof)
            } else {
                Ok(None)
            };
        }
        if !line.ends_with('\n') {
            return Err(if read >= MAX_HEADER_LINE {
                FramingError::MalformedHeader("header line too long".to_string())
            } else {
                FramingError::UnexpectedEof
            });
        }
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() {
            if saw_header {
                break;
            }
            // Tolerate blank lines between messages.
            continue;
        }
        saw_header = true;
        header_count += 1;
        if header_count > MAX_HEADERS {
            return Err(FramingError::MalformedHeader(format!(
                "more than {MAX_HEADERS} header lines"
            )));
        }
        let Some((name, value)) = trimmed.split_once(':') else {
            return Err(FramingError::MalformedHeader(trimmed.to_string()));
        };
        if name.trim().eq_ignore_ascii_case("content-length") {
            let len = value
                .trim()
                .parse::<usize>()
                .map_err(|_| FramingError::MalformedHeader(trimmed.to_string()))?;
            content_length = Some(len);
        }
    }
    let len = content_length.ok_or(FramingError::MissingContentLength)?;
    if len > max_body {
        return Err(FramingError::TooLarge { len, max: max_body });
    }
    let mut body = vec![0u8; len];
    reader.read_exact(&mut body).map_err(|e| {
        if e.kind() == io::ErrorKind::UnexpectedEof {
            FramingError::UnexpectedEof
        } else {
            FramingError::Io(e)
        }
    })?;
    Ok(Some(body))
}

/// Write one framed message and flush.
pub fn write_message<W: Write>(writer: &mut W, body: &[u8]) -> io::Result<()> {
    write!(writer, "Content-Length: {}\r\n\r\n", body.len())?;
    writer.write_all(body)?;
    writer.flush()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn framed(body: &str) -> Vec<u8> {
        let mut out = Vec::new();
        write_message(&mut out, body.as_bytes()).unwrap();
        out
    }

    #[test]
    fn round_trip_two_messages() {
        let mut bytes = framed(r#"{"a":1}"#);
        bytes.extend(framed(r#"{"b":"مرحبا"}"#));
        let mut cursor = Cursor::new(bytes);
        let first = read_message(&mut cursor, 1024).unwrap().unwrap();
        assert_eq!(first, br#"{"a":1}"#);
        let second = read_message(&mut cursor, 1024).unwrap().unwrap();
        assert_eq!(String::from_utf8(second).unwrap(), r#"{"b":"مرحبا"}"#);
        assert!(read_message(&mut cursor, 1024).unwrap().is_none());
    }

    #[test]
    fn extra_headers_and_case_are_accepted() {
        let raw = b"content-length: 2\r\nContent-Type: application/json\r\n\r\n{}";
        let body = read_message(&mut Cursor::new(raw.to_vec()), 16)
            .unwrap()
            .unwrap();
        assert_eq!(body, b"{}");
    }

    #[test]
    fn errors_are_reported() {
        let missing = b"X-Other: 1\r\n\r\n{}".to_vec();
        assert!(matches!(
            read_message(&mut Cursor::new(missing), 16),
            Err(FramingError::MissingContentLength)
        ));
        let bad_len = b"Content-Length: lots\r\n\r\n".to_vec();
        assert!(matches!(
            read_message(&mut Cursor::new(bad_len), 16),
            Err(FramingError::MalformedHeader(_))
        ));
        let too_big = b"Content-Length: 100\r\n\r\n".to_vec();
        assert!(matches!(
            read_message(&mut Cursor::new(too_big), 16),
            Err(FramingError::TooLarge { len: 100, max: 16 })
        ));
        let truncated = b"Content-Length: 10\r\n\r\n{}".to_vec();
        assert!(matches!(
            read_message(&mut Cursor::new(truncated), 16),
            Err(FramingError::UnexpectedEof)
        ));
        let no_colon = b"garbage\r\n\r\n".to_vec();
        assert!(matches!(
            read_message(&mut Cursor::new(no_colon), 16),
            Err(FramingError::MalformedHeader(_))
        ));
        let mut many = Vec::new();
        for i in 0..=MAX_HEADERS {
            many.extend(format!("X-Header-{i}: 1\r\n").into_bytes());
        }
        assert!(matches!(
            read_message(&mut Cursor::new(many), 16),
            Err(FramingError::MalformedHeader(_))
        ));
        let long_header = vec![b'x'; MAX_HEADER_LINE + 10];
        assert!(matches!(
            read_message(&mut Cursor::new(long_header), 16),
            Err(FramingError::MalformedHeader(_))
        ));
    }
}
