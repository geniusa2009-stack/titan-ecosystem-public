//! Content fingerprints.
//!
//! Two different hashes, for two different jobs:
//!
//! * [`ContentHash`] (BLAKE3, 256 bits) identifies **file content** wherever the answer
//!   drives a data-integrity decision: "is the file on disk still the version this buffer
//!   was loaded from?" (save conflicts, file-watcher reconciliation, recovery journals).
//!   A 64-bit non-cryptographic hash is not enough there: collisions are rare but cheap to
//!   construct deliberately, and a collision would silently overwrite someone's file.
//!   BLAKE3 is collision-resistant and, with SIMD, faster per byte than byte-at-a-time
//!   FNV-1a (see `benchmarks/`), so it costs nothing on these paths. It is never computed
//!   per keystroke: only on load, save, disk checks and file-system events.
//! * [`stable_hash`] (FNV-1a, 64 bits) is a fast, stable **name key**: journal file-name
//!   prefixes derived from a document path. A collision there only makes a lookup consider
//!   one more candidate, which is then verified against the journal's base record.
//!
//! Corruption detection inside journal records uses CRC-32 (`titan-recovery`); that detects
//! torn or bit-flipped writes, not tampering. Nothing here is used for authentication.

use std::fmt;
use std::fs::File;
use std::io::{self, Read};

/// BLAKE3 digest of file content.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct ContentHash(pub [u8; 32]);

impl ContentHash {
    /// Hash of a byte slice.
    pub fn of(bytes: &[u8]) -> Self {
        Self(*blake3::hash(bytes).as_bytes())
    }

    /// Lowercase hexadecimal, 64 characters.
    pub fn to_hex(&self) -> String {
        let mut out = String::with_capacity(64);
        for byte in self.0 {
            out.push(char::from(HEX[usize::from(byte >> 4)]));
            out.push(char::from(HEX[usize::from(byte & 0x0f)]));
        }
        out
    }

    /// Parse the output of [`ContentHash::to_hex`].
    pub fn from_hex(text: &str) -> Option<Self> {
        let bytes = text.as_bytes();
        if bytes.len() != 64 {
            return None;
        }
        let mut out = [0u8; 32];
        for (i, pair) in bytes.chunks_exact(2).enumerate() {
            out[i] = (nibble(pair[0])? << 4) | nibble(pair[1])?;
        }
        Some(Self(out))
    }
}

const HEX: &[u8; 16] = b"0123456789abcdef";

fn nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

impl fmt::Debug for ContentHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "blake3:{}", self.to_hex())
    }
}

impl fmt::Display for ContentHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_hex())
    }
}

/// Incremental hashing for content read in pieces.
#[derive(Default)]
pub struct ContentHasher(blake3::Hasher);

impl ContentHasher {
    pub fn new() -> Self {
        Self(blake3::Hasher::new())
    }

    pub fn update(&mut self, bytes: &[u8]) {
        self.0.update(bytes);
    }

    pub fn finalize(&self) -> ContentHash {
        ContentHash(*self.0.finalize().as_bytes())
    }
}

/// Hash everything readable from `file`, streaming (constant memory). Returns the hash and
/// the number of bytes read.
pub fn hash_reader(file: &mut File) -> io::Result<(ContentHash, u64)> {
    let mut hasher = ContentHasher::new();
    let mut buf = vec![0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let n = match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        hasher.update(&buf[..n]);
        total += n as u64;
    }
    Ok((hasher.finalize(), total))
}

/// Stable, non-cryptographic 64-bit FNV-1a. Stable across processes and builds. Use it only
/// as a lookup key (see the module docs), never to decide that two contents are equal.
pub fn stable_hash(bytes: &[u8]) -> u64 {
    const OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
    const PRIME: u64 = 0x0000_0100_0000_01b3;
    bytes.iter().fold(OFFSET, |hash, &byte| {
        (hash ^ u64::from(byte)).wrapping_mul(PRIME)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blake3_reference_vectors() {
        // Official BLAKE3 test vector for the empty input, and the published hash of "abc".
        assert_eq!(
            ContentHash::of(b"").to_hex(),
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
        );
        assert_eq!(
            ContentHash::of(b"abc").to_hex(),
            "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"
        );
    }

    #[test]
    fn hex_round_trip_and_rejects_garbage() {
        let h = ContentHash::of("سلام 👋".as_bytes());
        assert_eq!(ContentHash::from_hex(&h.to_hex()), Some(h));
        assert_eq!(ContentHash::from_hex(&h.to_hex().to_uppercase()), Some(h));
        assert!(ContentHash::from_hex("abc").is_none());
        assert!(ContentHash::from_hex(&"g".repeat(64)).is_none());
    }

    #[test]
    fn incremental_equals_one_shot() {
        let data: Vec<u8> = (0..300_000u32).map(|i| (i % 251) as u8).collect();
        let mut hasher = ContentHasher::new();
        for chunk in data.chunks(7_919) {
            hasher.update(chunk);
        }
        assert_eq!(hasher.finalize(), ContentHash::of(&data));
    }

    #[test]
    fn stable_hash_is_fnv1a() {
        assert_eq!(stable_hash(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(stable_hash(b"a"), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(stable_hash(b"foobar"), 0x8594_4171_f739_67e8);
    }
}
