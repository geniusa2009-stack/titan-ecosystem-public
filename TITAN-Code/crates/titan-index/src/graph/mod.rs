//! TITAN Project Twin V1: a version-aware graph of a Rust workspace.
//!
//! ## Identities
//! Every node has an opaque, stable [`NodeId`] assigned from its semantic [`NodeKey`]
//! (package name; module path inside a crate target; symbol name + kind + container +
//! module; test name + module; an opaque file identity). Ids survive rebuilds, line shifts
//! and moves of a module between `x.rs` and `x/mod.rs`. A file keeps its identity across a
//! rename the caller reports ([`ProjectGraph::rename_file`]); a symbol keeps it across a
//! rename the caller reports ([`ProjectGraph::rename_symbol`], used by rename refactorings).
//! Renames nobody reported produce a new identity (the old one disappears) — never a guess.
//! Display paths and names are attributes, not identities.
//!
//! ## Relationships and provenance
//! | Edge | Evidence (`source` / `kind`) |
//! |---|---|
//! | workspace **contains** package | `cargoManifest` / `package` |
//! | package **contains** module (crate roots) | `cargoManifest` / `cargoTarget` |
//! | module **contains** module | `tree-sitter` / `modDeclaration` or `inlineModule` |
//! | file **defines** module | `tree-sitter` / `moduleFile` |
//! | module **defines** function / type / symbol | `tree-sitter` / `syntaxDefinition` |
//! | module **contains** test | `tree-sitter` / `testAttribute` |
//! | test **tests** module/package | `tree-sitter` / `cfgTestModule`, `integrationTestTarget`, `testInModule` |
//! | module **imports** module / dependency | `tree-sitter` / `useDeclaration` |
//! | package **dependsOn** package / dependency | `cargoManifest` / `dependency` |
//!
//! Every edge records the file, line, content version ([`SourceVersion`]) and graph
//! generation it was derived from, and its analysis type (`structural` or `syntax`).
//! References and calls are **not** in this graph: they come from the language server per
//! query, with their own provenance (see the server's impact analysis). Nothing here is
//! inferred from name matching.
//!
//! ## Updates
//! Facts are computed per file version and cached by the caller. [`build_graph`] assembles
//! a graph from facts without parsing. [`ProjectGraph::update_file`] applies a new version
//! of one file in place when its structure signature (module declarations and `use`
//! paths) is unchanged — the common case of editing function bodies — and asks for a
//! rebuild otherwise. The randomized test below checks that incremental updates and full
//! rebuilds always produce the same graph.

mod facts;

use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use std::sync::Arc;

pub use facts::{FactSymbol, FileFacts, ModDecl, UseDecl, collect_facts, facts_from_tree};

use crate::{LineCol, SourceVersion, SymbolKind};

pub const LIMITATIONS: &[&str] = &[
    "syntax and manifest evidence only: macro-generated items and modules are not seen",
    "#[path] attributes on modules are not followed",
    "cfg attributes are not evaluated; all items are included",
    "imports point at the deepest module (or dependency) a use path names; re-exports are not followed",
    "references and calls are not stored; they come from the language server per query",
    "a rename that was not reported to the graph produces a new identity",
];

pub type NodeId = u64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum NodeKind {
    Workspace,
    Package,
    Dependency,
    Module,
    File,
    Function,
    Type,
    Symbol,
    Test,
}

impl NodeKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Workspace => "workspace",
            Self::Package => "package",
            Self::Dependency => "dependency",
            Self::Module => "module",
            Self::File => "file",
            Self::Function => "function",
            Self::Type => "type",
            Self::Symbol => "symbol",
            Self::Test => "test",
        }
    }

    pub const ALL: [NodeKind; 9] = [
        Self::Workspace,
        Self::Package,
        Self::Dependency,
        Self::Module,
        Self::File,
        Self::Function,
        Self::Type,
        Self::Symbol,
        Self::Test,
    ];

    fn of_symbol(kind: SymbolKind, is_test: bool) -> Self {
        match kind {
            _ if is_test => Self::Test,
            SymbolKind::Function | SymbolKind::Method => Self::Function,
            SymbolKind::Struct
            | SymbolKind::Enum
            | SymbolKind::Union
            | SymbolKind::Trait
            | SymbolKind::TypeAlias => Self::Type,
            _ => Self::Symbol,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum EdgeKind {
    Contains,
    Defines,
    Imports,
    Tests,
    DependsOn,
}

impl EdgeKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Contains => "contains",
            Self::Defines => "defines",
            Self::Imports => "imports",
            Self::Tests => "tests",
            Self::DependsOn => "dependsOn",
        }
    }

    pub const ALL: [EdgeKind; 5] = [
        Self::Contains,
        Self::Defines,
        Self::Imports,
        Self::Tests,
        Self::DependsOn,
    ];
}

