//! JSON-RPC message construction and classification, and the client side of `initialize`.

use std::path::Path;

use serde_json::{Map, Value, json};

use crate::types::{LspError, codes};
use crate::uri::path_to_uri;

/// One incoming message, classified.
#[derive(Debug, PartialEq)]
pub(crate) enum Incoming {
    /// A response. `id` is `None` when it is not a non-negative integer (we only issue those).
    Response {
        id: Option<u64>,
        result: Result<Value, LspError>,
    },
    /// A server-to-client request.
    Request {
        id: Value,
        method: String,
        params: Value,
    },
    Notification {
        method: String,
        params: Value,
    },
}

/// Classifies a parsed message. `Err` describes a protocol violation.
pub(crate) fn classify(mut message: Value) -> Result<Incoming, String> {
    let Some(object) = message.as_object_mut() else {
        return Err("server sent a JSON value that is not an object".to_string());
    };
    let method = match object.remove("method") {
        None => None,
        Some(Value::String(method)) => Some(method),
        Some(_) => return Err("server sent a message whose method is not a string".to_string()),
    };
    let params = object.remove("params").unwrap_or(Value::Null);
    let id = object.remove("id");
    match (method, id) {
        (Some(method), Some(id)) => {
            if !(id.is_string() || id.is_i64() || id.is_u64()) {
                return Err(format!("server request {method} has an invalid id {id}"));
            }
            Ok(Incoming::Request { id, method, params })
        }
        (Some(method), None) => Ok(Incoming::Notification { method, params }),
        (None, Some(id)) => {
            let id = id.as_u64();
            if let Some(error) = object.remove("error") {
                return Ok(Incoming::Response {
                    id,
                    result: Err(server_error(&error)),
                });
            }
            match object.remove("result") {
                Some(result) => Ok(Incoming::Response {
                    id,
                    result: Ok(result),
                }),
                None => Err("server sent a response with neither result nor error".to_string()),
            }
        }
        (None, None) => Err("server sent a message with neither method nor id".to_string()),
    }
}

fn server_error(error: &Value) -> LspError {
    LspError::Server {
        code: error
            .get("code")
            .and_then(Value::as_i64)
            .unwrap_or(codes::INTERNAL_ERROR),
        message: error
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string(),
        data: error.get("data").cloned(),
    }
}

pub(crate) fn encode(value: &Value) -> Result<Vec<u8>, LspError> {
    serde_json::to_vec(value).map_err(|e| LspError::Encode(e.to_string()))
}

pub(crate) fn request(id: u64, method: &str, params: Value) -> Value {
    let mut object = Map::new();
    object.insert("jsonrpc".into(), "2.0".into());
    object.insert("id".into(), id.into());
    object.insert("method".into(), method.into());
    if !params.is_null() {
        object.insert("params".into(), params);
    }
    Value::Object(object)
}

pub(crate) fn notification(method: &str, params: Value) -> Value {
    let mut object = Map::new();
    object.insert("jsonrpc".into(), "2.0".into());
    object.insert("method".into(), method.into());
    if !params.is_null() {
        object.insert("params".into(), params);
    }
    Value::Object(object)
}

pub(crate) fn response_ok(id: Value, result: Value) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "result": result })
}

pub(crate) fn response_err(id: Value, code: i64, message: &str) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "error": { "code": code, "message": message } })
}

/// The answer to a server-to-client request.
pub(crate) fn answer_server_request(
    method: &str,
    params: &Value,
    workspace_configuration: &Value,
    workspace_folders: &Value,
) -> Result<Value, (i64, String)> {
    match method {
        "workspace/configuration" => {
            let items = params
                .get("items")
                .and_then(Value::as_array)
                .map(Vec::as_slice)
                .unwrap_or_default();
            Ok(Value::Array(
                items
                    .iter()
                    .map(|item| {
                        configuration_section(
                            workspace_configuration,
                            item.get("section").and_then(Value::as_str),
                        )
                    })
                    .collect(),
            ))
        }
        "workspace/workspaceFolders" => Ok(workspace_folders.clone()),
        "client/registerCapability"
        | "client/unregisterCapability"
        | "window/workDoneProgress/create"
        | "window/showMessageRequest"
        | "workspace/semanticTokens/refresh"
        | "workspace/inlayHint/refresh"
        | "workspace/inlineValue/refresh"
        | "workspace/codeLens/refresh"
        | "workspace/diagnostic/refresh" => Ok(Value::Null),
        _ => Err((
            codes::METHOD_NOT_FOUND,
            format!("titand does not handle {method}"),
        )),
    }
}

/// The configured value for `section`: a direct key first, then a dotted path; `null` if absent.
pub(crate) fn configuration_section(config: &Value, section: Option<&str>) -> Value {
    let Some(section) = section.filter(|s| !s.is_empty()) else {
        return config.clone();
    };
    if let Some(value) = config.get(section) {
        return value.clone();
    }
    let mut current = config;
    for part in section.split('.') {
        match current.get(part) {
            Some(next) => current = next,
            None => return Value::Null,
        }
    }
    current.clone()
}

pub(crate) fn workspace_folders(root: &Path) -> Value {
    let name = root
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| root.to_string_lossy().into_owned());
    json!([{ "uri": path_to_uri(root), "name": name }])
}

