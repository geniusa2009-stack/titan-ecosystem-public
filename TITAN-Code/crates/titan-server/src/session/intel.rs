//! Semantic intelligence V1: version-aware queries that combine the tree-sitter index
//! (syntax evidence) and the language server (semantic evidence) without merging them.
//!
//! Every answer is a list of [`Evidence`] entries: one per source, each with its status
//! (`current`, `stale`, `unavailable`, `failed`, `timeout`, `cancelled`), the revision it
//! describes, the source's generation, whether the source considered itself complete, and
//! known limitations. Results computed for a revision that is no longer current are
//! returned as `stale`, never as current.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Instant;

use serde_json::{Value, json};
use titan_index::{EVIDENCE, SourceVersion};
use titan_lsp::{LspError, path_to_uri};
use titan_protocol::messages::{
    Evidence, IntelResult, Position, PositionEncoding, PositionParams, Range,
    WorkspaceSymbolsParams,
};
use titan_protocol::rpc::{RpcError, codes};
use titan_text::{ByteOffset, Point, TextBuffer, Utf16Point};

use super::lsp::{LIGHT_MODE_LIMITATION, LspPending, buffer_for, uri_to_path};
use super::{
    MAX_INDEXED_FILE_BYTES, Reply, ServerError, Session, check_revision, display_path,
    line_col_position, request_key, rpc_error, to_json, to_offset,
};

const DEFAULT_WORKSPACE_SYMBOLS: usize = 200;
const MAX_WORKSPACE_SYMBOLS: usize = 2_000;
const MAX_COMPLETIONS: usize = 200;
const MAX_LOCATIONS: usize = 1_000;

struct Ask {
    query: Query,
    params: Value,
    doc_id: Option<u64>,
    revision: Option<u64>,
    timeout_ms: Option<u64>,
    syntax: Vec<Evidence>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Query {
    DocumentSymbols,
    WorkspaceSymbols {
        max: usize,
    },
    Definition,
    References,
    Hover,
    Completion,
    /// One request of a multi-request job (impact analysis, rename).
    Part {
        job: u64,
        part: Part,
    },
}

/// The requests a job can consist of.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Part {
    Definition,
    References,
    PrepareCallHierarchy,
    IncomingCalls,
    PrepareRename,
    Rename,
}

impl Query {
    pub(super) fn method(self) -> &'static str {
        match self {
            Self::Part { part, .. } => match part {
                Part::Definition => "textDocument/definition",
                Part::References => "textDocument/references",
                Part::PrepareCallHierarchy => "textDocument/prepareCallHierarchy",
                Part::IncomingCalls => "callHierarchy/incomingCalls",
                Part::PrepareRename => "textDocument/prepareRename",
                Part::Rename => "textDocument/rename",
            },
            Self::DocumentSymbols => "textDocument/documentSymbol",
            Self::WorkspaceSymbols { .. } => "workspace/symbol",
            Self::Definition => "textDocument/definition",
            Self::References => "textDocument/references",
            Self::Hover => "textDocument/hover",
            Self::Completion => "textDocument/completion",
        }
    }

    /// The server capability that must be present for the request.
    pub(super) fn capability(self) -> &'static str {
        match self {
            Self::Part { part, .. } => match part {
                Part::Definition => "definitionProvider",
                Part::References => "referencesProvider",
                Part::PrepareCallHierarchy | Part::IncomingCalls => "callHierarchyProvider",
                Part::PrepareRename | Part::Rename => "renameProvider",
            },
            Self::DocumentSymbols => "documentSymbolProvider",
            Self::WorkspaceSymbols { .. } => "workspaceSymbolProvider",
            Self::Definition => "definitionProvider",
            Self::References => "referencesProvider",
            Self::Hover => "hoverProvider",
            Self::Completion => "completionProvider",
        }
    }
}

const SYMBOL_KINDS: &[&str] = &[
    "file",
    "module",
    "namespace",
    "package",
    "class",
    "method",
    "property",
    "field",
    "constructor",
    "enum",
    "interface",
    "function",
    "variable",
    "constant",
    "string",
    "number",
    "boolean",
    "array",
    "object",
    "key",
    "null",
    "enumMember",
    "struct",
    "event",
    "operator",
    "typeParameter",
];

