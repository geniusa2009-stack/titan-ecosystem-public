//! TITAN Code project intelligence, first layer: a syntactic Rust symbol index.
//!
//! **Evidence level: syntax only.** Symbols come from tree-sitter parse trees
//! ([`EVIDENCE`]). This finds definitions and their nesting; it does not resolve names,
//! types, imports or references. Nothing here claims semantic precision; that needs a
//! language server or compiler-backed analysis later.
//!
//! Every file entry records exactly which content it was computed from ([`SourceVersion`]:
//! a buffer revision or a disk length + hash). [`ProjectIndex::update`] skips files whose
//! version is unchanged and re-parses the rest, so invalidation is per file and per
//! revision. Each change to the index bumps its `generation`.

pub mod graph;

use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;
use std::path::{Path, PathBuf};

use titan_text::{ByteOffset, TextBuffer, TextEdit};
use tree_sitter::{InputEdit, Node, ParseOptions, ParseState, Parser, Point, Tree};

/// Provenance label attached to every symbol produced by this crate.
pub const EVIDENCE: &str = "syntax:tree-sitter-rust";

#[derive(Debug, thiserror::Error)]
pub enum IndexError {
    #[error("cannot load the tree-sitter Rust grammar: {0}")]
    Grammar(String),
    #[error("tree-sitter returned no tree for {path:?}")]
    ParseFailed { path: PathBuf },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum SymbolKind {
    Module,
    Struct,
    Enum,
    EnumVariant,
    Union,
    Trait,
    Impl,
    Function,
    Method,
    Const,
    Static,
    TypeAlias,
    Macro,
}

impl SymbolKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Module => "module",
            Self::Struct => "struct",
            Self::Enum => "enum",
            Self::EnumVariant => "enumVariant",
            Self::Union => "union",
            Self::Trait => "trait",
            Self::Impl => "impl",
            Self::Function => "function",
            Self::Method => "method",
            Self::Const => "const",
            Self::Static => "static",
            Self::TypeAlias => "typeAlias",
            Self::Macro => "macro",
        }
    }
}

/// A position recorded in both column units. Lines are counted by LF (tree-sitter rows).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineCol {
    pub line: usize,
    pub byte_column: usize,
    pub utf16_column: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Symbol {
    pub name: String,
    pub kind: SymbolKind,
    /// Byte range of the whole definition.
    pub range: Range<usize>,
    /// Byte range of the name.
    pub name_range: Range<usize>,
    pub name_start: LineCol,
    pub name_end: LineCol,
    /// Index of the enclosing symbol in the same file, if any.
    pub parent: Option<usize>,
}

