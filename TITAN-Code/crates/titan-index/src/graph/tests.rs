use super::facts::expand_use;
use super::*;
use crate::SyntaxParser;

fn facts(parser: &mut SyntaxParser, text: &str) -> Arc<FileFacts> {
    Arc::new(collect_facts(parser, text).expect("parse"))
}

fn input(files: &[(&str, &str)]) -> GraphInput {
    let mut parser = SyntaxParser::new().unwrap();
    let mut input = GraphInput {
        root_name: "ws".to_string(),
        ..GraphInput::default()
    };
    for (i, (path, text)) in files.iter().enumerate() {
        if path.ends_with("Cargo.toml") {
            input
                .manifests
                .insert(PathBuf::from(path), text.to_string());
        } else {
            input.files.insert(
                PathBuf::from(path),
                (
                    SourceVersion::Buffer { revision: i as u64 },
                    facts(&mut parser, text),
                ),
            );
        }
    }
    input
}

fn id(g: &ProjectGraph, key: NodeKey) -> NodeId {
    g.id_of(&key)
        .unwrap_or_else(|| panic!("no node {}", key.display()))
}

fn module(package: &str, target: &str, path: &[&str]) -> NodeKey {
    NodeKey::Module {
        package: package.to_string(),
        target: target.to_string(),
        path: path.iter().map(|s| s.to_string()).collect(),
    }
}

fn item(module: NodeKey, container: &[&str], name: &str, kind: SymbolKind) -> NodeKey {
    NodeKey::Item {
        module: Arc::new(module),
        container: container.iter().map(|s| s.to_string()).collect(),
        name: Arc::from(name),
        kind,
        ordinal: 0,
    }
}

fn has_edge(g: &ProjectGraph, from: NodeId, to: NodeId, kind: EdgeKind) -> bool {
    g.outgoing(from)
        .iter()
        .any(|e| e.to == to && e.kind == kind)
}

const WORKSPACE: &[(&str, &str)] = &[
    ("Cargo.toml", "[workspace]\nmembers = [\"core\", \"app\"]\n"),
    (
        "core/Cargo.toml",
        "[package]\nname = \"my-core\"\n\n[dependencies]\nserde = \"1\"\n",
    ),
    (
        "core/src/lib.rs",
        "pub mod text;\nmod util {\n    pub fn helper() {}\n}\npub use text::Buffer;\nuse serde::Serialize;\n\
         #[cfg(test)]\nmod tests {\n    use super::*;\n    #[test]\n    fn works() {}\n}\n",
    ),
    (
        "core/src/text.rs",
        "pub mod rope;\npub struct Buffer;\nimpl Buffer { pub fn new() -> Self { Buffer } }\n",
    ),
    (
        "core/src/text/rope.rs",
        "use crate::util::helper;\npub fn split() {}\n",
    ),
    (
        "core/tests/api.rs",
        "use my_core::text;\n#[test]\nfn integration() {}\n",
    ),
    (
        "app/Cargo.toml",
        "[package]\nname = \"app\"\n\n[dependencies]\nmy-core = { path = \"../core\" }\n",
    ),
    (
        "app/src/main.rs",
        "use my_core::text::rope;\nuse std::io;\nfn main() {}\n",
    ),
    ("core/src/orphan.rs", "fn lonely() {}\n"),
];

#[test]
fn expands_use_trees() {
    assert_eq!(expand_use("a::b::c"), vec![vec!["a", "b", "c"]]);
    assert_eq!(
        expand_use("crate::{x::{self, Y as Z}, w::*}"),
        vec![
            vec!["crate", "x", "self"],
            vec!["crate", "x", "Y"],
            vec!["crate", "w"],
        ]
    );
    assert_eq!(expand_use("::std::io"), vec![vec!["std", "io"]]);
    let _ = expand_use("{{{,,}}} :: ::");
    let _ = expand_use(&"{a, ".repeat(100));
}