const COMPLETION_KINDS: &[&str] = &[
    "text",
    "method",
    "function",
    "constructor",
    "field",
    "variable",
    "class",
    "interface",
    "module",
    "property",
    "unit",
    "value",
    "enum",
    "keyword",
    "snippet",
    "color",
    "file",
    "reference",
    "folder",
    "enumMember",
    "constant",
    "struct",
    "event",
    "operator",
    "typeParameter",
];

fn kind_name(table: &[&str], kind: &Value) -> String {
    kind.as_u64()
        .and_then(|k| usize::try_from(k).ok())
        .and_then(|k| k.checked_sub(1))
        .and_then(|k| table.get(k))
        .map_or_else(|| "unknown".to_string(), |s| (*s).to_string())
}

fn elapsed_ms(start: Instant) -> u64 {
    u64::try_from(start.elapsed().as_millis()).unwrap_or(u64::MAX)
}

/// Convert an LSP (UTF-16) range in `path` to the client's encoding.
pub(super) fn lsp_range_to_client(
    session: &Session,
    path: &Path,
    range: &Value,
    cache: &mut HashMap<PathBuf, Option<TextBuffer>>,
) -> Option<Range> {
    let point = |v: &Value| -> Option<(usize, usize)> {
        Some((
            usize::try_from(v["line"].as_u64()?).ok()?,
            usize::try_from(v["character"].as_u64()?).ok()?,
        ))
    };
    let start = point(&range["start"])?;
    let end = point(&range["end"])?;
    if session.encoding == PositionEncoding::Utf16 {
        return Some(Range {
            start: Position {
                line: start.0,
                character: start.1,
            },
            end: Position {
                line: end.0,
                character: end.1,
            },
        });
    }
    let convert = |buffer: &TextBuffer, (line, character): (usize, usize)| -> Option<Position> {
        let byte = buffer
            .utf16_point_to_byte(Utf16Point::new(line, character))
            .ok()?;
        let p = buffer.byte_to_point(byte).ok()?;
        Some(Position {
            line: p.line,
            character: p.column,
        })
    };
    if let Some(&doc_id) = session.by_path.get(path) {
        let buffer = session.documents.get(&doc_id)?.doc.text().buffer();
        return Some(Range {
            start: convert(buffer, start)?,
            end: convert(buffer, end)?,
        });
    }
    let buffer = cache
        .entry(path.to_path_buf())
        .or_insert_with(|| buffer_for(path, MAX_INDEXED_FILE_BYTES))
        .as_ref()?;
    Some(Range {
        start: convert(buffer, start)?,
        end: convert(buffer, end)?,
    })
}

impl Session {
    pub(super) fn semantic_name(&self) -> String {
        self.semantic
            .name
            .clone()
            .unwrap_or_else(|| "languageServer".to_string())
    }

    pub(super) fn semantic_limitations(&self) -> Vec<String> {
        let mut out = Vec::new();
        if self.options.lsp.as_ref().is_some_and(|o| o.light_mode) {
            out.push(LIGHT_MODE_LIMITATION.to_string());
        }
        if self.semantic.quiescent() != Some(true) {
            out.push(
                "the language server had not finished loading the workspace; results may be incomplete"
                    .to_string(),
            );
        }
        out
    }

    pub(super) fn unavailable(&self, reason: String, revision: Option<u64>) -> Evidence {
        Evidence {
            source: self.semantic_name(),
            kind: "semantic".to_string(),
            status: "unavailable".to_string(),
            document_revision: revision,
            generation: Some(self.semantic.generation),
            complete: None,
            limitations: Vec::new(),
            error: Some(reason),
            elapsed_ms: 0,
            items: Value::Array(Vec::new()),
        }
    }