/// The exact content an index entry was computed from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SourceVersion {
    /// An open document at this buffer revision.
    Buffer { revision: u64 },
    /// A file on disk with this length and content hash.
    Disk { len: u64, hash: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileIndex {
    pub version: SourceVersion,
    pub symbols: Vec<Symbol>,
    /// The parse tree contains error nodes; symbols are best-effort.
    pub has_syntax_errors: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UpdateOutcome {
    Unchanged,
    Reindexed,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct IndexStats {
    pub generation: u64,
    pub files: usize,
    pub symbols: usize,
    pub files_with_syntax_errors: usize,
}

#[derive(Clone, Debug)]
pub struct SymbolMatch<'a> {
    pub path: &'a Path,
    pub version: SourceVersion,
    pub symbol: &'a Symbol,
    pub container: Option<&'a Symbol>,
}

/// A tree-sitter Rust parser that reads directly from a [`TextBuffer`] (no copy of the
/// document), can reuse a previous tree, and can be cancelled while parsing.
pub struct SyntaxParser {
    parser: Parser,
}

impl std::fmt::Debug for SyntaxParser {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SyntaxParser").finish_non_exhaustive()
    }
}

impl SyntaxParser {
    pub fn new() -> Result<Self, IndexError> {
        let mut parser = Parser::new();
        parser
            .set_language(&tree_sitter_rust::LANGUAGE.into())
            .map_err(|e| IndexError::Grammar(e.to_string()))?;
        Ok(Self { parser })
    }

    /// Parse `buffer`. When `old_tree` is given it must already have been edited (see
    /// [`input_edits`]) to match `buffer`; unchanged subtrees are then reused.
    /// Returns `Ok(None)` if `cancelled` returned true during parsing.
    pub fn parse(
        &mut self,
        buffer: &TextBuffer,
        old_tree: Option<&Tree>,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<Tree>, IndexError> {
        let mut input = |byte: usize, _position: Point| {
            let (chunk, start) = buffer.chunk_at_byte(byte);
            chunk
                .as_bytes()
                .get(byte.saturating_sub(start)..)
                .unwrap_or(&[])
        };
        let mut progress = |_state: &ParseState| cancelled();
        let options = ParseOptions::new().progress_callback(&mut progress);
        match self
            .parser
            .parse_with_options(&mut input, old_tree, Some(options))
        {
            Some(tree) => Ok(Some(tree)),
            None => {
                // A halted parse keeps internal state for resumption; start clean next time.
                self.parser.reset();
                if cancelled() {
                    Ok(None)
                } else {
                    Err(IndexError::ParseFailed {
                        path: PathBuf::new(),
                    })
                }
            }
        }
    }
}

fn ts_point(buffer: &TextBuffer, offset: usize) -> Point {
    match buffer.byte_to_point(ByteOffset(offset)) {
        Ok(p) => Point {
            row: p.line,
            column: p.column,
        },
        Err(_) => Point { row: 0, column: 0 },
    }
}

/// End position of `text` inserted at `start`, counting LF, CRLF and lone CR as line breaks
/// (the same convention as `TextBuffer`).
fn advance(start: Point, text: &str) -> Point {
    let bytes = text.as_bytes();
    let mut row = start.row;
    let mut line_start = None;
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\r' if bytes.get(i + 1) == Some(&b'\n') => {
                row += 1;
                i += 1;
                line_start = Some(i + 1);
            }
            b'\r' | b'\n' => {
                row += 1;
                line_start = Some(i + 1);
            }
            _ => {}
        }
        i += 1;
    }
    match line_start {
        Some(ls) => Point {
            row,
            column: bytes.len() - ls,
        },
        None => Point {
            row,
            column: start.column + bytes.len(),
        },
    }
}

/// Tree-sitter edits for one committed change. `before` is the text the change was applied
/// to and `edits` are its applied edits, **sorted by start** and in `before` coordinates
/// (exactly what `TextBuffer::apply` / `Change::edits` provide). The result must be
/// applied in order with `Tree::edit`: it runs from the highest offset down, so every edit's
/// coordinates are still valid when it is applied.
pub fn input_edits(before: &TextBuffer, edits: &[TextEdit]) -> Vec<InputEdit> {
    edits
        .iter()
        .rev()
        .map(|e| {
            let start_position = ts_point(before, e.start.0);
            InputEdit {
                start_byte: e.start.0,
                old_end_byte: e.end.0,
                new_end_byte: e.start.0 + e.text.len(),
                start_position,
                old_end_position: ts_point(before, e.end.0),
                new_end_position: advance(start_position, &e.text),
            }
        })
        .collect()
}

fn line_col(buffer: &TextBuffer, offset: usize) -> LineCol {
    let point = buffer.byte_to_point(ByteOffset(offset)).unwrap_or_default();
    let utf16 = buffer
        .byte_to_utf16_point(ByteOffset(offset))
        .unwrap_or_default();
    LineCol {
        line: point.line,
        byte_column: point.column,
        utf16_column: utf16.column,
    }
}

/// Definitions in `tree` (parsed from `buffer`), with nesting. Positions come from the
/// buffer, so lines follow the protocol's convention (LF, CRLF and CR end a line).
pub fn symbols_from_tree(tree: &Tree, buffer: &TextBuffer) -> Vec<Symbol> {
    let mut symbols = Vec::new();
    // Explicit stack instead of recursion: deeply nested files cannot overflow.
    let mut stack: Vec<(Node, Option<usize>, Option<SymbolKind>)> =
        vec![(tree.root_node(), None, None)];
    while let Some((node, parent, parent_kind)) = stack.pop() {
        let mut child_parent = parent;
        let mut child_parent_kind = parent_kind;
        let named = classify(node.kind(), parent_kind).and_then(|kind| {
            symbol_name(node, kind, buffer).map(|(name, name_range)| (kind, name, name_range))
        });
        if let Some((kind, name, name_range)) = named {
            symbols.push(Symbol {
                name,
                kind,
                range: node.start_byte()..node.end_byte(),
                name_start: line_col(buffer, name_range.start),
                name_end: line_col(buffer, name_range.end),
                name_range,
                parent,
            });
            child_parent = Some(symbols.len() - 1);
            child_parent_kind = Some(kind);
        }
        let mut cursor = node.walk();
        let children: Vec<Node> = node.named_children(&mut cursor).collect();
        for child in children.into_iter().rev() {
            stack.push((child, child_parent, child_parent_kind));
        }
    }
    symbols
}