#[test]
fn builds_packages_modules_symbols_tests_imports_and_dependencies() {
    let g = build_graph(&input(WORKSPACE), None);
    let lib = id(&g, module("my-core", "lib", &[]));
    let text = id(&g, module("my-core", "lib", &["text"]));
    let rope = id(&g, module("my-core", "lib", &["text", "rope"]));
    let util = id(&g, module("my-core", "lib", &["util"]));
    let tests = id(&g, module("my-core", "lib", &["tests"]));
    let api = id(&g, module("my-core", "test:api", &[]));
    let main = id(&g, module("app", "bin:app", &[]));
    let core = id(&g, NodeKey::Package("my-core".to_string()));
    let app = id(&g, NodeKey::Package("app".to_string()));
    let serde = id(&g, NodeKey::Dependency("serde".to_string()));

    assert!(has_edge(&g, core, lib, EdgeKind::Contains));
    assert!(has_edge(&g, lib, text, EdgeKind::Contains));
    assert!(has_edge(&g, text, rope, EdgeKind::Contains));
    assert!(has_edge(&g, lib, util, EdgeKind::Contains));
    // Kinds: functions and types are distinguished.
    let buffer = id(
        &g,
        item(
            module("my-core", "lib", &["text"]),
            &[],
            "Buffer",
            SymbolKind::Struct,
        ),
    );
    assert_eq!(g.node(buffer).unwrap().kind, NodeKind::Type);
    let new = id(
        &g,
        item(
            module("my-core", "lib", &["text"]),
            &["Buffer"],
            "new",
            SymbolKind::Method,
        ),
    );
    assert_eq!(g.node(new).unwrap().kind, NodeKind::Function);
    assert!(has_edge(&g, text, new, EdgeKind::Defines));
    // Tests.
    let works = id(
        &g,
        item(
            module("my-core", "lib", &["tests"]),
            &[],
            "works",
            SymbolKind::Function,
        ),
    );
    assert_eq!(g.node(works).unwrap().kind, NodeKind::Test);
    assert!(has_edge(&g, tests, works, EdgeKind::Contains));
    assert!(has_edge(&g, works, lib, EdgeKind::Tests));
    let integration = id(
        &g,
        item(
            module("my-core", "test:api", &[]),
            &[],
            "integration",
            SymbolKind::Function,
        ),
    );
    assert!(has_edge(&g, integration, core, EdgeKind::Tests));
    // Imports, including a declared dependency and a workspace crate.
    assert!(has_edge(&g, rope, util, EdgeKind::Imports));
    assert!(has_edge(&g, lib, text, EdgeKind::Imports));
    assert!(has_edge(&g, lib, serde, EdgeKind::Imports));
    assert!(has_edge(&g, api, text, EdgeKind::Imports));
    assert!(has_edge(&g, main, rope, EdgeKind::Imports));
    assert!(has_edge(&g, tests, lib, EdgeKind::Imports));
    // Dependencies.
    assert!(has_edge(&g, core, serde, EdgeKind::DependsOn));
    assert!(has_edge(&g, app, core, EdgeKind::DependsOn));
    assert_eq!(g.stats.unreachable_files, 1, "orphan.rs");
    assert_eq!(g.stats.external_imports, 2, "serde (declared) and std::io");
    // Provenance on every edge: source, analysis, file (and line for syntax edges).
    for n in g.nodes() {
        for e in g.outgoing(n.id) {
            assert!(!e.provenance.source.is_empty() && !e.provenance.kind.is_empty());
            assert!(e.provenance.path.is_some(), "{e:?}");
            if e.provenance.analysis == "syntax" {
                assert!(e.provenance.line.is_some() && e.provenance.version.is_some());
            }
        }
    }
}

#[test]
fn ids_are_stable_across_rebuilds_line_shifts_and_reported_renames() {
    let mut files: Vec<(&str, String)> =
        WORKSPACE.iter().map(|(p, t)| (*p, t.to_string())).collect();
    let as_input = |files: &[(&str, String)]| {
        let refs: Vec<(&str, &str)> = files.iter().map(|(p, t)| (*p, t.as_str())).collect();
        input(&refs)
    };
    let g1 = build_graph(&as_input(&files), None);
    let split_key = item(
        module("my-core", "lib", &["text", "rope"]),
        &[],
        "split",
        SymbolKind::Function,
    );
    let split = id(&g1, split_key.clone());
    let text_file = g1.file_node(Path::new("core/src/text.rs")).unwrap().id;
    // Shift every line of rope.rs.
    files[4].1 = format!("// a comment\n// another\n{}", files[4].1);
    let g2 = build_graph(&as_input(&files), Some(g1));
    assert_eq!(
        id(&g2, split_key.clone()),
        split,
        "line shifts keep the identity"
    );
    assert_eq!(g2.node(split).unwrap().start.unwrap().line, 3);

    // A reported symbol rename keeps the identity.
    let mut g2 = g2;
    assert!(g2.rename_symbol(&split_key, "split_at"));
    files[4].1 = files[4].1.replace("fn split", "fn split_at");
    let g3 = build_graph(&as_input(&files), Some(g2));
    let renamed = item(
        module("my-core", "lib", &["text", "rope"]),
        &[],
        "split_at",
        SymbolKind::Function,
    );
    assert_eq!(id(&g3, renamed), split);
    assert!(g3.id_of(&split_key).is_none(), "the old name is gone");

    // A reported file rename keeps the file identity (module moves to text/mod.rs).
    let mut g3 = g3;
    g3.rename_file(
        Path::new("core/src/text.rs"),
        Path::new("core/src/text/mod.rs"),
    );
    files[3].0 = "core/src/text/mod.rs";
    let g4 = build_graph(&as_input(&files), Some(g3));
    assert_eq!(
        g4.file_node(Path::new("core/src/text/mod.rs")).unwrap().id,
        text_file
    );
    // The module identity is its module path, so it is unchanged too.
    assert!(g4.id_of(&module("my-core", "lib", &["text"])).is_some());
}