    /// Why semantic evidence cannot be produced for `doc_id`, if it cannot.
    pub(super) fn semantic_blocker(&self, doc_id: Option<u64>, query: Query) -> Option<String> {
        if self.semantic.supervisor.is_none() {
            return Some(match &self.semantic.name {
                None => "no language server is configured (start titand with --rust-analyzer)"
                    .to_string(),
                Some(_) => "the language server is not running".to_string(),
            });
        }
        if !self.semantic.running() {
            let state = self.semantic.state_name();
            return Some(match self.semantic.state_reason() {
                Some(reason) => format!("the language server is {state}: {reason}"),
                None => format!("the language server is {state}"),
            });
        }
        if let Some(doc_id) = doc_id {
            let od = self.documents.get(&doc_id)?;
            if !od.is_rust {
                return Some("no language server handles this file type".to_string());
            }
            let synced = self
                .semantic
                .synced
                .get(&doc_id)
                .is_some_and(|s| s.generation == self.semantic.generation);
            if !synced {
                return Some("the document is not yet open in the language server".to_string());
            }
        }
        let supported = self
            .semantic
            .supervisor
            .as_ref()
            .and_then(titan_lsp::Supervisor::server_capabilities)
            .is_some_and(|caps| {
                let cap = &caps[query.capability()];
                !cap.is_null() && cap != &Value::Bool(false)
            });
        if !supported {
            return Some(format!(
                "the language server does not support {}",
                query.method()
            ));
        }
        None
    }

    fn intel_reply(
        &self,
        doc_id: Option<u64>,
        results: Vec<Evidence>,
    ) -> Result<Value, ServerError> {
        to_json(IntelResult {
            document_id: doc_id,
            current_revision: doc_id
                .and_then(|id| self.documents.get(&id))
                .map(|od| od.doc.text().revision()),
            results,
        })
    }

    /// Send `query` to the language server and answer later, or answer now with the
    /// semantic part marked unavailable.
    fn ask_language_server(&mut self, ask: Ask, id: Option<Value>) -> Reply {
        let Ask {
            query,
            params,
            doc_id,
            revision,
            timeout_ms,
            syntax,
        } = ask;
        let mut results = Vec::new();
        if let Some(reason) = self.semantic_blocker(doc_id, query) {
            results.push(self.unavailable(reason, revision));
            results.extend(syntax);
            return Reply::Now(self.intel_reply(doc_id, results));
        }
        let timeout = timeout_ms.map(|ms| std::time::Duration::from_millis(ms.max(1)));
        let Some(supervisor) = &self.semantic.supervisor else {
            return Reply::Now(Err(rpc_error(codes::INTERNAL_ERROR, "no supervisor")));
        };
        match supervisor.request(query.method(), params, timeout) {
            Ok(ticket) => {
                let key = id.as_ref().map(request_key);
                if let Some(key) = &key {
                    self.semantic.by_key.insert(key.clone(), ticket);
                }
                self.semantic.pending.insert(
                    ticket,
                    LspPending {
                        id,
                        key,
                        query,
                        doc_id,
                        revision,
                        generation: self.semantic.generation,
                        started: Instant::now(),
                        syntax,
                    },
                );
                Reply::Deferred
            }
            Err(e) => {
                let mut failed = self.unavailable(e.to_string(), revision);
                failed.status = "failed".to_string();
                results.push(failed);
                results.extend(syntax);
                Reply::Now(self.intel_reply(doc_id, results))
            }
        }
    }

    /// Cancel a deferred intel request by its client id. Returns true if it was one.
    pub(super) fn cancel_intel_request(&mut self, key: &str) -> bool {
        let Some(ticket) = self.semantic.by_key.get(key).copied() else {
            return false;
        };
        if let Some(supervisor) = &self.semantic.supervisor {
            // The supervisor completes the ticket with `Cancelled`.
            supervisor.cancel(ticket);
        }
        true
    }