/// One syntax problem found by the parser. Evidence: syntax only.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SyntaxDiagnostic {
    pub range: Range<usize>,
    pub start: LineCol,
    pub end: LineCol,
    pub message: String,
}

/// ERROR and MISSING nodes in `tree`, in document order, at most `limit`.
pub fn syntax_diagnostics(tree: &Tree, buffer: &TextBuffer, limit: usize) -> Vec<SyntaxDiagnostic> {
    let mut out = Vec::new();
    if !tree.root_node().has_error() {
        return out;
    }
    let mut stack = vec![tree.root_node()];
    while let Some(node) = stack.pop() {
        if out.len() >= limit {
            break;
        }
        let message = if node.is_missing() {
            Some(format!("missing `{}`", node.kind()))
        } else if node.is_error() {
            Some("syntax error".to_string())
        } else {
            None
        };
        if let Some(message) = message {
            out.push(SyntaxDiagnostic {
                range: node.start_byte()..node.end_byte(),
                start: line_col(buffer, node.start_byte()),
                end: line_col(buffer, node.end_byte()),
                message,
            });
            continue;
        }
        let mut cursor = node.walk();
        let children: Vec<Node> = node
            .children(&mut cursor)
            .filter(|c| c.has_error() || c.is_missing())
            .collect();
        stack.extend(children.into_iter().rev());
    }
    out
}

/// Everything derived from one parse of one file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParsedFile {
    pub symbols: Vec<Symbol>,
    pub has_syntax_errors: bool,
    pub diagnostics: Vec<SyntaxDiagnostic>,
}

pub fn analyze(tree: &Tree, buffer: &TextBuffer, diagnostic_limit: usize) -> ParsedFile {
    ParsedFile {
        symbols: symbols_from_tree(tree, buffer),
        has_syntax_errors: tree.root_node().has_error(),
        diagnostics: syntax_diagnostics(tree, buffer, diagnostic_limit),
    }
}

/// Full parse of a string (for files that are not open in an editor).
pub struct RustSymbolExtractor {
    parser: SyntaxParser,
}

impl std::fmt::Debug for RustSymbolExtractor {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RustSymbolExtractor")
            .finish_non_exhaustive()
    }
}

impl RustSymbolExtractor {
    pub fn new() -> Result<Self, IndexError> {
        Ok(Self {
            parser: SyntaxParser::new()?,
        })
    }

    /// Parse `text` and return its symbols and whether the tree has syntax errors.
    pub fn extract(&mut self, path: &Path, text: &str) -> Result<(Vec<Symbol>, bool), IndexError> {
        let buffer = TextBuffer::new(text);
        let tree = self
            .parser
            .parse(&buffer, None, &|| false)?
            .ok_or_else(|| IndexError::ParseFailed {
                path: path.to_path_buf(),
            })?;
        Ok((
            symbols_from_tree(&tree, &buffer),
            tree.root_node().has_error(),
        ))
    }
}

fn classify(node_kind: &str, parent_kind: Option<SymbolKind>) -> Option<SymbolKind> {
    let in_impl_or_trait = matches!(parent_kind, Some(SymbolKind::Impl | SymbolKind::Trait));
    Some(match node_kind {
        "mod_item" => SymbolKind::Module,
        "struct_item" => SymbolKind::Struct,
        "enum_item" => SymbolKind::Enum,
        "enum_variant" => SymbolKind::EnumVariant,
        "union_item" => SymbolKind::Union,
        "trait_item" => SymbolKind::Trait,
        "impl_item" => SymbolKind::Impl,
        "function_item" | "function_signature_item" if in_impl_or_trait => SymbolKind::Method,
        "function_item" | "function_signature_item" => SymbolKind::Function,
        "const_item" => SymbolKind::Const,
        "static_item" => SymbolKind::Static,
        "type_item" => SymbolKind::TypeAlias,
        "macro_definition" => SymbolKind::Macro,
        _ => return None,
    })
}

