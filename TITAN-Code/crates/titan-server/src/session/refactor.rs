//! Semantic refactoring through the language server: rename.
//!
//! `refactor.rename` asks rust-analyzer (`textDocument/prepareRename`, then
//! `textDocument/rename`) and turns the returned `WorkspaceEdit` into a Verified Changes
//! proposal — nothing is applied. Versioned document edits become buffer changes expected at
//! exactly that revision; edits to files that are not open become disk changes expected at
//! their current content hash; file renames become rename changes; change annotations are
//! carried over. The proposal then goes through preview, explicit approval, revision-checked
//! application, evidence and restore like any other proposal.

use std::path::Path;
use std::time::Instant;

use serde::Deserialize;
use serde_json::{Value, json};
use titan_changes::{
    BaseSnapshot, ByteEdit, ChangeAnnotation, ExpectedBase, FileChange, Proposal, ProposalSource,
};
use titan_index::graph::NodeKey;
use titan_lsp::LspError;
use titan_protocol::messages::Position;
use titan_protocol::rpc::codes;
use titan_text::{TextBuffer, Utf16Point};

use super::impact::Job;
use super::intel::{Part, Query};
use super::lsp::uri_to_path;
use super::{Reply, Session, rpc_error};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub(super) struct RenameParams {
    document_id: u64,
    expected_revision: u64,
    position: Position,
    new_name: String,
}

/// Changes, annotations and conversion notes of a `WorkspaceEdit`.
type ConvertedEdit = (Vec<FileChange>, Vec<ChangeAnnotation>, Vec<String>);

pub(super) struct RenameJob {
    pub(super) id: Option<Value>,
    doc_id: u64,
    revision: u64,
    new_name: String,
    lsp: Value,
    generation: u64,
    started: Instant,
    old_key: Option<NodeKey>,
}

impl Session {
    pub(super) fn refactor_rename(&mut self, params: Value, id: Option<Value>) -> Reply {
        let p: RenameParams = match super::parse(params) {
            Ok(p) => p,
            Err(e) => return Reply::Now(Err(e)),
        };
        if p.new_name.trim().is_empty() || p.new_name.chars().any(char::is_whitespace) {
            return Reply::Now(Err(rpc_error(
                codes::INVALID_PARAMS,
                "newName must be one identifier",
            )));
        }
        let position = titan_protocol::messages::PositionParams {
            document_id: p.document_id,
            expected_revision: p.expected_revision,
            position: p.position,
            include_declaration: false,
            timeout_ms: None,
        };
        let (lsp, offset) = match self.prepare_position(Query::Hover, &position) {
            Ok(v) => v,
            Err(e) => return Reply::Now(Err(e)),
        };
        self.semantic.next_job += 1;
        let job = self.semantic.next_job;
        if let Some(reason) = self.semantic_blocker(
            Some(p.document_id),
            Query::Part {
                job,
                part: Part::Rename,
            },
        ) {
            return Reply::Now(Err(rpc_error(
                codes::SEMANTIC_UNAVAILABLE,
                format!("rename needs the language server: {reason}"),
            )));
        }
        let old_key = self.symbol_key_at(p.document_id, offset);
        let prepare_supported = self
            .semantic
            .supervisor
            .as_ref()
            .and_then(titan_lsp::Supervisor::server_capabilities)
            .is_some_and(|c| c["renameProvider"]["prepareProvider"] == true);
        let rename = RenameJob {
            id,
            doc_id: p.document_id,
            revision: p.expected_revision,
            new_name: p.new_name,
            lsp: lsp.clone(),
            generation: self.semantic.generation,
            started: Instant::now(),
            old_key,
        };
        let (part, params) = if prepare_supported {
            (Part::PrepareRename, lsp)
        } else {
            let mut params = lsp;
            params["newName"] = json!(rename.new_name);
            (Part::Rename, params)
        };
        if !self.send_part(job, part, params, p.document_id, p.expected_revision) {
            return Reply::Now(Err(rpc_error(
                codes::SEMANTIC_UNAVAILABLE,
                "could not send the request",
            )));
        }
        self.semantic
            .jobs
            .insert(job, Job::Rename(Box::new(rename)));
        Reply::Deferred
    }

    /// The graph identity of the definition under `offset`, when the cursor is on one.
    fn symbol_key_at(&self, doc_id: u64, offset: titan_text::ByteOffset) -> Option<NodeKey> {
        let od = self.documents.get(&doc_id)?;
        let word = Self::identifier_at(od.doc.text().buffer(), offset)?;
        let line = od.doc.text().buffer().byte_to_point(offset).ok()?.line;
        let rel = self.workspace.as_ref()?.relative(od.doc.path())?;
        let graph = self.graph.graph.as_deref()?;
        graph
            .nodes_in_file(&rel)
            .into_iter()
            .find(|n| *n.name == *word && n.start.is_some_and(|s| s.line == line))
            .map(|n| n.key.clone())
    }