/// The semantic identity of a node.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum NodeKey {
    Workspace,
    Package(String),
    Dependency(String),
    Module {
        package: String,
        /// `lib`, `bin:<name>`, `test:<name>`.
        target: String,
        path: Vec<String>,
    },
    /// Opaque file identity (stable across reported renames).
    File(u64),
    Item {
        /// Shared with every item of the module (one allocation per module).
        module: Arc<NodeKey>,
        container: Vec<String>,
        /// Shared with the node and the cached facts.
        name: Arc<str>,
        kind: SymbolKind,
        /// 0 for the first definition with this key, 1.. for duplicates (cfg variants).
        ordinal: usize,
    },
}

impl NodeKey {
    /// Human-readable rendering (not an identity: names can repeat across workspaces).
    pub fn display(&self) -> String {
        match self {
            Self::Workspace => "workspace".to_string(),
            Self::Package(name) => format!("package {name}"),
            Self::Dependency(name) => format!("dependency {name}"),
            Self::Module {
                package,
                target,
                path,
            } => {
                let mut s = format!("{package}[{target}]::crate");
                for p in path {
                    s.push_str("::");
                    s.push_str(p);
                }
                s
            }
            Self::File(id) => format!("file#{id}"),
            Self::Item {
                module,
                container,
                name,
                ordinal,
                ..
            } => {
                let mut s = module.display();
                for c in container {
                    s.push_str("::");
                    s.push_str(c);
                }
                s.push_str("::");
                s.push_str(name);
                if *ordinal > 0 {
                    s.push_str(&format!("#{ordinal}"));
                }
                s
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GraphNode {
    pub id: NodeId,
    pub key: NodeKey,
    pub kind: NodeKind,
    pub name: Arc<str>,
    /// Current workspace-relative file (interned: one allocation per file).
    pub path: Option<Arc<Path>>,
    /// Name range (lines by the buffer's convention).
    pub start: Option<LineCol>,
    pub end: Option<LineCol>,
    /// Symbol kind, crate target kind, `cfgTest` for modules, version requirement for deps.
    pub detail: Option<Cow<'static, str>>,
    pub version: Option<SourceVersion>,
}

/// Where an edge comes from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Provenance {
    /// `cargoManifest`, `tree-sitter`.
    pub source: &'static str,
    /// `structural` (manifests, module tree, imports) or `syntax` (definitions, tests).
    pub analysis: &'static str,
    pub kind: &'static str,
    pub path: Option<Arc<Path>>,
    pub line: Option<usize>,
    pub version: Option<SourceVersion>,
    /// Graph generation in which the edge was (re)derived.
    pub generation: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GraphEdge {
    pub from: NodeId,
    pub to: NodeId,
    pub kind: EdgeKind,
    pub provenance: Provenance,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GraphStats {
    pub files_parsed: usize,
    pub files_with_syntax_errors: usize,
    pub unreachable_files: usize,
    pub unresolved_modules: usize,
    pub unresolved_imports: usize,
    pub external_imports: usize,
    pub manifest_errors: usize,
    pub full_builds: u64,
    pub local_updates: u64,
    pub rebuilds_requested: u64,
}

/// Inputs of a build: manifests (text), every Rust file with its version and facts.
#[derive(Clone, Debug, Default)]
pub struct GraphInput {
    pub root_name: String,
    pub manifests: BTreeMap<PathBuf, String>,
    pub files: BTreeMap<PathBuf, (SourceVersion, Arc<FileFacts>)>,
}

/// Why [`ProjectGraph::update_file`] could not update in place.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NeedsRebuild {
    /// Module declarations or `use` paths changed.
    StructureChanged,
    /// The file is not part of any crate's module tree (yet).
    UnknownFile,
}

#[derive(Clone, Debug)]
struct ModuleInfo {
    package: String,
    package_node: NodeId,
    target: String,
    path: Vec<String>,
    file: PathBuf,
    inline_path: Vec<String>,
    dir: PathBuf,
    parent: Option<NodeId>,
    crate_root: NodeId,
    children: BTreeMap<String, NodeId>,
    cfg_test: bool,
    integration_test: bool,
}

/// Local contribution of one file: definitions and tests (replaced by `update_file`).
#[derive(Clone, Debug, Default)]
struct Contribution {
    nodes: Vec<NodeId>,
    edges: Vec<(NodeId, NodeId, EdgeKind)>,
    signature: u64,
    version: Option<SourceVersion>,
}

#[derive(Clone, Debug, Default)]
pub struct ProjectGraph {
    nodes: HashMap<NodeId, GraphNode>,
    edges: HashMap<(NodeId, NodeId, EdgeKind), Provenance>,
    outgoing: HashMap<NodeId, BTreeSet<(NodeId, EdgeKind)>>,
    incoming: HashMap<NodeId, BTreeSet<(NodeId, EdgeKind)>>,
    /// Identity table: survives rebuilds so ids are stable.
    ids: HashMap<NodeKey, NodeId>,
    file_ids: HashMap<PathBuf, u64>,
    next_id: NodeId,
    next_file: u64,
    modules: HashMap<NodeId, ModuleInfo>,
    /// Modules defined in each file: (inline path, module node).
    file_modules: HashMap<PathBuf, Vec<(Vec<String>, NodeId)>>,
    contributions: HashMap<PathBuf, Contribution>,
    /// Interned paths and module keys (shared by nodes, keys and provenance).
    paths: HashMap<PathBuf, Arc<Path>>,
    module_keys: HashMap<NodeId, Arc<NodeKey>>,
    generation: u64,
    pub stats: GraphStats,
}

impl ProjectGraph {
    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn node(&self, id: NodeId) -> Option<&GraphNode> {
        self.nodes.get(&id)
    }

    pub fn id_of(&self, key: &NodeKey) -> Option<NodeId> {
        self.ids
            .get(key)
            .copied()
            .filter(|id| self.nodes.contains_key(id))
    }

    pub fn nodes(&self) -> impl Iterator<Item = &GraphNode> {
        self.nodes.values()
    }

    pub fn edge_count(&self, kind: EdgeKind) -> usize {
        self.edges.keys().filter(|(_, _, k)| *k == kind).count()
    }

    pub fn count(&self, kind: NodeKind) -> usize {
        self.nodes.values().filter(|n| n.kind == kind).count()
    }

    pub fn outgoing(&self, id: NodeId) -> Vec<GraphEdge> {
        self.adjacent(id, true)
    }

    pub fn incoming(&self, id: NodeId) -> Vec<GraphEdge> {
        self.adjacent(id, false)
    }

    fn adjacent(&self, id: NodeId, outgoing: bool) -> Vec<GraphEdge> {
        let map = if outgoing {
            &self.outgoing
        } else {
            &self.incoming
        };
        map.get(&id)
            .into_iter()
            .flatten()
            .filter_map(|&(other, kind)| {
                let key = if outgoing {
                    (id, other, kind)
                } else {
                    (other, id, kind)
                };
                self.edges.get(&key).map(|p| GraphEdge {
                    from: key.0,
                    to: key.1,
                    kind,
                    provenance: p.clone(),
                })
            })
            .collect()
    }

    /// Nodes defined in `path` (modules, definitions, tests), most specific first.
    pub fn nodes_in_file(&self, path: &Path) -> Vec<&GraphNode> {
        let mut out: Vec<&GraphNode> = self
            .nodes
            .values()
            .filter(|n| n.path.as_deref() == Some(path) && n.kind != NodeKind::File)
            .collect();
        out.sort_by_key(|n| (n.kind == NodeKind::Module, n.start.map(|s| s.line)));
        out
    }

    /// The file node of `path`.
    pub fn file_node(&self, path: &Path) -> Option<&GraphNode> {
        let id = self.file_ids.get(path)?;
        self.id_of(&NodeKey::File(*id))
            .and_then(|n| self.nodes.get(&n))
    }

    /// The innermost module containing `line` of `path`.
    pub fn module_at(&self, path: &Path, line: usize) -> Option<NodeId> {
        let modules = self.file_modules.get(path)?;
        // Inline modules: choose the deepest whose first line is <= line (a heuristic for
        // nesting only between modules of the same file; module ranges are not stored).
        modules
            .iter()
            .filter(|(inline, id)| {
                inline.is_empty()
                    || self
                        .nodes
                        .get(id)
                        .and_then(|n| n.start)
                        .is_some_and(|s| s.line <= line)
            })
            .max_by_key(|(inline, _)| inline.len())
            .map(|(_, id)| *id)
    }

    /// Approximate heap bytes held by the graph (an estimate for memory reports).
    pub fn estimated_bytes(&self) -> usize {
        let node = std::mem::size_of::<GraphNode>() + std::mem::size_of::<NodeKey>();
        let nodes: usize = self.nodes.values().map(|n| node + n.name.len()).sum();
        let edges = self.edges.len()
            * (std::mem::size_of::<(NodeId, NodeId, EdgeKind)>()
                + std::mem::size_of::<Provenance>())
            * 3;
        let paths: usize = self.paths.keys().map(|p| 2 * p.as_os_str().len()).sum();
        nodes + edges + self.ids.len() * node + paths
    }

    /// An empty graph that carries only this graph's identity table and counters, to be
    /// passed to [`build_graph`] on another thread while this graph keeps serving queries.
    pub fn identities_only(&self) -> ProjectGraph {
        ProjectGraph {
            ids: self.ids.clone(),
            file_ids: self.file_ids.clone(),
            next_id: self.next_id,
            next_file: self.next_file,
            generation: self.generation,
            stats: self.stats.clone(),
            ..ProjectGraph::default()
        }
    }

    /// Report a file rename (from the file watcher) so the file keeps its identity.
    pub fn rename_file(&mut self, from: &Path, to: &Path) {
        if let Some(id) = self.file_ids.remove(from) {
            self.file_ids.insert(to.to_path_buf(), id);
        }
    }

    /// Report a symbol rename (a rename refactoring) so the symbol keeps its identity:
    /// the identity of `old` is moved to the same key with `new_name`.
    pub fn rename_symbol(&mut self, old: &NodeKey, new_name: &str) -> bool {
        let NodeKey::Item {
            module,
            container,
            kind,
            ordinal,
            ..
        } = old
        else {
            return false;
        };
        let Some(id) = self.ids.remove(old) else {
            return false;
        };
        let new_key = NodeKey::Item {
            module: module.clone(),
            container: container.clone(),
            name: Arc::from(new_name),
            kind: *kind,
            ordinal: *ordinal,
        };
        self.ids.insert(new_key, id);
        true
    }

    fn intern(&mut self, path: &Path) -> Arc<Path> {
        if let Some(p) = self.paths.get(path) {
            return Arc::clone(p);
        }
        let arc: Arc<Path> = Arc::from(path);
        self.paths.insert(path.to_path_buf(), Arc::clone(&arc));
        arc
    }

    fn identity(&mut self, key: &NodeKey) -> NodeId {
        if let Some(id) = self.ids.get(key) {
            return *id;
        }
        self.next_id += 1;
        self.ids.insert(key.clone(), self.next_id);
        self.next_id
    }

    fn file_identity(&mut self, path: &Path) -> u64 {
        if let Some(id) = self.file_ids.get(path) {
            return *id;
        }
        self.next_file += 1;
        self.file_ids.insert(path.to_path_buf(), self.next_file);
        self.next_file
    }

    fn add_node(&mut self, mut node: GraphNode) -> NodeId {
        let id = self.identity(&node.key);
        node.id = id;
        if node.kind == NodeKind::Module {
            self.module_keys.insert(id, Arc::new(node.key.clone()));
        }
        self.nodes.insert(id, node);
        id
    }

    #[allow(clippy::too_many_arguments)]
    fn add_edge(
        &mut self,
        from: NodeId,
        to: NodeId,
        kind: EdgeKind,
        source: &'static str,
        analysis: &'static str,
        evidence: &'static str,
        path: &Path,
        line: Option<usize>,
        version: Option<SourceVersion>,
    ) -> bool {
        if from == to || !self.nodes.contains_key(&from) || !self.nodes.contains_key(&to) {
            return false;
        }
        if self.edges.contains_key(&(from, to, kind)) {
            return false;
        }
        let interned = self.intern(path);
        self.edges.insert(
            (from, to, kind),
            Provenance {
                source,
                analysis,
                kind: evidence,
                path: Some(interned),
                line,
                version,
                generation: self.generation,
            },
        );
        self.outgoing.entry(from).or_default().insert((to, kind));
        self.incoming.entry(to).or_default().insert((from, kind));
        true
    }

    fn remove_edge(&mut self, key: (NodeId, NodeId, EdgeKind)) {
        if self.edges.remove(&key).is_some() {
            if let Some(set) = self.outgoing.get_mut(&key.0) {
                set.remove(&(key.1, key.2));
            }
            if let Some(set) = self.incoming.get_mut(&key.1) {
                set.remove(&(key.0, key.2));
            }
        }
    }

    fn remove_node(&mut self, id: NodeId) {
        let edges: Vec<(NodeId, NodeId, EdgeKind)> = self
            .outgoing
            .get(&id)
            .into_iter()
            .flatten()
            .map(|&(to, k)| (id, to, k))
            .chain(
                self.incoming
                    .get(&id)
                    .into_iter()
                    .flatten()
                    .map(|&(from, k)| (from, id, k)),
            )
            .collect();
        for e in edges {
            self.remove_edge(e);
        }
        self.outgoing.remove(&id);
        self.incoming.remove(&id);
        self.nodes.remove(&id);
    }

    /// Add the definitions and tests of one file (its local contribution).
    fn add_local(&mut self, path: &Path, version: SourceVersion, facts: &FileFacts) {
        let mut contribution = Contribution {
            signature: facts.structure_signature(),
            version: Some(version),
            ..Contribution::default()
        };
        let modules: HashMap<Vec<String>, NodeId> = self
            .file_modules
            .get(path)
            .map(|m| m.iter().cloned().collect())
            .unwrap_or_default();
        let mut seen: HashMap<NodeKey, usize> = HashMap::new();
        for symbol in &facts.symbols {
            let Some(&module) = modules.get(&symbol.module) else {
                continue;
            };
            let Some(module_key) = self.module_keys.get(&module).cloned() else {
                continue;
            };
            let base = NodeKey::Item {
                module: module_key,
                container: symbol.container.clone(),
                name: Arc::clone(&symbol.name),
                kind: symbol.kind,
                ordinal: 0,
            };
            let ordinal = {
                let n = seen.entry(base.clone()).or_insert(0);
                *n += 1;
                *n - 1
            };
            let key = match base {
                NodeKey::Item {
                    module,
                    container,
                    name,
                    kind,
                    ..
                } => NodeKey::Item {
                    module,
                    container,
                    name,
                    kind,
                    ordinal,
                },
                other => other,
            };
            let node_kind = NodeKind::of_symbol(symbol.kind, symbol.is_test);
            let interned = self.intern(path);
            let id = self.add_node(GraphNode {
                id: 0,
                key,
                kind: node_kind,
                name: Arc::clone(&symbol.name),
                path: Some(interned),
                start: Some(symbol.start),
                end: Some(symbol.end),
                detail: Some(Cow::Borrowed(symbol.kind.as_str())),
                version: Some(version),
            });
            contribution.nodes.push(id);
            let line = Some(symbol.start.line);
            if symbol.is_test {
                if self.add_edge(
                    module,
                    id,
                    EdgeKind::Contains,
                    "tree-sitter",
                    "syntax",
                    "testAttribute",
                    path,
                    line,
                    Some(version),
                ) {
                    contribution.edges.push((module, id, EdgeKind::Contains));
                }
                let (target, evidence) = self.test_target(module);
                if self.add_edge(
                    id,
                    target,
                    EdgeKind::Tests,
                    "tree-sitter",
                    "syntax",
                    evidence,
                    path,
                    line,
                    Some(version),
                ) {
                    contribution.edges.push((id, target, EdgeKind::Tests));
                }
            } else if self.add_edge(
                module,
                id,
                EdgeKind::Defines,
                "tree-sitter",
                "syntax",
                "syntaxDefinition",
                path,
                line,
                Some(version),
            ) {
                contribution.edges.push((module, id, EdgeKind::Defines));
            }
        }
        self.contributions.insert(path.to_path_buf(), contribution);
    }

    fn remove_local(&mut self, path: &Path) {
        if let Some(c) = self.contributions.remove(path) {
            for e in c.edges {
                self.remove_edge(e);
            }
            for n in c.nodes {
                self.remove_node(n);
            }
        }
    }

    fn test_target(&self, module: NodeId) -> (NodeId, &'static str) {
        let mut at = Some(module);
        while let Some(id) = at {
            let Some(info) = self.modules.get(&id) else {
                break;
            };
            if info.cfg_test {
                if let Some(parent) = info.parent {
                    return (parent, "cfgTestModule");
                }
            }
            at = info.parent;
        }
        match self.modules.get(&module) {
            Some(info) if info.integration_test => (info.package_node, "integrationTestTarget"),
            _ => (module, "testInModule"),
        }
    }

    /// Apply a new version of `path` in place if only local definitions changed.
    pub fn update_file(
        &mut self,
        path: &Path,
        version: SourceVersion,
        facts: &FileFacts,
    ) -> Result<(), NeedsRebuild> {
        let Some(old) = self.contributions.get(path) else {
            self.stats.rebuilds_requested += 1;
            return Err(NeedsRebuild::UnknownFile);
        };
        if old.signature != facts.structure_signature() {
            self.stats.rebuilds_requested += 1;
            return Err(NeedsRebuild::StructureChanged);
        }
        self.generation += 1;
        self.remove_local(path);
        self.add_local(path, version, facts);
        if let Some(file) = self.file_node(path).map(|n| n.id) {
            if let Some(node) = self.nodes.get_mut(&file) {
                node.version = Some(version);
            }
        }
        self.stats.local_updates += 1;
        Ok(())
    }

    /// The version each file contribution was derived from.
    pub fn file_version(&self, path: &Path) -> Option<SourceVersion> {
        self.contributions.get(path).and_then(|c| c.version)
    }

    /// Canonical form for comparisons: node keys + attributes and edges by key.
    #[doc(hidden)]
    pub fn canonical(&self) -> (BTreeSet<String>, BTreeSet<String>) {
        let key = |id: &NodeId| {
            self.nodes
                .get(id)
                .map(|n| format!("{:?}", n.key))
                .unwrap_or_default()
        };
        let nodes = self
            .nodes
            .values()
            .map(|n| {
                format!(
                    "{:?}|{:?}|{}|{:?}|{:?}|{:?}",
                    n.key,
                    n.kind,
                    n.name,
                    n.path,
                    n.start.map(|s| s.line),
                    n.detail
                )
            })
            .collect();
        let edges = self
            .edges
            .iter()
            .map(|((from, to, kind), p)| {
                format!(
                    "{}|{}|{:?}|{}|{:?}|{:?}",
                    key(from),
                    key(to),
                    kind,
                    p.kind,
                    p.path,
                    p.line
                )
            })
            .collect();
        (nodes, edges)
    }
}

// ----- building ------------------------------------------------------------------------

struct Target {
    kind: &'static str,
    name: String,
    root: PathBuf,
}

fn targets(dir: &Path, manifest: &toml::Table, input: &GraphInput) -> Vec<Target> {
    let package = manifest
        .get("package")
        .and_then(|p| p.get("name"))
        .and_then(|n| n.as_str())
        .unwrap_or_default()
        .to_string();
    let exists = |p: &Path| input.files.contains_key(p);
    let mut out = Vec::new();
    let lib_path = manifest
        .get("lib")
        .and_then(|l| l.get("path"))
        .and_then(|p| p.as_str())
        .map(|p| dir.join(p))
        .unwrap_or_else(|| dir.join("src/lib.rs"));
    if exists(&lib_path) {
        let name = manifest
            .get("lib")
            .and_then(|l| l.get("name"))
            .and_then(|n| n.as_str())
            .map_or_else(|| package.replace('-', "_"), str::to_string);
        out.push(Target {
            kind: "lib",
            name,
            root: lib_path,
        });
    }
    let mut explicit = BTreeSet::new();
    for (key, kind) in [("bin", "bin"), ("test", "test")] {
        for entry in manifest
            .get(key)
            .and_then(|b| b.as_array())
            .into_iter()
            .flatten()
        {
            if let Some(path) = entry.get("path").and_then(|p| p.as_str()) {
                let root = dir.join(path);
                explicit.insert(root.clone());
                if exists(&root) {
                    let name = entry
                        .get("name")
                        .and_then(|n| n.as_str())
                        .unwrap_or_default()
                        .to_string();
                    out.push(Target { kind, name, root });
                }
            }
        }
    }
    let main = dir.join("src/main.rs");
    if exists(&main) && !explicit.contains(&main) {
        out.push(Target {
            kind: "bin",
            name: package.clone(),
            root: main,
        });
    }
    for rel in input.files.range(dir.to_path_buf()..).map(|(p, _)| p) {
        if !rel.starts_with(dir) {
            break;
        }
        let Ok(inner) = rel.strip_prefix(dir) else {
            continue;
        };
        let parts: Vec<String> = inner
            .components()
            .map(|c| c.as_os_str().to_string_lossy().into_owned())
            .collect();
        let stem = rel
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let (kind, name) = match parts
            .iter()
            .map(String::as_str)
            .collect::<Vec<_>>()
            .as_slice()
        {
            ["src", "bin", _] => ("bin", stem),
            ["src", "bin", dir_name, "main.rs"] => ("bin", (*dir_name).to_string()),
            ["tests", _] => ("test", stem),
            _ => continue,
        };
        if !explicit.contains(rel) {
            out.push(Target {
                kind,
                name,
                root: rel.clone(),
            });
        }
    }
    out
}

fn dependencies(manifest: &toml::Table) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for section in ["dependencies", "dev-dependencies", "build-dependencies"] {
        if let Some(table) = manifest.get(section).and_then(|t| t.as_table()) {
            for (name, spec) in table {
                let requirement = spec
                    .as_str()
                    .map(str::to_string)
                    .or_else(|| {
                        spec.get("version")
                            .and_then(|v| v.as_str())
                            .map(str::to_string)
                    })
                    .or_else(|| {
                        spec.get("path")
                            .and_then(|v| v.as_str())
                            .map(|p| format!("path {p}"))
                    })
                    .unwrap_or_else(|| "workspace".to_string());
                out.push((name.clone(), format!("{section}: {requirement}")));
            }
        }
    }
    out
}

/// A package node, its manifest and its declared dependencies (name, requirement).
type PackageDeps = (NodeId, PathBuf, Vec<(String, String)>);

enum Resolution {
    Module(NodeId),
    Dependency(String),
    External,
    Unresolved,
}

/// Build (or rebuild) the graph from `input`, keeping the identity table of `previous` so
/// ids stay stable. Never parses: facts come with the input.
pub fn build_graph(input: &GraphInput, previous: Option<ProjectGraph>) -> ProjectGraph {
    let mut g = ProjectGraph::default();
    if let Some(prev) = previous {
        g.ids = prev.ids;
        g.file_ids = prev.file_ids;
        g.next_id = prev.next_id;
        g.next_file = prev.next_file;
        g.generation = prev.generation;
        g.stats.full_builds = prev.stats.full_builds;
        g.stats.local_updates = prev.stats.local_updates;
        g.stats.rebuilds_requested = prev.stats.rebuilds_requested;
    }
    g.generation += 1;
    g.stats.full_builds += 1;
    let workspace = g.add_node(GraphNode {
        id: 0,
        key: NodeKey::Workspace,
        kind: NodeKind::Workspace,
        name: Arc::from(input.root_name.as_str()),
        path: None,
        start: None,
        end: None,
        detail: None,
        version: None,
    });
    for (path, (version, facts)) in &input.files {
        let fid = g.file_identity(path);
        let interned = g.intern(path);
        g.add_node(GraphNode {
            id: 0,
            key: NodeKey::File(fid),
            kind: NodeKind::File,
            name: Arc::from(path.to_string_lossy().replace('\\', "/").as_str()),
            path: Some(interned),
            start: None,
            end: None,
            detail: None,
            version: Some(*version),
        });
        if facts.has_errors {
            g.stats.files_with_syntax_errors += 1;
        }
    }
    g.stats.files_parsed = input.files.len();

    let mut lib_roots: HashMap<String, NodeId> = HashMap::new();
    let mut queue: std::collections::VecDeque<NodeId> = std::collections::VecDeque::new();
    let mut package_deps: Vec<PackageDeps> = Vec::new();
    let mut package_names: HashMap<String, NodeId> = HashMap::new();
    for (manifest_path, text) in &input.manifests {
        let Ok(manifest) = text.parse::<toml::Table>() else {
            g.stats.manifest_errors += 1;
            continue;
        };
        let Some(name) = manifest
            .get("package")
            .and_then(|p| p.get("name"))
            .and_then(|n| n.as_str())
        else {
            continue;
        };
        let dir = manifest_path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_default();
        let manifest_interned = g.intern(manifest_path);
        let package = g.add_node(GraphNode {
            id: 0,
            key: NodeKey::Package(name.to_string()),
            kind: NodeKind::Package,
            name: Arc::from(name),
            path: Some(manifest_interned),
            start: None,
            end: None,
            detail: None,
            version: None,
        });
        package_names.insert(name.to_string(), package);
        g.add_edge(
            workspace,
            package,
            EdgeKind::Contains,
            "cargoManifest",
            "structural",
            "package",
            manifest_path,
            None,
            None,
        );
        package_deps.push((package, manifest_path.clone(), dependencies(&manifest)));
        for target in targets(&dir, &manifest, input) {
            let target_name = match target.kind {
                "lib" => "lib".to_string(),
                kind => format!("{kind}:{}", target.name),
            };
            let key = NodeKey::Module {
                package: name.to_string(),
                target: target_name.clone(),
                path: Vec::new(),
            };
            let root_interned = g.intern(&target.root);
            let id = g.add_node(GraphNode {
                id: 0,
                key,
                kind: NodeKind::Module,
                name: Arc::from(target.name.as_str()),
                path: Some(root_interned),
                start: None,
                end: None,
                detail: Some(Cow::Borrowed(target.kind)),
                version: None,
            });
            g.add_edge(
                package,
                id,
                EdgeKind::Contains,
                "cargoManifest",
                "structural",
                "cargoTarget",
                manifest_path,
                None,
                None,
            );
            if target.kind == "lib" {
                lib_roots.insert(target.name.clone(), id);
            }
            g.modules.insert(
                id,
                ModuleInfo {
                    package: name.to_string(),
                    package_node: package,
                    target: target_name,
                    path: Vec::new(),
                    file: target.root.clone(),
                    inline_path: Vec::new(),
                    dir: target
                        .root
                        .parent()
                        .map(Path::to_path_buf)
                        .unwrap_or_default(),
                    parent: None,
                    crate_root: id,
                    children: BTreeMap::new(),
                    cfg_test: false,
                    integration_test: target.kind == "test",
                },
            );
            queue.push_back(id);
        }
    }

    // Dependencies: workspace packages link to each other; others become dependency nodes.
    for (package, manifest_path, deps) in package_deps {
        for (dep, requirement) in deps {
            let target = match package_names.get(&dep) {
                Some(&p) => p,
                None => g.add_node(GraphNode {
                    id: 0,
                    key: NodeKey::Dependency(dep.clone()),
                    kind: NodeKind::Dependency,
                    name: Arc::from(dep.as_str()),
                    path: None,
                    start: None,
                    end: None,
                    detail: Some(Cow::Owned(requirement)),
                    version: None,
                }),
            };
            g.add_edge(
                package,
                target,
                EdgeKind::DependsOn,
                "cargoManifest",
                "structural",
                "dependency",
                &manifest_path,
                None,
                None,
            );
        }
    }

    // Module trees, breadth first.
    let mut visited: BTreeSet<NodeId> = BTreeSet::new();
    while let Some(mod_id) = queue.pop_front() {
        if !visited.insert(mod_id) {
            continue;
        }
        let Some(info) = g.modules.get(&mod_id).cloned() else {
            continue;
        };
        if !info.inline_path.is_empty() {
            continue;
        }
        let file = info.file.clone();
        let Some((version, facts)) = input.files.get(&file) else {
            continue;
        };
        let version = *version;
        if let Some(file_node) = g.file_node(&file).map(|n| n.id) {
            g.add_edge(
                info.package_node,
                file_node,
                EdgeKind::Contains,
                "tree-sitter",
                "structural",
                "moduleTree",
                &file,
                None,
                Some(version),
            );
            g.add_edge(
                file_node,
                mod_id,
                EdgeKind::Defines,
                "tree-sitter",
                "structural",
                "moduleFile",
                &file,
                None,
                Some(version),
            );
        }
        g.file_modules
            .entry(file.clone())
            .or_default()
            .push((Vec::new(), mod_id));
        let mut decls = facts.mods.clone();
        decls.sort_by_key(|d| d.parent.len());
        for decl in decls {
            let parent_id = g
                .file_modules
                .get(&file)
                .and_then(|m| m.iter().find(|(p, _)| *p == decl.parent).map(|(_, id)| *id));
            let Some(parent_id) = parent_id else { continue };
            let Some(parent) = g.modules.get(&parent_id).cloned() else {
                continue;
            };
            let mut module_path = parent.path.clone();
            module_path.push(decl.name.clone());
            let (child_file, child_inline, child_dir) = if decl.inline {
                let mut inline = decl.parent.clone();
                inline.push(decl.name.clone());
                (file.clone(), inline, parent.dir.join(&decl.name))
            } else {
                let flat = parent.dir.join(format!("{}.rs", decl.name));
                let nested = parent.dir.join(&decl.name).join("mod.rs");
                let Some(found) = [flat, nested]
                    .into_iter()
                    .find(|p| input.files.contains_key(p))
                else {
                    g.stats.unresolved_modules += 1;
                    continue;
                };
                (found, Vec::new(), parent.dir.join(&decl.name))
            };
            let key = NodeKey::Module {
                package: parent.package.clone(),
                target: parent.target.clone(),
                path: module_path.clone(),
            };
            let child_interned = g.intern(&child_file);
            let child = g.add_node(GraphNode {
                id: 0,
                key,
                kind: NodeKind::Module,
                name: Arc::from(decl.name.as_str()),
                path: Some(child_interned),
                start: decl.inline.then_some(LineCol {
                    line: decl.line,
                    byte_column: 0,
                    utf16_column: 0,
                }),
                end: None,
                detail: decl.cfg_test.then_some(Cow::Borrowed("cfgTest")),
                version: None,
            });
            g.modules.insert(
                child,
                ModuleInfo {
                    package: parent.package.clone(),
                    package_node: parent.package_node,
                    target: parent.target.clone(),
                    path: module_path,
                    file: child_file.clone(),
                    inline_path: child_inline.clone(),
                    dir: child_dir,
                    parent: Some(parent_id),
                    crate_root: parent.crate_root,
                    children: BTreeMap::new(),
                    cfg_test: decl.cfg_test,
                    integration_test: parent.integration_test,
                },
            );
            if let Some(p) = g.modules.get_mut(&parent_id) {
                p.children.insert(decl.name.clone(), child);
            }
            let evidence = if decl.inline {
                "inlineModule"
            } else {
                "modDeclaration"
            };
            g.add_edge(
                parent_id,
                child,
                EdgeKind::Contains,
                "tree-sitter",
                "structural",
                evidence,
                &file,
                Some(decl.line),
                Some(version),
            );
            if decl.inline {
                g.file_modules
                    .entry(file.clone())
                    .or_default()
                    .push((child_inline, child));
                if let Some(file_node) = g.file_node(&file).map(|n| n.id) {
                    g.add_edge(
                        file_node,
                        child,
                        EdgeKind::Defines,
                        "tree-sitter",
                        "structural",
                        "moduleFile",
                        &file,
                        Some(decl.line),
                        Some(version),
                    );
                }
            } else {
                queue.push_back(child);
            }
        }
    }

    // Local contributions (definitions, tests) of every reachable file.
    let reachable: Vec<PathBuf> = g.file_modules.keys().cloned().collect();
    for path in &reachable {
        if let Some((version, facts)) = input.files.get(path) {
            g.add_local(path, *version, facts);
        }
    }
    g.stats.unreachable_files = input.files.len().saturating_sub(reachable.len());

    // Imports, once every module is known.
    let mut deps_by_crate: HashMap<String, NodeId> = HashMap::new();
    for n in g.nodes.values() {
        if n.kind == NodeKind::Dependency {
            deps_by_crate.insert(n.name.replace('-', "_"), n.id);
        }
    }
    for path in &reachable {
        let Some((version, facts)) = input.files.get(path) else {
            continue;
        };
        for decl in &facts.uses {
            let from = g
                .file_modules
                .get(path)
                .and_then(|m| m.iter().find(|(p, _)| *p == decl.module).map(|(_, id)| *id));
            let Some(from) = from else { continue };
            for use_path in &decl.paths {
                match resolve(use_path, from, &g.modules, &lib_roots, &deps_by_crate) {
                    Resolution::Module(to) => {
                        g.add_edge(
                            from,
                            to,
                            EdgeKind::Imports,
                            "tree-sitter",
                            "structural",
                            "useDeclaration",
                            path,
                            Some(decl.line),
                            Some(*version),
                        );
                    }
                    Resolution::Dependency(name) => {
                        if let Some(&to) = deps_by_crate.get(&name) {
                            g.add_edge(
                                from,
                                to,
                                EdgeKind::Imports,
                                "tree-sitter",
                                "structural",
                                "useDeclaration",
                                path,
                                Some(decl.line),
                                Some(*version),
                            );
                        }
                        g.stats.external_imports += 1;
                    }
                    Resolution::External => g.stats.external_imports += 1,
                    Resolution::Unresolved => g.stats.unresolved_imports += 1,
                }
            }
        }
    }
    g
}

fn resolve(
    path: &[String],
    current: NodeId,
    modules: &HashMap<NodeId, ModuleInfo>,
    lib_roots: &HashMap<String, NodeId>,
    deps: &HashMap<String, NodeId>,
) -> Resolution {
    let Some((first, rest)) = path.split_first() else {
        return Resolution::Unresolved;
    };
    let Some(here) = modules.get(&current) else {
        return Resolution::Unresolved;
    };
    let mut at = match first.as_str() {
        "crate" => here.crate_root,
        "self" => current,
        "super" => match here.parent {
            Some(parent) => parent,
            None => return Resolution::Unresolved,
        },
        name => {
            if let Some(child) = here.children.get(name) {
                *child
            } else if let Some(root) = lib_roots.get(name) {
                *root
            } else if deps.contains_key(name) {
                return Resolution::Dependency(name.to_string());
            } else {
                return Resolution::External;
            }
        }
    };
    for segment in rest {
        let Some(info) = modules.get(&at) else {
            return Resolution::Unresolved;
        };
        match segment.as_str() {
            "self" => {}
            "super" => match info.parent {
                Some(parent) => at = parent,
                None => return Resolution::Unresolved,
            },
            name => match info.children.get(name) {
                Some(child) => at = *child,
                None => break,
            },
        }
    }
    Resolution::Module(at)
}

#[cfg(test)]
mod tests;