fn node_text(node: Node, buffer: &TextBuffer) -> Option<String> {
    buffer
        .slice(ByteOffset(node.start_byte())..ByteOffset(node.end_byte()))
        .ok()
}

fn symbol_name(
    node: Node,
    kind: SymbolKind,
    buffer: &TextBuffer,
) -> Option<(String, Range<usize>)> {
    let field = if kind == SymbolKind::Impl {
        "type"
    } else {
        "name"
    };
    let name_node = node.child_by_field_name(field)?;
    let range = name_node.start_byte()..name_node.end_byte();
    let type_name = node_text(name_node, buffer)?;
    let trait_name = if kind == SymbolKind::Impl {
        node.child_by_field_name("trait")
            .and_then(|t| node_text(t, buffer))
    } else {
        None
    };
    let name = match trait_name {
        Some(trait_name) => format!("{trait_name} for {type_name}"),
        None => type_name,
    };
    Some((name, range))
}

/// Symbol index for a set of files, with per-file, per-version invalidation.
#[derive(Debug)]
pub struct ProjectIndex {
    extractor: RustSymbolExtractor,
    files: BTreeMap<PathBuf, FileIndex>,
    generation: u64,
}

impl ProjectIndex {
    pub fn new() -> Result<Self, IndexError> {
        Ok(Self {
            extractor: RustSymbolExtractor::new()?,
            files: BTreeMap::new(),
            generation: 0,
        })
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn file(&self, path: &Path) -> Option<&FileIndex> {
        self.files.get(path)
    }

    /// Every indexed path with the version it was computed from.
    pub fn versions(&self) -> impl Iterator<Item = (&Path, SourceVersion)> + '_ {
        self.files
            .iter()
            .map(|(path, file)| (path.as_path(), file.version))
    }

    /// True if `path` is indexed at exactly `version`.
    pub fn is_current(&self, path: &Path, version: SourceVersion) -> bool {
        self.files.get(path).is_some_and(|f| f.version == version)
    }

    /// Index `text` for `path` unless the entry is already at `version`.
    pub fn update(
        &mut self,
        path: &Path,
        version: SourceVersion,
        text: &str,
    ) -> Result<UpdateOutcome, IndexError> {
        if self.is_current(path, version) {
            return Ok(UpdateOutcome::Unchanged);
        }
        let (symbols, has_syntax_errors) = self.extractor.extract(path, text)?;
        self.files.insert(
            path.to_path_buf(),
            FileIndex {
                version,
                symbols,
                has_syntax_errors,
            },
        );
        self.generation += 1;
        Ok(UpdateOutcome::Reindexed)
    }

    /// Store an entry computed elsewhere (e.g. by a background parse). Replaces any previous
    /// entry for `path` and bumps the generation.
    pub fn publish(&mut self, path: &Path, entry: FileIndex) {
        self.files.insert(path.to_path_buf(), entry);
        self.generation += 1;
    }

    /// Remove `path`. Returns true if it was indexed.
    pub fn remove(&mut self, path: &Path) -> bool {
        let removed = self.files.remove(path).is_some();
        if removed {
            self.generation += 1;
        }
        removed
    }

    /// Drop entries for files not in `keep` (deleted or now ignored). Returns how many.
    pub fn retain_only(&mut self, keep: &BTreeSet<PathBuf>) -> usize {
        let before = self.files.len();
        self.files.retain(|path, _| keep.contains(path));
        let removed = before - self.files.len();
        if removed > 0 {
            self.generation += 1;
        }
        removed
    }

    /// Approximate heap bytes held by the index (paths, symbol records, names). An
    /// estimate for memory reports, not an allocator measurement.
    pub fn estimated_bytes(&self) -> usize {
        self.files
            .iter()
            .map(|(path, entry)| {
                path.as_os_str().len()
                    + std::mem::size_of::<FileIndex>()
                    + entry
                        .symbols
                        .iter()
                        .map(|s| std::mem::size_of::<Symbol>() + s.name.capacity())
                        .sum::<usize>()
            })
            .sum()
    }