    pub(super) fn on_lsp_response(
        &mut self,
        ticket: u64,
        generation: u64,
        result: Result<Value, LspError>,
    ) {
        let Some(pending) = self.semantic.pending.remove(&ticket) else {
            return;
        };
        if let Some(key) = &pending.key {
            self.semantic.by_key.remove(key);
        }
        if let Query::Part { job, part } = pending.query {
            return self.on_job_part(job, part, generation, result);
        }
        let Some(id) = pending.id.clone() else {
            return;
        };
        if matches!(result, Err(LspError::Cancelled)) {
            self.counters.requests_cancelled += 1;
            self.push_response(titan_protocol::rpc::Response::failure(
                id,
                RpcError::new(codes::REQUEST_CANCELLED, "request cancelled"),
            ));
            return;
        }
        if generation == self.semantic.generation && result.is_ok() {
            let at = self.semantic.cold.since().unwrap_or(0);
            let latency = elapsed_ms(pending.started);
            self.semantic
                .cold
                .first_answers
                .entry(pending.query.method())
                .or_insert((at, latency));
        }
        let current_revision = pending
            .doc_id
            .and_then(|d| self.documents.get(&d))
            .map(|od| od.doc.text().revision());
        let moved_on = pending.revision.is_some() && pending.revision != current_revision;
        let restarted = generation != self.semantic.generation;
        let mut evidence = Evidence {
            source: self.semantic_name(),
            kind: "semantic".to_string(),
            status: String::new(),
            document_revision: pending.revision,
            generation: Some(pending.generation),
            complete: self.semantic.quiescent(),
            limitations: self.semantic_limitations(),
            error: None,
            elapsed_ms: elapsed_ms(pending.started),
            items: Value::Array(Vec::new()),
        };
        match result {
            Ok(value) => {
                evidence.status = if moved_on || restarted {
                    "stale"
                } else {
                    "current"
                }
                .to_string();
                if moved_on {
                    evidence.limitations.push(
                        "the document changed while the server answered: positions refer to documentRevision"
                            .to_string(),
                    );
                }
                let doc_path = pending
                    .doc_id
                    .and_then(|d| self.documents.get(&d))
                    .map(|od| od.doc.path().to_path_buf());
                evidence.items = self.convert_semantic(
                    pending.query,
                    doc_path.as_deref(),
                    &value,
                    &mut evidence.limitations,
                );
            }
            Err(e) => {
                evidence.status = match &e {
                    LspError::Timeout => "timeout",
                    e if e.is_content_modified() => "stale",
                    _ => "failed",
                }
                .to_string();
                evidence.error = Some(e.to_string());
            }
        }
        let mut results = vec![evidence];
        results.extend(pending.syntax);
        let reply = self.intel_reply(pending.doc_id, results);
        self.respond(id, reply);
    }

    pub(super) fn location_json(
        &self,
        uri: &str,
        range: &Value,
        cache: &mut HashMap<PathBuf, Option<TextBuffer>>,
    ) -> Option<Value> {
        let path = uri_to_path(uri)?;
        let range = lsp_range_to_client(self, &path, range, cache)?;
        let root = self.workspace.as_ref().map(|ws| ws.root().to_path_buf());
        let external = root.as_ref().is_none_or(|r| !path.starts_with(r));
        let revision = self
            .by_path
            .get(&path)
            .and_then(|id| self.semantic.synced.get(id))
            .map(|s| s.version);
        Some(json!({
            "path": if external { display_path(&path) } else { self.relative_display(&path) },
            "range": range,
            "revision": revision,
            "external": external,
        }))
    }

    pub(super) fn locations(&self, value: &Value) -> Vec<Value> {
        let mut cache = HashMap::new();
        let entries: Vec<&Value> = match value {
            Value::Array(items) => items.iter().collect(),
            Value::Null => Vec::new(),
            single => vec![single],
        };
        entries
            .into_iter()
            .take(MAX_LOCATIONS)
            .filter_map(|loc| {
                if let Some(uri) = loc["targetUri"].as_str() {
                    let range = if loc["targetSelectionRange"].is_object() {
                        &loc["targetSelectionRange"]
                    } else {
                        &loc["targetRange"]
                    };
                    self.location_json(uri, range, &mut cache)
                } else {
                    self.location_json(loc["uri"].as_str()?, &loc["range"], &mut cache)
                }
            })
            .collect()
    }