#[test]
fn local_edits_update_in_place_and_structural_edits_ask_for_a_rebuild() {
    let mut parser = SyntaxParser::new().unwrap();
    let mut g = build_graph(&input(WORKSPACE), None);
    let path = Path::new("core/src/text.rs");
    let generation = g.generation();
    let local = facts(
        &mut parser,
        "pub mod rope;\npub struct Buffer;\nimpl Buffer { pub fn new() -> Self { Buffer } pub fn len(&self) -> usize { 0 } }\n",
    );
    g.update_file(path, SourceVersion::Buffer { revision: 99 }, &local)
        .unwrap();
    assert!(g.generation() > generation);
    let len = id(
        &g,
        item(
            module("my-core", "lib", &["text"]),
            &["Buffer"],
            "len",
            SymbolKind::Method,
        ),
    );
    assert_eq!(
        g.node(len).unwrap().version,
        Some(SourceVersion::Buffer { revision: 99 })
    );
    assert_eq!(
        g.file_version(path),
        Some(SourceVersion::Buffer { revision: 99 })
    );
    let structural = facts(
        &mut parser,
        "pub mod rope;\npub mod extra;\npub struct Buffer;\n",
    );
    assert_eq!(
        g.update_file(path, SourceVersion::Buffer { revision: 100 }, &structural),
        Err(NeedsRebuild::StructureChanged)
    );
    assert_eq!(
        g.update_file(
            Path::new("core/src/orphan.rs"),
            SourceVersion::Buffer { revision: 1 },
            &local
        ),
        Err(NeedsRebuild::UnknownFile)
    );
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}

/// Random local and structural edits; after each one the incrementally maintained graph
/// (update in place, rebuild only when asked) must equal a fresh full build.
#[test]
fn incremental_updates_match_full_rebuilds() {
    let mut parser = SyntaxParser::new().unwrap();
    for seed in 1..=12u64 {
        let mut rng = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15));
        let mut files: BTreeMap<String, String> = WORKSPACE
            .iter()
            .map(|(p, t)| (p.to_string(), t.to_string()))
            .collect();
        let to_input = |files: &BTreeMap<String, String>, parser: &mut SyntaxParser| {
            let mut input = GraphInput {
                root_name: "ws".to_string(),
                ..GraphInput::default()
            };
            for (i, (p, t)) in files.iter().enumerate() {
                if p.ends_with("Cargo.toml") {
                    input.manifests.insert(PathBuf::from(p), t.clone());
                } else {
                    input.files.insert(
                        PathBuf::from(p),
                        (
                            SourceVersion::Buffer { revision: i as u64 },
                            facts(parser, t),
                        ),
                    );
                }
            }
            input
        };
        let mut incremental = build_graph(&to_input(&files, &mut parser), None);
        let rust: Vec<String> = files
            .keys()
            .filter(|p| p.ends_with(".rs"))
            .cloned()
            .collect();
        for step in 0..30 {
            let path = rust[rng.below(rust.len())].clone();
            let text = files.get_mut(&path).unwrap();
            match rng.below(6) {
                0 => text.push_str(&format!("pub fn f{step}() {{}}\n")),
                1 => text.push_str(&format!("#[test]\nfn t{step}() {{}}\n")),
                2 => text.push_str(&format!(
                    "pub struct S{step};\nimpl S{step} {{ fn m(&self) {{}} }}\n"
                )),
                3 => text.insert_str(0, "// shifted\n"),
                4 => text.push_str(&format!("mod inner{step} {{ pub fn g() {{}} }}\n")),
                _ => text.push_str(&format!("use crate::text{step};\n")),
            }
            let new_facts = facts(&mut parser, text);
            let input = to_input(&files, &mut parser);
            let version = input.files[Path::new(&path)].0;
            match incremental.update_file(Path::new(&path), version, &new_facts) {
                Ok(()) => {}
                Err(_) => incremental = build_graph(&input, Some(incremental)),
            }
            let fresh = build_graph(&input, None);
            assert_eq!(
                incremental.canonical(),
                fresh.canonical(),
                "seed {seed} step {step}: incremental graph differs from a full rebuild after editing {path}"
            );
        }
        assert!(
            incremental.stats.local_updates > 0,
            "seed {seed}: some updates were local"
        );
    }
}

#[test]
fn unresolved_modules_and_bad_manifests_are_counted() {
    let g = build_graph(
        &input(&[
            ("Cargo.toml", "[package]\nname = \"x\"\n"),
            ("src/lib.rs", "mod missing;\nuse self::missing::Thing;\n"),
            ("bad/Cargo.toml", "this is = = not toml"),
        ]),
        None,
    );
    assert_eq!(g.stats.unresolved_modules, 1);
    assert_eq!(g.stats.manifest_errors, 1);
    assert_eq!(g.edge_count(EdgeKind::Imports), 0);
}
