//! JSON-RPC 2.0 envelope and TITAN error codes.

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const JSONRPC_VERSION: &str = "2.0";

/// Stable numeric error codes. Standard JSON-RPC codes are negative 32xxx; TITAN-specific
/// codes live in -32001..=-32099. Codes are never reused for a different meaning.
pub mod codes {
    pub const PARSE_ERROR: i64 = -32700;
    pub const INVALID_REQUEST: i64 = -32600;
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const INVALID_PARAMS: i64 = -32602;
    pub const INTERNAL_ERROR: i64 = -32603;

    /// The request named a revision that is not the document's current revision.
    /// `data`: `{ "expectedRevision": n, "currentRevision": m }`.
    pub const STALE_REVISION: i64 = -32001;
    /// A position or range is invalid for the document (out of bounds, not on a character
    /// boundary, inside a surrogate pair, inverted, or edits overlap).
    pub const INVALID_POSITION: i64 = -32002;
    /// Unknown document id.
    pub const DOCUMENT_NOT_FOUND: i64 = -32003;
    /// The path resolves outside the workspace.
    pub const PATH_OUTSIDE_WORKSPACE: i64 = -32004;
    /// The file changed on disk since it was loaded or saved; save with `force` to override.
    pub const EXTERNAL_MODIFICATION: i64 = -32005;
    /// The file is binary, not UTF-8, or in an unsupported encoding.
    pub const UNSUPPORTED_CONTENT: i64 = -32006;
    /// The file is above the configured size limit.
    pub const FILE_TOO_LARGE: i64 = -32007;
    /// The method needs an open workspace.
    pub const NO_WORKSPACE: i64 = -32008;
    /// `session.hello` must be the first request.
    pub const NOT_INITIALIZED: i64 = -32009;
    /// Filesystem error (missing file, permission denied, ...).
    pub const IO_ERROR: i64 = -32010;
    /// Closing would discard unsaved changes; pass `discard: true`.
    pub const UNSAVED_CHANGES: i64 = -32011;
    /// The client's protocol version is not supported.
    pub const UNSUPPORTED_PROTOCOL_VERSION: i64 = -32012;
    /// The background task queue is full; retry later.
    pub const SERVER_BUSY: i64 = -32013;
    /// The request exceeded its deadline and was stopped.
    pub const REQUEST_TIMEOUT: i64 = -32014;
    /// The response would exceed the configured size limit.
    pub const RESPONSE_TOO_LARGE: i64 = -32015;
    /// Restoring would discard content that changed on disk; pass `acceptConflict: true`.
    pub const RECOVERY_CONFLICT: i64 = -32016;
    /// The journal cannot be restored (unknown id, unrecoverable, or recovery disabled).
    pub const RECOVERY_UNAVAILABLE: i64 = -32017;
    /// The disk changed again since the version the client resolved against; look at the
    /// new `document/diskChanged` and decide again.
    pub const DISK_CHANGED_AGAIN: i64 = -32018;
    /// There is no disk conflict to resolve for this document.
    pub const NO_CONFLICT: i64 = -32019;
    /// The project graph has not been built yet (it builds in the background after
    /// `workspace.open`); see `graph.summary`.
    pub const GRAPH_UNAVAILABLE: i64 = -32020;
    /// The operation needs the language server, which is not available (reason in message).
    pub const SEMANTIC_UNAVAILABLE: i64 = -32021;
    /// The request was cancelled with `$/cancelRequest` (same code as LSP).
    pub const REQUEST_CANCELLED: i64 = -32800;
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

impl RpcError {
    pub fn new(code: i64, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
            data: None,
        }
    }

    pub fn with_data(mut self, data: Value) -> Self {
        self.data = Some(data);
        self
    }
}

/// A request as sent by a client. `id` absent means a notification (no response).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Request {
    pub jsonrpc: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<Value>,
    pub method: String,
    #[serde(default, skip_serializing_if = "Value::is_null")]
    pub params: Value,
}

impl Request {
    pub fn new(id: u64, method: impl Into<String>, params: Value) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id: Some(Value::from(id)),
            method: method.into(),
            params,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Response {
    pub jsonrpc: String,
    pub id: Value,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<RpcError>,
}

impl Response {
    pub fn success(id: Value, result: Value) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id,
            result: Some(result),
            error: None,
        }
    }

    pub fn failure(id: Value, error: RpcError) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id,
            result: None,
            error: Some(error),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn success_keeps_null_result() {
        let resp = Response::success(json!(1), Value::Null);
        let text = serde_json::to_string(&resp).unwrap();
        assert_eq!(text, r#"{"jsonrpc":"2.0","id":1,"result":null}"#);
    }

    #[test]
    fn failure_shape() {
        let resp = Response::failure(
            Value::Null,
            RpcError::new(codes::PARSE_ERROR, "bad").with_data(json!({"x": 1})),
        );
        let value = serde_json::to_value(&resp).unwrap();
        assert_eq!(
            value,
            json!({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"bad","data":{"x":1}}})
        );
    }
}
