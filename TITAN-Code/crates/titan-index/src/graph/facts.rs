//! Per-file facts for the project graph: module declarations, `use` paths, definitions
//! and tests, from one tree-sitter parse. Facts are cached per file version, so a graph
//! update re-parses only files that changed.

use std::collections::BTreeSet;

use titan_text::TextBuffer;
use tree_sitter::Node;

use crate::{LineCol, SymbolKind, SyntaxParser, symbols_from_tree};

/// A `mod` item: declared (`mod x;`) or inline (`mod x { .. }`).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct ModDecl {
    /// Inline module path inside the file of the declaring module.
    pub parent: Vec<String>,
    pub name: String,
    pub inline: bool,
    pub cfg_test: bool,
    pub line: usize,
}

/// A `use` declaration, expanded into full paths.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct UseDecl {
    pub module: Vec<String>,
    pub paths: Vec<Vec<String>>,
    pub line: usize,
}

/// A definition in a file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FactSymbol {
    /// Inline module path inside the file.
    pub module: Vec<String>,
    /// Names of enclosing non-module definitions (impl, trait, enum).
    pub container: Vec<String>,
    /// Shared with the graph node and key built from it.
    pub name: std::sync::Arc<str>,
    pub kind: SymbolKind,
    pub start: LineCol,
    pub end: LineCol,
    pub is_test: bool,
}

/// Everything the graph needs from one file, computed once per file version.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileFacts {
    pub mods: Vec<ModDecl>,
    pub uses: Vec<UseDecl>,
    pub symbols: Vec<FactSymbol>,
    pub has_errors: bool,
}

impl FileFacts {
    /// Hash of the facts that shape the module tree and imports. Equal signatures mean an
    /// edit only changed local definitions, so the graph can update this file alone.
    pub fn structure_signature(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut h = std::collections::hash_map::DefaultHasher::new();
        self.mods.hash(&mut h);
        self.uses.hash(&mut h);
        h.finish()
    }
}

fn text_of(node: Node, source: &str) -> String {
    source
        .get(node.start_byte()..node.end_byte())
        .unwrap_or_default()
        .to_string()
}

fn attributes(node: Node, source: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut sibling = node.prev_named_sibling();
    while let Some(s) = sibling {
        match s.kind() {
            "attribute_item" => out.push(text_of(s, source).split_whitespace().collect()),
            "line_comment" | "block_comment" => {}
            _ => break,
        }
        sibling = s.prev_named_sibling();
    }
    out
}

fn is_test_attribute(attr: &str) -> bool {
    attr == "#[test]" || (attr.starts_with("#[") && attr.ends_with("::test]"))
}

/// Parse `text` and collect its facts.
pub fn collect_facts(parser: &mut SyntaxParser, text: &str) -> Option<FileFacts> {
    let buffer = TextBuffer::new(text);
    let tree = parser.parse(&buffer, None, &|| false).ok().flatten()?;
    Some(facts_from_tree(&tree, &buffer))
}

/// Facts from an existing parse tree of `buffer` (no re-parse).
pub fn facts_from_tree(tree: &tree_sitter::Tree, buffer: &TextBuffer) -> FileFacts {
    let text_owned = buffer.text();
    let text = text_owned.as_str();
    let mut tests: BTreeSet<usize> = BTreeSet::new();
    let mut facts = FileFacts {
        mods: Vec::new(),
        uses: Vec::new(),
        symbols: Vec::new(),
        has_errors: tree.root_node().has_error(),
    };
    // Explicit stack: (container node, inline module path).
    let mut stack: Vec<(Node, Vec<String>)> = vec![(tree.root_node(), Vec::new())];
    while let Some((container, path)) = stack.pop() {
        let mut cursor = container.walk();
        let children: Vec<Node> = container.named_children(&mut cursor).collect();
        for item in children {
            let line = item.start_position().row;
            match item.kind() {
                "mod_item" => {
                    let Some(name) = item.child_by_field_name("name").map(|n| text_of(n, text))
                    else {
                        continue;
                    };
                    let cfg_test = attributes(item, text).iter().any(|a| a == "#[cfg(test)]");
                    let body = item.child_by_field_name("body");
                    facts.mods.push(ModDecl {
                        parent: path.clone(),
                        name: name.clone(),
                        inline: body.is_some(),
                        cfg_test,
                        line,
                    });
                    if let Some(body) = body {
                        let mut inner = path.clone();
                        inner.push(name);
                        stack.push((body, inner));
                    }
                }
                "use_declaration" => {
                    if let Some(argument) = item.child_by_field_name("argument") {
                        facts.uses.push(UseDecl {
                            module: path.clone(),
                            paths: expand_use(&text_of(argument, text)),
                            line,
                        });
                    }
                }
                "function_item" => {
                    if attributes(item, text).iter().any(|a| is_test_attribute(a)) {
                        tests.insert(item.start_byte());
                    }
                }
                _ => {}
            }
        }
    }
    // Symbols with their inline module path (Module ancestors) and containers.
    let symbols = symbols_from_tree(tree, buffer);
    for symbol in &symbols {
        if symbol.kind == SymbolKind::Module {
            continue;
        }
        let mut module = Vec::new();
        let mut container = Vec::new();
        let mut parent = symbol.parent;
        while let Some(i) = parent {
            if symbols[i].kind == SymbolKind::Module {
                module.push(symbols[i].name.clone());
            } else {
                container.push(symbols[i].name.clone());
            }
            parent = symbols[i].parent;
        }
        module.reverse();
        container.reverse();
        facts.symbols.push(FactSymbol {
            module,
            container,
            name: std::sync::Arc::from(symbol.name.as_str()),
            kind: symbol.kind,
            start: symbol.name_start,
            end: symbol.name_end,
            is_test: symbol.kind == SymbolKind::Function && tests.contains(&symbol.range.start),
        });
    }
    facts
}