    pub(super) fn on_rename_part(
        &mut self,
        job: u64,
        rename: RenameJob,
        part: Part,
        result: Result<Value, LspError>,
    ) {
        let fail = |this: &mut Session, id: Option<Value>, code: i64, message: String| {
            if let Some(id) = id {
                this.respond(id, Err(rpc_error(code, message)));
            }
        };
        match (part, result) {
            (Part::PrepareRename, Ok(Value::Null)) => {
                fail(
                    self,
                    rename.id,
                    codes::INVALID_PARAMS,
                    "rust-analyzer cannot rename at this position".to_string(),
                );
            }
            (Part::PrepareRename, Ok(_)) | (Part::PrepareRename, Err(LspError::Server { .. })) => {
                let mut params = rename.lsp.clone();
                params["newName"] = json!(rename.new_name);
                if self.send_part(job, Part::Rename, params, rename.doc_id, rename.revision) {
                    self.semantic
                        .jobs
                        .insert(job, Job::Rename(Box::new(rename)));
                } else {
                    fail(
                        self,
                        rename.id,
                        codes::SEMANTIC_UNAVAILABLE,
                        "could not send the rename".to_string(),
                    );
                }
            }
            (Part::Rename, Ok(edit)) if !edit.is_null() => {
                let id = rename.id.clone();
                let result = self.rename_proposal(&rename, &edit);
                if let Some(id) = id {
                    self.respond(id, result);
                }
            }
            (Part::Rename, Ok(_)) => {
                fail(
                    self,
                    rename.id,
                    codes::INVALID_PARAMS,
                    "rust-analyzer returned no edit".to_string(),
                );
            }
            (_, Err(e)) => {
                fail(
                    self,
                    rename.id,
                    codes::SEMANTIC_UNAVAILABLE,
                    format!("rename failed: {e}"),
                );
            }
            (other, Ok(_)) => {
                fail(
                    self,
                    rename.id,
                    codes::INTERNAL_ERROR,
                    format!("unexpected part {other:?}"),
                );
            }
        }
    }

    fn rename_proposal(
        &mut self,
        rename: &RenameJob,
        edit: &Value,
    ) -> Result<Value, super::ServerError> {
        let root = self.workspace()?.root().to_path_buf();
        let (changes, annotations, notes) = self
            .workspace_edit_to_changes(edit)
            .map_err(|e| rpc_error(codes::INVALID_PARAMS, e))?;
        let mut proposal = Proposal::new(
            ProposalSource {
                kind: "lsp.rename".to_string(),
                detail: json!({
                    "server": self.semantic_name(),
                    "generation": rename.generation,
                    "documentId": rename.doc_id,
                    "documentRevision": rename.revision,
                    "request": rename.lsp,
                    "newName": rename.new_name,
                    "symbol": rename.old_key.as_ref().map(NodeKey::display),
                    "elapsedMs": rename.started.elapsed().as_millis() as u64,
                    "notes": notes,
                }),
            },
            BaseSnapshot {
                git_head: None,
                workspace_root: root,
            },
            changes,
        );
        proposal.annotations = annotations;
        proposal.expected_result = Some(format!(
            "every reference rust-analyzer knows is renamed to `{}`",
            rename.new_name
        ));
        proposal.preconditions = vec![
            "every edited document still has the revision rust-analyzer answered for".to_string(),
            "every edited file that is not open still has the content it had when proposed"
                .to_string(),
        ];
        let renamed = rename.old_key.clone().map(|k| (k, rename.new_name.clone()));
        self.store_proposal(proposal, renamed)
    }