    fn convert_semantic(
        &self,
        query: Query,
        doc_path: Option<&Path>,
        value: &Value,
        limitations: &mut Vec<String>,
    ) -> Value {
        match query {
            Query::Definition | Query::References => Value::Array(self.locations(value)),
            Query::Part { .. } => value.clone(),
            Query::Hover => {
                if value.is_null() {
                    return Value::Array(Vec::new());
                }
                let (text, format) = hover_text(&value["contents"]);
                json!([{ "contents": text, "format": format }])
            }
            Query::Completion => {
                let (items, incomplete) = match value {
                    Value::Array(items) => (items.clone(), false),
                    Value::Object(_) => (
                        value["items"].as_array().cloned().unwrap_or_default(),
                        value["isIncomplete"].as_bool().unwrap_or(false),
                    ),
                    _ => (Vec::new(), false),
                };
                if incomplete {
                    limitations
                        .push("the server marked the completion list incomplete".to_string());
                }
                if items.len() > MAX_COMPLETIONS {
                    limitations.push(format!(
                        "{} completion items, first {MAX_COMPLETIONS} returned",
                        items.len()
                    ));
                }
                Value::Array(
                    items
                        .iter()
                        .take(MAX_COMPLETIONS)
                        .map(|item| {
                            json!({
                                "label": item["label"],
                                "kind": kind_name(COMPLETION_KINDS, &item["kind"]),
                                "detail": item["detail"],
                            })
                        })
                        .collect(),
                )
            }
            Query::DocumentSymbols => {
                let mut out = Vec::new();
                let mut cache = HashMap::new();
                if let Some(items) = value.as_array() {
                    for item in items {
                        self.flatten_symbol(item, doc_path, None, &mut out, &mut cache, 0);
                    }
                }
                Value::Array(out)
            }
            Query::WorkspaceSymbols { max } => {
                let mut cache = HashMap::new();
                let items = value.as_array().cloned().unwrap_or_default();
                if items.len() > max {
                    limitations.push(format!("{} symbols, first {max} returned", items.len()));
                }
                Value::Array(
                    items
                        .iter()
                        .take(max)
                        .map(|s| {
                            let location = s["location"]["uri"].as_str().and_then(|uri| {
                                if s["location"]["range"].is_object() {
                                    self.location_json(uri, &s["location"]["range"], &mut cache)
                                } else {
                                    uri_to_path(uri)
                                        .map(|p| json!({ "path": self.relative_display(&p) }))
                                }
                            });
                            json!({
                                "name": s["name"],
                                "kind": kind_name(SYMBOL_KINDS, &s["kind"]),
                                "containerName": s["containerName"],
                                "location": location,
                            })
                        })
                        .collect(),
                )
            }
        }
    }

    /// Flatten `DocumentSymbol` trees (or pass `SymbolInformation` through).
    fn flatten_symbol(
        &self,
        item: &Value,
        doc_path: Option<&Path>,
        container: Option<&str>,
        out: &mut Vec<Value>,
        cache: &mut HashMap<PathBuf, Option<TextBuffer>>,
        depth: usize,
    ) {
        if depth > 64 {
            return;
        }
        let name = item["name"].as_str().unwrap_or_default();
        let range = if item["selectionRange"].is_object() {
            item.get("selectionRange")
        } else {
            item.get("location").map(|l| &l["range"])
        };
        // DocumentSymbol positions are in the queried document; convert via its path.
        let path = item["location"]["uri"]
            .as_str()
            .and_then(uri_to_path)
            .or_else(|| doc_path.map(Path::to_path_buf));
        let range = match (range, path) {
            (Some(range), Some(path)) => lsp_range_to_client(self, &path, range, cache),
            _ => None,
        };
        out.push(json!({
            "name": name,
            "kind": kind_name(SYMBOL_KINDS, &item["kind"]),
            "detail": item["detail"],
            "containerName": container.or(item["containerName"].as_str()),
            "range": range,
        }));
        if let Some(children) = item["children"].as_array() {
            for child in children {
                self.flatten_symbol(child, doc_path, Some(name), out, cache, depth + 1);
            }
        }
    }

    // ----- syntax evidence -----------------------------------------------------------

    pub(super) fn syntax_evidence(
        &self,
        status: &str,
        revision: Option<u64>,
        items: Value,
    ) -> Evidence {
        Evidence {
            source: "tree-sitter".to_string(),
            kind: "syntax".to_string(),
            status: status.to_string(),
            document_revision: revision,
            generation: Some(self.index.generation()),
            complete: Some(self.index_idle()),
            limitations: vec![format!(
                "{EVIDENCE}: definitions and nesting only; names are not resolved"
            )],
            error: None,
            elapsed_ms: 0,
            items,
        }
    }

