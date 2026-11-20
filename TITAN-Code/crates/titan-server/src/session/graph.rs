//! Project Twin service: incremental graph maintenance and headless queries.
//!
//! * **Facts cache.** Per-file facts (module declarations, `use` paths, definitions, tests)
//!   are cached by file version. Open documents get fresh facts from every published parse
//!   (computed on the parse worker from the same tree); disk files from background builds,
//!   which re-parse only files whose index version changed.
//! * **Local updates.** A published parse whose module declarations and `use` paths are
//!   unchanged updates the graph in place on the event loop (microseconds for one file);
//!   anything structural marks the graph stale.
//! * **Rebuilds.** A stale graph is re-assembled on a worker from cached facts, at most once
//!   per [`MIN_REBUILD_INTERVAL`], keeping the identity table so ids stay stable.
//! * **Freshness.** Every node and edge is reported with the content version it was derived
//!   from and whether that is still the current version of its file.

use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{Value, json};
use titan_index::SourceVersion;
use titan_index::graph::{
    EdgeKind, FileFacts, GraphEdge, GraphInput, GraphNode, LIMITATIONS, NodeId, NodeKind,
    ProjectGraph, build_graph, collect_facts,
};
use titan_protocol::messages::{GraphNodeParams, GraphQueryParams};
use titan_protocol::rpc::codes;
use titan_tasks::{Priority, TaskId, TaskOutcome};

use super::{
    MAX_INDEXED_FILE_BYTES, MAX_LIST_LIMIT, Pending, ServerError, Session, TaskResult,
    display_path, is_rust_file, rpc_error, with_parser,
};

const MIN_REBUILD_INTERVAL: Duration = Duration::from_millis(1_000);
/// In-place updates of open documents are coalesced: at most one per interval (the latest
/// facts win), so fast typing in a large file does not repeat the update on the event loop.
const LOCAL_UPDATE_INTERVAL: Duration = Duration::from_millis(250);
const DEFAULT_QUERY_RESULTS: usize = 500;
const MAX_QUERY_RESULTS: usize = 10_000;

type FactsCache = HashMap<PathBuf, (SourceVersion, Arc<FileFacts>)>;

#[derive(Default)]
pub(super) struct GraphState {
    /// Incremented on every reset (workspace open/close): results of builds started under
    /// an older epoch are dropped.
    pub epoch: u64,
    /// Not built: disabled by configuration, too many files, or memory pressure.
    pub paused: Option<String>,
    pub graph: Option<Box<ProjectGraph>>,
    pub building: Option<TaskId>,
    pub stale: bool,
    /// Something changed while a build ran: rebuild once it is published.
    pub changed_during_build: bool,
    pub last_start: Option<Instant>,
    pub built_unix_ms: Option<u64>,
    pub build_ms: u64,
    pub input_files: usize,
    pub parsed_last_build: usize,
    pub reused_last_build: usize,
    pub builds: u64,
    pub failures: u64,
    pub local_update_us: Vec<u64>,
    /// Files with newer facts waiting for the next coalesced in-place update.
    pub pending_local: BTreeMap<PathBuf, SourceVersion>,
    pub last_local_apply: Option<Instant>,
    pub coalesced_updates: u64,
    /// Workspace-relative path → (version, facts).
    pub facts: FactsCache,
}

pub(crate) struct GraphOutput {
    pub graph: Option<ProjectGraph>,
    pub facts: FactsCache,
    pub input_files: usize,
    pub parsed: usize,
    pub reused: usize,
    pub elapsed_ms: u64,
}

fn version_json(version: Option<SourceVersion>) -> Value {
    match version {
        Some(SourceVersion::Buffer { revision }) => {
            json!({ "source": "buffer", "revision": revision })
        }
        Some(SourceVersion::Disk { len, .. }) => json!({ "source": "disk", "len": len }),
        None => Value::Null,
    }
}

impl Session {
    fn graph(&self) -> Option<&ProjectGraph> {
        self.graph.graph.as_deref()
    }

    /// The current version of a workspace-relative file, as the index knows it.
    fn current_version(&self, rel: &Path) -> Option<SourceVersion> {
        let ws = self.workspace.as_ref()?;
        let abs = ws.root().join(rel);
        if let Some(&id) = self.by_path.get(&abs) {
            let rev = self.documents.get(&id)?.doc.text().revision();
            return Some(SourceVersion::Buffer { revision: rev });
        }
        self.index.file(&abs).map(|f| f.version)
    }

