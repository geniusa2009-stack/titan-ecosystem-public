//! TITAN Code headless protocol (version [`PROTOCOL_VERSION`]).
//!
//! Transport-independent: [`framing`] reads and writes `Content-Length`-framed messages on
//! any byte stream (stdio today; Unix sockets later). [`rpc`] holds the JSON-RPC 2.0
//! envelope and error codes, [`messages`] the typed params and results of every method.
//! The full contract is documented in `docs/protocol.md`.

pub mod framing;
pub mod messages;
pub mod rpc;

/// Incremented on any incompatible change to methods, params or results.
pub const PROTOCOL_VERSION: u32 = 1;