    /// Identifier around `offset` on its line.
    pub(super) fn identifier_at(buffer: &TextBuffer, offset: ByteOffset) -> Option<String> {
        let point = buffer.byte_to_point(offset).ok()?;
        let start = buffer.point_to_byte(Point::new(point.line, 0)).ok()?;
        let end = buffer
            .point_to_byte(Point::new(point.line + 1, 0))
            .unwrap_or(ByteOffset(buffer.len_bytes()));
        let line = buffer.slice(start..end).ok()?;
        let col = point.column.min(line.len());
        let is_ident = |c: char| c.is_alphanumeric() || c == '_';
        let before = line[..col]
            .chars()
            .rev()
            .take_while(|c| is_ident(*c))
            .count();
        let begin = match before.checked_sub(1) {
            None => col,
            Some(n) => line[..col]
                .char_indices()
                .rev()
                .nth(n)
                .map_or(col, |(i, _)| i),
        };
        let after: usize = line[col..]
            .chars()
            .take_while(|c| is_ident(*c))
            .map(char::len_utf8)
            .sum();
        let word = &line[begin..col + after];
        (!word.is_empty() && !word.starts_with(|c: char| c.is_ascii_digit()))
            .then(|| word.to_string())
    }

    pub(super) fn name_match_evidence(&self, doc_id: u64, offset: ByteOffset) -> Evidence {
        let Some(od) = self.documents.get(&doc_id) else {
            return self.syntax_evidence("failed", None, Value::Array(Vec::new()));
        };
        let revision = od.doc.text().revision();
        let word = Self::identifier_at(od.doc.text().buffer(), offset);
        let items: Vec<Value> = match &word {
            None => Vec::new(),
            Some(word) => self
                .index
                .search(word, 200)
                .into_iter()
                .filter(|m| &m.symbol.name == word)
                .map(|m| {
                    let revision = match m.version {
                        SourceVersion::Buffer { revision } => Some(revision),
                        SourceVersion::Disk { .. } => None,
                    };
                    json!({
                        "path": self.relative_display(m.path),
                        "range": Range {
                            start: line_col_position(self.encoding, m.symbol.name_start),
                            end: line_col_position(self.encoding, m.symbol.name_end),
                        },
                        "kind": m.symbol.kind.as_str(),
                        "revision": revision,
                    })
                })
                .collect(),
        };
        let mut evidence = self.syntax_evidence("current", Some(revision), Value::Array(items));
        evidence.kind = "nameMatch".to_string();
        evidence.limitations = vec![
            "candidates are definitions with the same name as the identifier; the name is not resolved (shadowing, imports, methods of other types)".to_string(),
        ];
        if word.is_none() {
            evidence.error = Some("no identifier at this position".to_string());
        }
        evidence
    }

    // ----- requests ------------------------------------------------------------------

    pub(super) fn prepare_position(
        &self,
        query: Query,
        params: &PositionParams,
    ) -> Result<(Value, ByteOffset), ServerError> {
        let od = self.open_doc(params.document_id)?;
        check_revision(&od.doc, params.expected_revision)?;
        let buffer = od.doc.text().buffer();
        let offset = to_offset(self.encoding, buffer, params.position)?;
        let utf16 = buffer.byte_to_utf16_point(offset)?;
        let mut lsp = json!({
            "textDocument": { "uri": path_to_uri(od.doc.path()) },
            "position": { "line": utf16.line, "character": utf16.column },
        });
        if query == Query::References {
            lsp["context"] = json!({ "includeDeclaration": params.include_declaration });
        }
        Ok((lsp, offset))
    }

    pub(super) fn intel_position(
        &mut self,
        query: Query,
        params: PositionParams,
        id: Option<Value>,
    ) -> Reply {
        let prepared = self.prepare_position(query, &params);
        let (lsp, offset) = match prepared {
            Ok(p) => p,
            Err(e) => return Reply::Now(Err(e)),
        };
        let syntax = match query {
            Query::Definition => vec![self.name_match_evidence(params.document_id, offset)],
            Query::References => vec![{
                let mut e = self.syntax_evidence(
                    "unavailable",
                    Some(params.expected_revision),
                    Value::Array(Vec::new()),
                );
                e.error = Some("the syntax index does not record references".to_string());
                e
            }],
            _ => Vec::new(),
        };
        self.ask_language_server(
            Ask {
                query,
                params: lsp,
                doc_id: Some(params.document_id),
                revision: Some(params.expected_revision),
                timeout_ms: params.timeout_ms,
                syntax,
            },
            id,
        )
    }