    fn freshness(&self, path: Option<&Path>, version: Option<SourceVersion>) -> &'static str {
        match (path, version) {
            (Some(path), Some(version)) => match self.current_version(path) {
                Some(current) if current == version => "current",
                Some(_) => "stale",
                None => "unknown",
            },
            _ => "structural",
        }
    }

    pub(super) fn node_json(&self, node: &GraphNode) -> Value {
        json!({
            "id": node.id.to_string(),
            "key": node.key.display(),
            "kind": node.kind.as_str(),
            "name": &*node.name,
            "path": node.path.as_deref().map(display_path),
            "range": node.start.map(|s| json!({
                "startLine": s.line,
                "endLine": node.end.map(|e| e.line),
            })),
            "detail": node.detail,
            "version": version_json(node.version),
            "freshness": self.freshness(node.path.as_deref(), node.version),
        })
    }

    pub(super) fn edge_json(&self, edge: &GraphEdge) -> Value {
        let p = &edge.provenance;
        json!({
            "from": edge.from.to_string(),
            "to": edge.to.to_string(),
            "kind": edge.kind.as_str(),
            "provenance": {
                "source": p.source,
                "analysis": p.analysis,
                "evidence": p.kind,
                "path": p.path.as_deref().map(display_path),
                "line": p.line,
                "version": version_json(p.version),
                "generation": p.generation,
                "freshness": self.freshness(p.path.as_deref(), p.version),
            },
        })
    }

    pub(super) fn mark_graph_stale(&mut self) {
        if self.workspace.is_some() {
            self.graph.stale = true;
            if self.graph.building.is_some() {
                self.graph.changed_during_build = true;
            }
        }
    }

    pub(super) fn reset_graph(&mut self) {
        if let Some(task) = self.graph.building.take() {
            self.pool.cancel(task);
        }
        let epoch = self.graph.epoch + 1;
        let paused = if !self.options.graph {
            Some("disabled (--no-graph)".to_string())
        } else {
            self.memory_pressure_reason()
        };
        self.graph = GraphState {
            epoch,
            paused,
            stale: self.workspace.is_some(),
            ..GraphState::default()
        };
    }

    /// A parse of an open document was published: remember its facts; the in-place graph
    /// update runs coalesced on the next tick (see [`LOCAL_UPDATE_INTERVAL`]).
    pub(super) fn graph_file_parsed(
        &mut self,
        abs: &Path,
        version: SourceVersion,
        facts: FileFacts,
    ) {
        let Some(rel) = self.workspace.as_ref().and_then(|ws| ws.relative(abs)) else {
            return;
        };
        self.graph
            .facts
            .insert(rel.clone(), (version, Arc::new(facts)));
        if self.graph.pending_local.insert(rel, version).is_some() {
            self.graph.coalesced_updates += 1;
        }
    }

    /// Apply pending in-place updates (called on every tick).
    pub(super) fn apply_pending_graph_updates(&mut self) {
        if self.graph.pending_local.is_empty()
            || self
                .graph
                .last_local_apply
                .is_some_and(|t| t.elapsed() < LOCAL_UPDATE_INTERVAL)
        {
            return;
        }
        self.graph.last_local_apply = Some(Instant::now());
        for (rel, version) in std::mem::take(&mut self.graph.pending_local) {
            let Some((cached, facts)) = self.graph.facts.get(&rel).cloned() else {
                continue;
            };
            if cached != version {
                continue;
            }
            let started = Instant::now();
            let result = match self.graph.graph.as_mut() {
                Some(graph) => graph.update_file(&rel, version, &facts),
                None => {
                    self.mark_graph_stale();
                    continue;
                }
            };
            match result {
                Ok(()) => {
                    let us = u64::try_from(started.elapsed().as_micros()).unwrap_or(u64::MAX);
                    if self.graph.local_update_us.len() >= 1_000 {
                        self.graph.local_update_us.remove(0);
                    }
                    self.graph.local_update_us.push(us);
                    if self.graph.building.is_some() {
                        self.graph.changed_during_build = true;
                    }
                }
                Err(_) => self.mark_graph_stale(),
            }
        }
    }

    /// A rename reported by the file watcher: keep the file's identity.
    pub(super) fn graph_file_renamed(&mut self, from: &Path, to: &Path) {
        let Some(ws) = self.workspace.as_ref() else {
            return;
        };
        let (Some(from), Some(to)) = (ws.relative(from), ws.relative(to)) else {
            return;
        };
        if let Some(graph) = self.graph.graph.as_mut() {
            graph.rename_file(&from, &to);
        }
        if let Some(entry) = self.graph.facts.remove(&from) {
            self.graph.facts.insert(to, entry);
        }
        self.mark_graph_stale();
    }

    /// Called on every tick: start a build when one is due.
    pub(super) fn maybe_build_graph(&mut self) {
        // Disk versions come from the index: wait until a scan has published them.
        if !self.graph.stale
            || self.graph.building.is_some()
            || self.scanning.is_some()
            || self.graph.paused.is_some()
        {
            return;
        }
        let files = self.index.stats().files;
        if files > self.options.graph_max_files {
            self.graph.paused = Some(format!(
                "the workspace has {files} indexed files, above the graph limit of {} (--graph-max-files)",
                self.options.graph_max_files
            ));
            return;
        }
        if self
            .graph
            .last_start
            .is_some_and(|t| t.elapsed() < MIN_REBUILD_INTERVAL)
        {
            return;
        }
        let Some(workspace) = self.workspace.clone() else {
            return;
        };
        // Versions known now: open buffers (with snapshots) and indexed disk files.
        let mut buffers: BTreeMap<PathBuf, (titan_text::TextSnapshot, u64)> = BTreeMap::new();
        for od in self.documents.values().filter(|od| od.is_rust) {
            if let Some(rel) = workspace.relative(od.doc.path()) {
                let snap = od.doc.text().snapshot();
                let revision = snap.revision();
                buffers.insert(rel, (snap, revision));
            }
        }
        let disk: BTreeMap<PathBuf, SourceVersion> = self
            .index
            .versions()
            .filter_map(|(abs, v)| workspace.relative(abs).map(|rel| (rel, v)))
            .filter(|(rel, _)| !buffers.contains_key(rel))
            .collect();
        let cache = self.graph.facts.clone();
        let identities = self
            .graph
            .graph
            .as_deref()
            .map(ProjectGraph::identities_only);
        let submitted = self
            .pool
            .submit("graph.build", Priority::Low, None, move |token| {
                let started = Instant::now();
                let mut out = GraphOutput {
                    graph: None,
                    facts: HashMap::new(),
                    input_files: 0,
                    parsed: 0,
                    reused: 0,
                    elapsed_ms: 0,
                };
                let mut input = GraphInput {
                    root_name: workspace
                        .root()
                        .file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default(),
                    ..GraphInput::default()
                };
                for rel in workspace.list_files(MAX_LIST_LIMIT).files {
                    if rel.file_name().is_some_and(|n| n == "Cargo.toml") {
                        if let Ok(text) = std::fs::read_to_string(workspace.root().join(&rel)) {
                            input.manifests.insert(rel, text);
                        }
                    }
                }
                let mut cancelled = false;
                {
                    let mut take = |rel: &PathBuf, version: SourceVersion, text: Option<String>| {
                        let facts = match cache.get(rel) {
                            Some((v, f)) if *v == version => {
                                out.reused += 1;
                                Arc::clone(f)
                            }
                            _ => {
                                let text = text.or_else(|| {
                                    let path = workspace.root().join(rel);
                                    let meta = std::fs::metadata(&path).ok()?;
                                    if meta.len() > MAX_INDEXED_FILE_BYTES {
                                        return None;
                                    }
                                    std::fs::read_to_string(path).ok()
                                });
                                let Some(text) = text else { return };
                                let Some(facts) = with_parser(|p| collect_facts(p, &text)) else {
                                    return;
                                };
                                out.parsed += 1;
                                Arc::new(facts)
                            }
                        };
                        out.facts.insert(rel.clone(), (version, Arc::clone(&facts)));
                        input.files.insert(rel.clone(), (version, facts));
                    };
                    for (rel, (snap, revision)) in &buffers {
                        if token.is_cancelled() {
                            cancelled = true;
                            break;
                        }
                        let version = SourceVersion::Buffer {
                            revision: *revision,
                        };
                        let text = match cache.get(rel) {
                            Some((v, _)) if *v == version => None,
                            _ => Some(snap.buffer().text()),
                        };
                        take(rel, version, text);
                    }
                    for (rel, version) in &disk {
                        if cancelled || token.is_cancelled() {
                            cancelled = true;
                            break;
                        }
                        if is_rust_file(rel) {
                            take(rel, *version, None);
                        }
                    }
                }
                if cancelled {
                    return TaskResult::Graph(Box::new(out));
                }
                out.input_files = input.files.len() + input.manifests.len();
                out.graph = Some(build_graph(&input, identities));
                out.elapsed_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
                TaskResult::Graph(Box::new(out))
            });
        if let Ok((task, _)) = submitted {
            self.graph.building = Some(task);
            self.graph.stale = false;
            self.graph.changed_during_build = false;
            self.graph.last_start = Some(Instant::now());
            self.pending.insert(
                task,
                Pending::Graph {
                    epoch: self.graph.epoch,
                },
            );
        }
    }

    pub(super) fn on_graph_built(&mut self, epoch: u64, outcome: TaskOutcome<TaskResult>) {
        if epoch != self.graph.epoch {
            // Built for a workspace that was closed or reopened meanwhile.
            return;
        }
        self.graph.building = None;
        match outcome {
            TaskOutcome::Completed(TaskResult::Graph(output)) => match output.graph {
                Some(graph) => {
                    self.graph.graph = Some(Box::new(graph));
                    // Keep newer facts that arrived during the build.
                    let mut facts = output.facts;
                    for (path, entry) in std::mem::take(&mut self.graph.facts) {
                        let newer_buffer = matches!(entry.0, SourceVersion::Buffer { .. })
                            && facts.get(&path).is_none_or(|(v, _)| *v != entry.0);
                        if newer_buffer {
                            facts.insert(path, entry);
                        }
                    }
                    self.graph.facts = facts;
                    self.graph.build_ms = output.elapsed_ms;
                    self.graph.input_files = output.input_files;
                    self.graph.parsed_last_build = output.parsed;
                    self.graph.reused_last_build = output.reused;
                    self.graph.builds += 1;
                    self.graph.built_unix_ms = SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .ok()
                        .map(|d| d.as_millis() as u64);
                    if self.graph.changed_during_build {
                        self.graph.stale = true;
                    }
                }
                None => self.graph.stale = true,
            },
            TaskOutcome::Cancelled | TaskOutcome::TimedOut => self.graph.stale = true,
            _ => self.graph.failures += 1,
        }
    }

    fn graph_state_name(&self) -> &'static str {
        if self.graph.paused.is_some() && self.graph().is_none() {
            return "paused";
        }
        match (self.graph(), self.graph.building.is_some()) {
            (None, true) => "building",
            (None, false) => "none",
            (Some(_), true) => "rebuilding",
            (Some(_), false) => "ready",
        }
    }

    pub(super) fn graph_header(&self) -> Value {
        json!({
            "generation": self.graph().map_or(0, ProjectGraph::generation),
            "state": self.graph_state_name(),
            // Structural changes since the last build are not reflected yet.
            "stale": self.graph.stale
                || self.graph.building.is_some()
                || !self.graph.pending_local.is_empty(),
            "evidence": "Cargo manifests + tree-sitter-rust syntax",
            "paused": self.graph.paused,
        })
    }

    pub(super) fn graph_cache_stats(&self) -> Value {
        let mut us = self.graph.local_update_us.clone();
        us.sort_unstable();
        let pick = |q: f64| {
            us.get(((us.len().saturating_sub(1)) as f64 * q) as usize)
                .copied()
        };
        json!({
            "cachedFiles": self.graph.facts.len(),
            "parsedLastBuild": self.graph.parsed_last_build,
            "reusedLastBuild": self.graph.reused_last_build,
            "lastBuildMs": self.graph.build_ms,
            "localUpdates": us.len(),
            "coalescedUpdates": self.graph.coalesced_updates,
            "pendingLocalUpdates": self.graph.pending_local.len(),
            "localUpdateUs": { "median": pick(0.5), "p95": pick(0.95), "max": us.last() },
        })
    }

    pub(super) fn graph_summary(&self) -> Result<Value, ServerError> {
        self.workspace()?;
        let mut out = self.graph_header();
        out["builtUnixMs"] = json!(self.graph.built_unix_ms);
        out["inputFiles"] = json!(self.graph.input_files);
        out["limitations"] = json!(LIMITATIONS);
        out["cache"] = self.graph_cache_stats();
        if let Some(graph) = self.graph() {
            out["nodes"] = NodeKind::ALL
                .iter()
                .map(|k| (k.as_str().to_string(), json!(graph.count(*k))))
                .collect::<serde_json::Map<_, _>>()
                .into();
            out["edges"] = EdgeKind::ALL
                .iter()
                .map(|k| (k.as_str().to_string(), json!(graph.edge_count(*k))))
                .collect::<serde_json::Map<_, _>>()
                .into();
            out["packages"] = graph
                .nodes()
                .filter(|n| n.kind == NodeKind::Package)
                .map(|n| self.node_json(n))
                .collect::<Vec<_>>()
                .into();
            let s = &graph.stats;
            out["stats"] = json!({
                "filesWithSyntaxErrors": s.files_with_syntax_errors,
                "unreachableFiles": s.unreachable_files,
                "unresolvedModules": s.unresolved_modules,
                "unresolvedImports": s.unresolved_imports,
                "externalImports": s.external_imports,
                "manifestErrors": s.manifest_errors,
                "fullBuilds": s.full_builds,
                "localUpdates": s.local_updates,
                "rebuildsRequested": s.rebuilds_requested,
            });
        }
        Ok(out)
    }

    pub(super) fn built_graph(&self) -> Result<&ProjectGraph, ServerError> {
        self.workspace()?;
        self.graph().ok_or_else(|| {
            rpc_error(
                codes::GRAPH_UNAVAILABLE,
                format!("the project graph is {}", self.graph_state_name()),
            )
        })
    }

    fn graph_node_id(&self, graph: &ProjectGraph, id: &str) -> Result<NodeId, ServerError> {
        id.parse::<NodeId>()
            .ok()
            .filter(|n| graph.node(*n).is_some())
            .ok_or_else(|| rpc_error(codes::INVALID_PARAMS, format!("unknown node {id:?}")))
    }

    pub(super) fn graph_node(&self, params: GraphNodeParams) -> Result<Value, ServerError> {
        let graph = self.built_graph()?;
        let id = self.graph_node_id(graph, &params.id)?;
        let mut out = self.graph_header();
        if let Some(node) = graph.node(id) {
            out["node"] = self.node_json(node);
        }
        out["outgoing"] = graph
            .outgoing(id)
            .iter()
            .map(|e| self.edge_json(e))
            .collect::<Vec<_>>()
            .into();
        out["incoming"] = graph
            .incoming(id)
            .iter()
            .map(|e| self.edge_json(e))
            .collect::<Vec<_>>()
            .into();
        Ok(out)
    }

    pub(super) fn graph_query(&self, params: GraphQueryParams) -> Result<Value, ServerError> {
        let graph = self.built_graph()?;
        let limit = params
            .max_results
            .unwrap_or(DEFAULT_QUERY_RESULTS)
            .min(MAX_QUERY_RESULTS);
        if params.relation == "find" {
            // `id` is a name (exact) or `path:<relative path>`.
            let mut found: Vec<&GraphNode> = match params.id.strip_prefix("path:") {
                Some(path) => graph.nodes_in_file(Path::new(path)),
                None => graph.nodes().filter(|n| *n.name == *params.id).collect(),
            };
            found.sort_by_key(|n| n.id);
            let truncated = found.len() > limit;
            let mut out = self.graph_header();
            out["relation"] = json!("find");
            out["results"] = found
                .into_iter()
                .take(limit)
                .map(|n| self.node_json(n))
                .collect::<Vec<_>>()
                .into();
            out["truncated"] = json!(truncated);
            return Ok(out);
        }
        let id = self.graph_node_id(graph, &params.id)?;
        let (kind, outgoing) = match params.relation.as_str() {
            "children" => (EdgeKind::Contains, true),
            "parents" => (EdgeKind::Contains, false),
            "defines" => (EdgeKind::Defines, true),
            "definedIn" => (EdgeKind::Defines, false),
            "imports" => (EdgeKind::Imports, true),
            "importedBy" => (EdgeKind::Imports, false),
            "tests" => (EdgeKind::Tests, true),
            "testsOf" => (EdgeKind::Tests, false),
            "dependsOn" => (EdgeKind::DependsOn, true),
            "dependents" => (EdgeKind::DependsOn, false),
            other => {
                return Err(rpc_error(
                    codes::INVALID_PARAMS,
                    format!("unknown relation {other:?}"),
                ));
            }
        };
        let edges: Vec<GraphEdge> = if outgoing {
            graph.outgoing(id)
        } else {
            graph.incoming(id)
        }
        .into_iter()
        .filter(|e| e.kind == kind)
        .collect();
        let truncated = edges.len() > limit;
        let results: Vec<Value> = edges
            .iter()
            .take(limit)
            .filter_map(|e| {
                let other = if outgoing { e.to } else { e.from };
                let mut v = self.node_json(graph.node(other)?);
                v["edge"] = self.edge_json(e);
                Some(v)
            })
            .collect();
        let mut out = self.graph_header();
        out["relation"] = json!(params.relation);
        out["id"] = json!(params.id);
        out["results"] = results.into();
        out["truncated"] = json!(truncated);
        Ok(out)
    }

    pub(super) fn graph_metrics(&self) -> Value {
        json!({
            "state": self.graph_state_name(),
            "generation": self.graph().map_or(0, ProjectGraph::generation),
            "builds": self.graph.builds,
            "failures": self.graph.failures,
            "lastBuildMs": self.graph.build_ms,
            "inputFiles": self.graph.input_files,
            "cache": self.graph_cache_stats(),
        })
    }
}