    pub fn stats(&self) -> IndexStats {
        IndexStats {
            generation: self.generation,
            files: self.files.len(),
            symbols: self.files.values().map(|f| f.symbols.len()).sum(),
            files_with_syntax_errors: self.files.values().filter(|f| f.has_syntax_errors).count(),
        }
    }

    /// Case-insensitive substring search over symbol names. Exact (case-insensitive) name
    /// matches come first, then by path and position.
    pub fn search(&self, query: &str, limit: usize) -> Vec<SymbolMatch<'_>> {
        let needle = query.to_lowercase();
        let mut found: Vec<(bool, SymbolMatch<'_>)> = Vec::new();
        for (path, file) in &self.files {
            for symbol in &file.symbols {
                let name = symbol.name.to_lowercase();
                if !name.contains(&needle) {
                    continue;
                }
                found.push((
                    name == needle,
                    SymbolMatch {
                        path,
                        version: file.version,
                        symbol,
                        container: symbol.parent.and_then(|i| file.symbols.get(i)),
                    },
                ));
            }
        }
        // Stable sort keeps path/position order within each group.
        found.sort_by_key(|(exact, _)| !*exact);
        found.into_iter().take(limit).map(|(_, m)| m).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"// تعليق: مرحبا
pub mod engine {
    pub struct Buffer { len: usize }
    pub enum Mode { Insert, Normal }
    pub trait Render {
        fn render(&self) -> String;
    }
    impl Render for Buffer {
        fn render(&self) -> String { String::new() }
    }
    impl Buffer {
        pub fn new() -> Self { Buffer { len: 0 } }
    }
}
const LIMIT: usize = 10;
static NAME: &str = "titan";
type Id = u64;
macro_rules! shout { () => {} }
fn main() {}
"#;

    fn names(symbols: &[Symbol]) -> Vec<(String, SymbolKind)> {
        symbols.iter().map(|s| (s.name.clone(), s.kind)).collect()
    }

    #[test]
    fn extracts_definitions_with_nesting() {
        let mut extractor = RustSymbolExtractor::new().unwrap();
        let (symbols, errors) = extractor.extract(Path::new("lib.rs"), SAMPLE).unwrap();
        assert!(!errors);
        let got = names(&symbols);
        let expected: Vec<(String, SymbolKind)> = [
            ("engine", SymbolKind::Module),
            ("Buffer", SymbolKind::Struct),
            ("Mode", SymbolKind::Enum),
            ("Insert", SymbolKind::EnumVariant),
            ("Normal", SymbolKind::EnumVariant),
            ("Render", SymbolKind::Trait),
            ("render", SymbolKind::Method),
            ("Render for Buffer", SymbolKind::Impl),
            ("render", SymbolKind::Method),
            ("Buffer", SymbolKind::Impl),
            ("new", SymbolKind::Method),
            ("LIMIT", SymbolKind::Const),
            ("NAME", SymbolKind::Static),
            ("Id", SymbolKind::TypeAlias),
            ("shout", SymbolKind::Macro),
            ("main", SymbolKind::Function),
        ]
        .iter()
        .map(|(n, k)| ((*n).to_string(), *k))
        .collect();
        assert_eq!(got, expected);

        let new_fn = symbols.iter().find(|s| s.name == "new").unwrap();
        let parent = &symbols[new_fn.parent.unwrap()];
        assert_eq!(
            (parent.name.as_str(), parent.kind),
            ("Buffer", SymbolKind::Impl)
        );
        let grandparent = &symbols[parent.parent.unwrap()];
        assert_eq!(grandparent.name, "engine");
        assert_eq!(&SAMPLE[new_fn.name_range.clone()], "new");
    }

    #[test]
    fn positions_count_utf16_after_arabic() {
        let text = "const S: &str = \"مرحبا\"; fn after() {}\n";
        let mut extractor = RustSymbolExtractor::new().unwrap();
        let (symbols, _) = extractor.extract(Path::new("a.rs"), text).unwrap();
        let after = symbols.iter().find(|s| s.name == "after").unwrap();
        let byte = text.find("after").unwrap();
        assert_eq!(after.name_start.line, 0);
        assert_eq!(after.name_start.byte_column, byte);
        assert_eq!(
            after.name_start.utf16_column,
            text[..byte].encode_utf16().count()
        );
        assert!(after.name_start.utf16_column < byte);
    }

    #[test]
    fn syntax_errors_are_flagged_but_symbols_survive() {
        let text = "fn good() {}\nfn broken( {\nstruct Kept;\n";
        let mut extractor = RustSymbolExtractor::new().unwrap();
        let (symbols, errors) = extractor.extract(Path::new("b.rs"), text).unwrap();
        assert!(errors);
        assert!(symbols.iter().any(|s| s.name == "good"));
    }

    struct Rng(u64);

    impl Rng {
        fn below(&mut self, n: usize) -> usize {
            let mut x = self.0;
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            self.0 = x;
            (x.wrapping_mul(0x2545_F491_4F6C_DD1D) % n.max(1) as u64) as usize
        }
    }

    const SNIPPETS: &[&str] = &[
        "fn f() {}\n",
        "struct S { a: u8 }\n",
        "impl S {\n    fn g(&self) -> u8 { self.a }\n}\n",
        "let x = 1;",
        "{",
        "}",
        "(",
        ")",
        "// note\n",
        "\"text\"",
        "mod m {",
        "\n",
        "\r\n",
        "مرحبا",
        "😀",
        " ",
        "x",
        "",
    ];

    /// Every node in pre-order: kind, byte range, error/missing flags.
    fn node_list(tree: &Tree) -> Vec<(u16, usize, usize, bool, bool)> {
        let mut out = Vec::new();
        let mut stack = vec![tree.root_node()];
        while let Some(node) = stack.pop() {
            out.push((
                node.kind_id(),
                node.start_byte(),
                node.end_byte(),
                node.is_error(),
                node.is_missing(),
            ));
            let mut cursor = node.walk();
            let children: Vec<Node> = node.children(&mut cursor).collect();
            stack.extend(children.into_iter().rev());
        }
        out
    }

    #[test]
    fn incremental_parse_matches_full_reparse() {
        let mut incremental_parser = SyntaxParser::new().unwrap();
        let mut full_parser = SyntaxParser::new().unwrap();
        for seed in 1..=25u64 {
            let mut rng = Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
            let mut buffer = TextBuffer::new(SAMPLE);
            let mut tree = incremental_parser
                .parse(&buffer, None, &|| false)
                .unwrap()
                .unwrap();
            for step in 0..40 {
                let text = buffer.text();
                let bounds: Vec<usize> = text
                    .char_indices()
                    .map(|(i, _)| i)
                    .chain(std::iter::once(text.len()))
                    .collect();
                let count = (1 + rng.below(3)) * 2;
                let mut points: Vec<usize> = (0..count)
                    .map(|_| bounds[rng.below(bounds.len())])
                    .collect();
                points.sort_unstable();
                points.dedup();
                let mut edits = Vec::new();
                for pair in points.chunks(2) {
                    let start = pair[0];
                    let end = if pair.len() == 2 && rng.below(2) == 0 {
                        pair[1]
                    } else {
                        start
                    };
                    let snippet = SNIPPETS[rng.below(SNIPPETS.len())];
                    edits.push(TextEdit::replace(
                        ByteOffset(start),
                        ByteOffset(end),
                        snippet,
                    ));
                }
                let before = buffer.clone();
                let applied = buffer.apply(edits).unwrap();
                for edit in input_edits(&before, &applied.forward) {
                    tree.edit(&edit);
                }
                let incremental = incremental_parser
                    .parse(&buffer, Some(&tree), &|| false)
                    .unwrap()
                    .unwrap();
                let full = full_parser
                    .parse(&buffer, None, &|| false)
                    .unwrap()
                    .unwrap();
                assert_eq!(
                    node_list(&incremental),
                    node_list(&full),
                    "seed {seed} step {step}: incremental and full parse differ"
                );
                assert_eq!(
                    incremental.root_node().to_sexp(),
                    full.root_node().to_sexp(),
                    "seed {seed} step {step}"
                );
                tree = incremental;
            }
        }
    }

    #[test]
    fn cancelled_parse_returns_none_and_parser_recovers() {
        let mut parser = SyntaxParser::new().unwrap();
        let big = TextBuffer::new(&SAMPLE.repeat(2_000));
        assert!(parser.parse(&big, None, &|| true).unwrap().is_none());
        let tree = parser.parse(&big, None, &|| false).unwrap().unwrap();
        assert!(!tree.root_node().has_error());
    }

    #[test]
    fn edit_points_follow_line_breaks() {
        let before = TextBuffer::new("ab\r\ncd\nef");
        // Sorted by start, as `TextBuffer::apply` returns them.
        let edits = vec![
            TextEdit::replace(ByteOffset(0), ByteOffset(2), ""),
            TextEdit::insert(ByteOffset(5), "X\r\nYZ"),
        ];
        let input = input_edits(&before, &edits);
        // Highest offset first.
        assert_eq!(input[0].start_byte, 5);
        assert_eq!(input[0].start_position, Point { row: 1, column: 1 });
        assert_eq!(input[0].new_end_byte, 10);
        assert_eq!(input[0].new_end_position, Point { row: 2, column: 2 });
        assert_eq!(input[1].old_end_position, Point { row: 0, column: 2 });
        assert_eq!(input[1].new_end_position, Point { row: 0, column: 0 });
    }

    #[test]
    fn diagnostics_locate_syntax_errors() {
        let buffer = TextBuffer::new("fn ok() {}\nfn broken( {\n}\n");
        let mut parser = SyntaxParser::new().unwrap();
        let tree = parser.parse(&buffer, None, &|| false).unwrap().unwrap();
        let parsed = analyze(&tree, &buffer, 10);
        assert!(parsed.has_syntax_errors);
        assert!(!parsed.diagnostics.is_empty());
        assert!(parsed.diagnostics.iter().all(|d| d.start.line >= 1));
        let clean = TextBuffer::new("fn ok() {}\n");
        let tree = parser.parse(&clean, None, &|| false).unwrap().unwrap();
        assert!(analyze(&tree, &clean, 10).diagnostics.is_empty());
    }

    #[test]
    fn invalidation_is_per_file_and_per_version() {
        let mut index = ProjectIndex::new().unwrap();
        let a = Path::new("src/a.rs");
        let b = Path::new("src/b.rs");
        let v1 = SourceVersion::Buffer { revision: 1 };
        assert_eq!(
            index.update(a, v1, "fn alpha() {}").unwrap(),
            UpdateOutcome::Reindexed
        );
        let disk = SourceVersion::Disk { len: 13, hash: 42 };
        assert_eq!(
            index.update(b, disk, "fn beta() {}").unwrap(),
            UpdateOutcome::Reindexed
        );
        let generation = index.generation();

        assert_eq!(
            index.update(a, v1, "ignored").unwrap(),
            UpdateOutcome::Unchanged
        );
        assert_eq!(index.generation(), generation);
        assert!(index.is_current(a, v1));

        let v2 = SourceVersion::Buffer { revision: 2 };
        assert!(!index.is_current(a, v2));
        index.update(a, v2, "fn alpha() {}\nfn gamma() {}").unwrap();
        assert_eq!(index.generation(), generation + 1);
        assert_eq!(index.search("gamma", 10).len(), 1);
        assert_eq!(index.search("gamma", 10)[0].version, v2);

        let keep: BTreeSet<PathBuf> = [a.to_path_buf()].into_iter().collect();
        assert_eq!(index.retain_only(&keep), 1);
        assert!(index.search("beta", 10).is_empty());
        let stats = index.stats();
        assert_eq!((stats.files, stats.symbols), (1, 2));
    }

    #[test]
    fn search_ranks_exact_matches_first() {
        let mut index = ProjectIndex::new().unwrap();
        index
            .update(
                Path::new("x.rs"),
                SourceVersion::Buffer { revision: 0 },
                "fn render_all() {}\nfn Render() {}\n",
            )
            .unwrap();
        let found = index.search("render", 10);
        assert_eq!(found.len(), 2);
        assert_eq!(found[0].symbol.name, "Render");
        assert_eq!(index.search("render", 1).len(), 1);
    }
}