    pub(super) fn intel_document_symbols(&mut self, doc_id: u64, id: Option<Value>) -> Reply {
        let (path, revision) = match self.open_doc(doc_id) {
            Ok(od) => (od.doc.path().to_path_buf(), od.doc.text().revision()),
            Err(e) => return Reply::Now(Err(e)),
        };
        let syntax = match self.index.file(&path) {
            Some(entry) => {
                let (status, rev) = match entry.version {
                    SourceVersion::Buffer { revision: r } if r == revision => ("current", Some(r)),
                    SourceVersion::Buffer { revision: r } => ("stale", Some(r)),
                    SourceVersion::Disk { .. } => ("stale", None),
                };
                let items: Vec<Value> = entry
                    .symbols
                    .iter()
                    .map(|s| {
                        json!({
                            "name": s.name,
                            "kind": s.kind.as_str(),
                            "containerName": s.parent.and_then(|p| entry.symbols.get(p)).map(|p| p.name.clone()),
                            "range": Range {
                                start: line_col_position(self.encoding, s.name_start),
                                end: line_col_position(self.encoding, s.name_end),
                            },
                        })
                    })
                    .collect();
                self.syntax_evidence(status, rev, Value::Array(items))
            }
            None => {
                let mut e =
                    self.syntax_evidence("unavailable", Some(revision), Value::Array(Vec::new()));
                e.error = Some("the document has not been parsed yet".to_string());
                e
            }
        };
        let params = json!({ "textDocument": { "uri": path_to_uri(&path) } });
        self.ask_language_server(
            Ask {
                query: Query::DocumentSymbols,
                params,
                doc_id: Some(doc_id),
                revision: Some(revision),
                timeout_ms: None,
                syntax: vec![syntax],
            },
            id,
        )
    }

    pub(super) fn intel_workspace_symbols(
        &mut self,
        params: WorkspaceSymbolsParams,
        id: Option<Value>,
    ) -> Reply {
        if let Err(e) = self.workspace() {
            return Reply::Now(Err(e));
        }
        if params.query.is_empty() {
            return Reply::Now(Err(rpc_error(
                codes::INVALID_PARAMS,
                "query must not be empty",
            )));
        }
        let max = params
            .max_results
            .unwrap_or(DEFAULT_WORKSPACE_SYMBOLS)
            .min(MAX_WORKSPACE_SYMBOLS);
        let items: Vec<Value> = self
            .index
            .search(&params.query, max)
            .into_iter()
            .map(|m| {
                json!({
                    "name": m.symbol.name,
                    "kind": m.symbol.kind.as_str(),
                    "containerName": m.container.map(|c| c.name.clone()),
                    "location": {
                        "path": self.relative_display(m.path),
                        "range": Range {
                            start: line_col_position(self.encoding, m.symbol.name_start),
                            end: line_col_position(self.encoding, m.symbol.name_end),
                        },
                    },
                })
            })
            .collect();
        let syntax = self.syntax_evidence("current", None, Value::Array(items));
        self.ask_language_server(
            Ask {
                query: Query::WorkspaceSymbols { max },
                params: json!({ "query": params.query }),
                doc_id: None,
                revision: None,
                timeout_ms: params.timeout_ms,
                syntax: vec![syntax],
            },
            id,
        )
    }

    pub(super) fn intel_diagnostics(&self, doc_id: u64) -> Result<Value, ServerError> {
        let od = self.open_doc(doc_id)?;
        let revision = od.doc.text().revision();
        let syntax = match &od.syntax.diagnostics {
            Some((rev, list)) => self.syntax_evidence(
                if *rev == revision { "current" } else { "stale" },
                Some(*rev),
                serde_json::to_value(list).unwrap_or_default(),
            ),
            None => {
                let mut e =
                    self.syntax_evidence("unavailable", Some(revision), Value::Array(Vec::new()));
                e.error = Some(if od.is_rust {
                    "the document has not been parsed yet".to_string()
                } else {
                    "no syntax analysis for this file type".to_string()
                });
                e
            }
        };
        let semantic = match self.semantic_diagnostics_params(doc_id) {
            Some(params) => Evidence {
                source: params.source.clone(),
                kind: "semantic".to_string(),
                status: if params.current { "current" } else { "stale" }.to_string(),
                document_revision: params.revision,
                generation: Some(params.generation),
                complete: self.semantic.quiescent(),
                limitations: {
                    let mut l = self.semantic_limitations();
                    if params.revision.is_none() {
                        l.push(
                            "the server did not say which version these diagnostics describe"
                                .to_string(),
                        );
                    }
                    l
                },
                error: None,
                elapsed_ms: 0,
                items: serde_json::to_value(&params.diagnostics).unwrap_or_default(),
            },
            None => match self.semantic_blocker(Some(doc_id), Query::Hover) {
                Some(reason) => self.unavailable(reason, Some(revision)),
                None => self.unavailable(
                    "the language server has not published diagnostics for this document yet"
                        .to_string(),
                    Some(revision),
                ),
            },
        };
        self.intel_reply(Some(doc_id), vec![semantic, syntax])
    }