/// `InitializeParams` sent by titand.
pub(crate) fn initialize_params(root: &Path, initialization_options: &Value) -> Value {
    let mut params = json!({
        "processId": std::process::id(),
        "clientInfo": { "name": "titand", "version": env!("CARGO_PKG_VERSION") },
        "locale": "en",
        "rootUri": path_to_uri(root),
        "workspaceFolders": workspace_folders(root),
        "trace": "off",
        "capabilities": client_capabilities(),
    });
    let has_options = !initialization_options.is_null();
    if let Some(object) = params.as_object_mut().filter(|_| has_options) {
        object.insert(
            "initializationOptions".into(),
            initialization_options.clone(),
        );
    }
    params
}

/// The capabilities titand implements as an LSP client.
pub fn client_capabilities() -> Value {
    json!({
        "general": { "positionEncodings": ["utf-16"] },
        "textDocument": {
            "synchronization": {
                "dynamicRegistration": false,
                "willSave": false,
                "willSaveWaitUntil": false,
                "didSave": true
            },
            "publishDiagnostics": { "relatedInformation": true, "versionSupport": true },
            "definition": { "dynamicRegistration": false, "linkSupport": false },
            "references": { "dynamicRegistration": false },
            "hover": { "dynamicRegistration": false, "contentFormat": ["markdown", "plaintext"] },
            "documentSymbol": {
                "dynamicRegistration": false,
                "hierarchicalDocumentSymbolSupport": true
            },
            "completion": {
                "dynamicRegistration": false,
                "contextSupport": false,
                "completionItem": { "snippetSupport": false }
            }
        },
        "workspace": {
            "configuration": true,
            "workspaceFolders": true,
            "symbol": { "dynamicRegistration": false },
            "didChangeWatchedFiles": { "dynamicRegistration": false }
        },
        "window": { "workDoneProgress": true },
        "experimental": { "serverStatusNotification": true }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_messages() {
        assert_eq!(
            classify(json!({"jsonrpc":"2.0","id":7,"result":{"a":1}})),
            Ok(Incoming::Response {
                id: Some(7),
                result: Ok(json!({"a":1}))
            })
        );
        assert_eq!(
            classify(json!({"jsonrpc":"2.0","id":8,"result":null})),
            Ok(Incoming::Response {
                id: Some(8),
                result: Ok(Value::Null)
            })
        );
        assert_eq!(
            classify(json!({"id":"x","error":{"code":-32801,"message":"content modified"}})),
            Ok(Incoming::Response {
                id: None,
                result: Err(LspError::Server {
                    code: -32801,
                    message: "content modified".into(),
                    data: None
                })
            })
        );
        assert_eq!(
            classify(json!({"id":1,"method":"workspace/configuration","params":{"items":[]}})),
            Ok(Incoming::Request {
                id: json!(1),
                method: "workspace/configuration".into(),
                params: json!({"items":[]})
            })
        );
        assert_eq!(
            classify(json!({"method":"$/progress"})),
            Ok(Incoming::Notification {
                method: "$/progress".into(),
                params: Value::Null
            })
        );
        assert!(classify(json!([1, 2])).is_err());
        assert!(classify(json!({"id": 1})).is_err());
        assert!(classify(json!({"jsonrpc":"2.0"})).is_err());
        assert!(classify(json!({"method": 3})).is_err());
        assert!(classify(json!({"method": "m", "id": {"x": 1}})).is_err());
    }

    #[test]
    fn requests_omit_null_params() {
        let request = request(3, "shutdown", Value::Null);
        assert_eq!(request, json!({"jsonrpc":"2.0","id":3,"method":"shutdown"}));
        let notification = notification("exit", Value::Null);
        assert_eq!(notification, json!({"jsonrpc":"2.0","method":"exit"}));
    }

    #[test]
    fn workspace_configuration_lookup() {
        let config = json!({"rust-analyzer": {"checkOnSave": false}, "a": {"b": {"c": 1}}});
        let folders = json!([]);
        let answer = answer_server_request(
            "workspace/configuration",
            &json!({"items": [{"section": "rust-analyzer"}, {"section": "a.b"}, {"section": "missing"}, {}]}),
            &config,
            &folders,
        );
        assert_eq!(
            answer,
            Ok(json!([{"checkOnSave": false}, {"c": 1}, null, config.clone()]))
        );
        assert_eq!(
            answer_server_request("client/registerCapability", &json!({}), &config, &folders),
            Ok(Value::Null)
        );
        assert!(matches!(
            answer_server_request("custom/thing", &json!({}), &config, &folders),
            Err((codes::METHOD_NOT_FOUND, _))
        ));
    }

    #[test]
    fn initialize_params_carry_client_info_and_options() {
        let params = initialize_params(Path::new("/work/space"), &json!({"x": 1}));
        assert_eq!(params["clientInfo"]["name"], "titand");
        assert_eq!(params["rootUri"], "file:///work/space");
        assert_eq!(params["workspaceFolders"][0]["name"], "space");
        assert_eq!(params["initializationOptions"], json!({"x": 1}));
        assert_eq!(
            params["capabilities"]["general"]["positionEncodings"],
            json!(["utf-16"])
        );
        let without = initialize_params(Path::new("/w"), &Value::Null);
        assert!(without.get("initializationOptions").is_none());
    }
}