// ----- `use` trees ---------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
enum Token {
    Ident(String),
    PathSep,
    Open,
    Close,
    Comma,
    Star,
}

fn tokenize(text: &str) -> Vec<Token> {
    let mut out = Vec::new();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c.is_whitespace() {
            i += 1;
        } else if c == ':' && chars.get(i + 1) == Some(&':') {
            out.push(Token::PathSep);
            i += 2;
        } else if c == '{' {
            out.push(Token::Open);
            i += 1;
        } else if c == '}' {
            out.push(Token::Close);
            i += 1;
        } else if c == ',' {
            out.push(Token::Comma);
            i += 1;
        } else if c == '*' {
            out.push(Token::Star);
            i += 1;
        } else if c.is_alphanumeric() || c == '_' || c == '#' {
            let start = i;
            while i < chars.len()
                && (chars[i].is_alphanumeric() || chars[i] == '_' || chars[i] == '#')
            {
                i += 1;
            }
            let word: String = chars[start..i].iter().collect();
            out.push(Token::Ident(word.trim_start_matches("r#").to_string()));
        } else {
            // Comments or anything unexpected: stop; the paths so far are still useful.
            break;
        }
    }
    out
}

/// Expand a `use` argument (`a::b::{c, d::e as f, g::*}`) into full paths.
pub(crate) fn expand_use(text: &str) -> Vec<Vec<String>> {
    let tokens = tokenize(text);
    let mut out = Vec::new();
    let mut pos = 0;
    if tokens.first() == Some(&Token::PathSep) {
        pos = 1;
    }
    parse_use_tree(&tokens, &mut pos, Vec::new(), &mut out, 0);
    out
}

fn parse_use_tree(
    tokens: &[Token],
    pos: &mut usize,
    prefix: Vec<String>,
    out: &mut Vec<Vec<String>>,
    depth: usize,
) {
    if depth > 32 {
        return;
    }
    let mut path = prefix;
    while let Some(token) = tokens.get(*pos) {
        match token {
            Token::Ident(word) => {
                path.push(word.clone());
                *pos += 1;
                if tokens.get(*pos) == Some(&Token::Ident("as".to_string())) {
                    *pos += 2;
                }
                if tokens.get(*pos) == Some(&Token::PathSep) {
                    *pos += 1;
                    continue;
                }
                out.push(path);
                return;
            }
            Token::Open => {
                *pos += 1;
                while let Some(token) = tokens.get(*pos) {
                    match token {
                        Token::Close => {
                            *pos += 1;
                            return;
                        }
                        Token::Comma => *pos += 1,
                        _ => {
                            let before = *pos;
                            parse_use_tree(tokens, pos, path.clone(), out, depth + 1);
                            if *pos == before {
                                *pos += 1;
                            }
                        }
                    }
                }
                return;
            }
            Token::Star => {
                *pos += 1;
                out.push(path);
                return;
            }
            _ => {
                if !path.is_empty() {
                    out.push(path);
                }
                return;
            }
        }
    }
    if !path.is_empty() {
        out.push(path);
    }
}