    /// Convert an LSP `WorkspaceEdit` (positions in UTF-16, versioned document edits) into
    /// proposal changes with byte offsets.
    fn workspace_edit_to_changes(&self, edit: &Value) -> Result<ConvertedEdit, String> {
        let annotations: Vec<ChangeAnnotation> = edit["changeAnnotations"]
            .as_object()
            .into_iter()
            .flatten()
            .map(|(id, a)| ChangeAnnotation {
                id: id.clone(),
                label: a["label"].as_str().unwrap_or(id).to_string(),
                needs_confirmation: a["needsConfirmation"].as_bool().unwrap_or(false),
            })
            .collect();
        let mut notes = Vec::new();
        let mut entries: Vec<(String, Option<i64>, Value)> = Vec::new();
        let mut file_ops = Vec::new();
        if let Some(doc_changes) = edit["documentChanges"].as_array() {
            for change in doc_changes {
                if change.get("textDocument").is_some() {
                    entries.push((
                        change["textDocument"]["uri"]
                            .as_str()
                            .unwrap_or_default()
                            .to_string(),
                        change["textDocument"]["version"].as_i64(),
                        change["edits"].clone(),
                    ));
                } else {
                    file_ops.push(change.clone());
                }
            }
        } else if let Some(map) = edit["changes"].as_object() {
            notes.push(
                "unversioned edits (`changes`): revisions are taken from the current buffers"
                    .to_string(),
            );
            for (uri, edits) in map {
                entries.push((uri.clone(), None, edits.clone()));
            }
        }
        let ws = self.workspace.as_ref().ok_or("no workspace")?;
        let mut out = Vec::new();
        for (uri, version, edits) in entries {
            let abs = uri_to_path(&uri).ok_or_else(|| format!("unsupported URI {uri}"))?;
            let rel = ws.relative(&abs).ok_or_else(|| {
                format!(
                    "rust-analyzer edits a file outside the workspace: {}",
                    abs.display()
                )
            })?;
            let rel_str = rel.to_string_lossy().replace('\\', "/");
            let (buffer, expected, bom) =
                match self.by_path.get(&abs).and_then(|id| self.documents.get(id)) {
                    Some(od) => {
                        let text = od.doc.text().buffer().text();
                        let revision =
                            version.map_or(od.doc.text().revision(), |v| v.max(0) as u64);
                        (
                            od.doc.text().buffer().clone(),
                            ExpectedBase::buffer(revision, &text),
                            0,
                        )
                    }
                    None => {
                        let bytes = std::fs::read(&abs).map_err(|e| format!("{rel_str}: {e}"))?;
                        let bom = if bytes.starts_with(&[0xEF, 0xBB, 0xBF]) {
                            3
                        } else {
                            0
                        };
                        let text = std::str::from_utf8(&bytes[bom..])
                            .map_err(|_| format!("{rel_str} is not UTF-8"))?;
                        (TextBuffer::new(text), ExpectedBase::disk(&bytes), bom)
                    }
                };
            let mut byte_edits = Vec::new();
            for e in edits.as_array().into_iter().flatten() {
                let at = |p: &Value| -> Result<usize, String> {
                    let point = Utf16Point::new(
                        p["line"].as_u64().unwrap_or(0) as usize,
                        p["character"].as_u64().unwrap_or(0) as usize,
                    );
                    buffer
                        .utf16_point_to_byte(point)
                        .map(|b| b.0 + bom)
                        .map_err(|err| format!("{rel_str}: {err}"))
                };
                byte_edits.push(ByteEdit {
                    start: at(&e["range"]["start"])?,
                    end: at(&e["range"]["end"])?,
                    text: e["newText"].as_str().unwrap_or_default().to_string(),
                    annotation: e["annotationId"].as_str().map(str::to_string),
                });
            }
            byte_edits.sort_by_key(|e| e.start);
            out.push(FileChange::Edit {
                path: rel_str,
                expected,
                edits: byte_edits,
            });
        }
        for op in file_ops {
            let rel = |key: &str| -> Result<String, String> {
                let abs = uri_to_path(op[key].as_str().unwrap_or_default()).ok_or("bad URI")?;
                ws.relative(&abs)
                    .map(|r| r.to_string_lossy().replace('\\', "/"))
                    .ok_or_else(|| {
                        format!("file operation outside the workspace: {}", abs.display())
                    })
            };
            let annotation = op["annotationId"].as_str().map(str::to_string);
            match op["kind"].as_str() {
                Some("rename") => {
                    let from = rel("oldUri")?;
                    let bytes = std::fs::read(ws.root().join(Path::new(&from)))
                        .map_err(|e| e.to_string())?;
                    out.push(FileChange::Rename {
                        to: rel("newUri")?,
                        expected: ExpectedBase::disk(&bytes),
                        from,
                        annotation,
                    });
                }
                Some("create") => out.push(FileChange::Create {
                    path: rel("uri")?,
                    content: String::new(),
                    annotation,
                }),
                Some("delete") => {
                    let path = rel("uri")?;
                    let bytes = std::fs::read(ws.root().join(Path::new(&path)))
                        .map_err(|e| e.to_string())?;
                    out.push(FileChange::Delete {
                        path,
                        expected: ExpectedBase::disk(&bytes),
                        annotation,
                    });
                }
                other => notes.push(format!("ignored unknown document change {other:?}")),
            }
        }
        Ok((out, annotations, notes))
    }
}