    pub(super) fn intel_status(&self) -> Result<Value, ServerError> {
        self.workspace()?;
        let stats = self.index.stats();
        Ok(json!({
            "syntax": {
                "source": "tree-sitter",
                "evidence": EVIDENCE,
                "generation": stats.generation,
                "idle": self.index_idle(),
                "filesIndexed": stats.files,
                "symbols": stats.symbols,
            },
            "semantic": self.lsp_metrics(),
            "graph": self.graph_metrics(),
            "limitations": self.semantic_limitations(),
        }))
    }
}

fn hover_text(contents: &Value) -> (String, &'static str) {
    match contents {
        Value::String(s) => (s.clone(), "markdown"),
        Value::Object(o) if o.contains_key("kind") => (
            o.get("value")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string(),
            if o.get("kind").and_then(Value::as_str) == Some("plaintext") {
                "plaintext"
            } else {
                "markdown"
            },
        ),
        Value::Object(o) => (
            format!(
                "```{}\n{}\n```",
                o.get("language")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
                o.get("value").and_then(Value::as_str).unwrap_or_default()
            ),
            "markdown",
        ),
        Value::Array(parts) => (
            parts
                .iter()
                .map(|p| hover_text(p).0)
                .collect::<Vec<_>>()
                .join("\n\n"),
            "markdown",
        ),
        _ => (String::new(), "plaintext"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identifiers_are_found_around_the_cursor() {
        let buffer = TextBuffer::new("let سلام = other::helper_fn(x);\n");
        let at = |s: &str| ByteOffset(buffer.text().find(s).unwrap() + 2);
        assert_eq!(
            Session::identifier_at(&buffer, at("helper_fn")).as_deref(),
            Some("helper_fn")
        );
        assert_eq!(
            Session::identifier_at(&buffer, at("سلام")).as_deref(),
            Some("سلام")
        );
        assert_eq!(
            Session::identifier_at(&buffer, ByteOffset(3)).as_deref(),
            Some("let")
        );
        assert_eq!(
            Session::identifier_at(&buffer, at("(x")).as_deref(),
            Some("x")
        );
        // At the first character of an identifier.
        let start = ByteOffset(buffer.text().find("other").unwrap());
        assert_eq!(
            Session::identifier_at(&buffer, start).as_deref(),
            Some("other")
        );
        assert_eq!(
            Session::identifier_at(&buffer, ByteOffset(buffer.text().find(" = ").unwrap() + 1)),
            None
        );
    }

    #[test]
    fn hover_contents_of_every_shape() {
        assert_eq!(hover_text(&json!("plain")).0, "plain");
        assert_eq!(
            hover_text(&json!({"kind":"plaintext","value":"v"})),
            ("v".to_string(), "plaintext")
        );
        assert_eq!(
            hover_text(&json!({"language":"rust","value":"fn f()"})).0,
            "```rust\nfn f()\n```"
        );
        assert_eq!(
            hover_text(&json!(["a", {"kind":"markdown","value":"b"}])).0,
            "a\n\nb"
        );
    }

    #[test]
    fn kind_tables() {
        assert_eq!(kind_name(SYMBOL_KINDS, &json!(12)), "function");
        assert_eq!(kind_name(SYMBOL_KINDS, &json!(23)), "struct");
        assert_eq!(kind_name(COMPLETION_KINDS, &json!(3)), "function");
        assert_eq!(kind_name(SYMBOL_KINDS, &json!(0)), "unknown");
        assert_eq!(kind_name(SYMBOL_KINDS, &json!(99)), "unknown");
    }
}
