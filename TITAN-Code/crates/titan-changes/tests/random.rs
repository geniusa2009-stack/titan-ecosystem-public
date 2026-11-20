//! Randomized multi-file proposals with injected failures and simulated crashes (fixed
//! seeds). Invariant: after apply / rollback / recover every file is exactly its original
//! bytes or exactly its proposed bytes, and every byte sequence that existed (original or
//! proposed) can be found on disk or in the transaction directory.

mod common;

use std::collections::BTreeMap;

use common::{Ws, all_files, create};
use titan_changes::{
    ApplyError, ApplyPoint, ByteEdit, FileChange, HookAction, Outcome, RecoveryAction,
    apply_with_hook, recover,
};

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n.max(1) as u64) as usize
    }
    fn chance(&mut self, percent: u64) -> bool {
        self.next() % 100 < percent
    }
}

const WORDS: &[&str] = &[
    "fn", "let", "x", "é", "ü", "日本", "🦀", " ", "\n", "\r\n", "{}", "42",
];

fn text(rng: &mut Rng, max_words: usize) -> String {
    (0..rng.below(max_words + 1))
        .map(|_| WORDS[rng.below(WORDS.len())])
        .collect()
}

/// Random sorted, non-overlapping edits on character boundaries.
fn random_edits(rng: &mut Rng, base: &str) -> Vec<ByteEdit> {
    let mut bounds: Vec<usize> = base.char_indices().map(|(i, _)| i).collect();
    bounds.push(base.len());
    let count = 1 + rng.below(3);
    let mut points: Vec<usize> = (0..count * 2)
        .map(|_| bounds[rng.below(bounds.len())])
        .collect();
    points.sort_unstable();
    points
        .chunks(2)
        .map(|pair| ByteEdit::new(pair[0], pair[1], text(rng, 3)))
        .collect()
}

/// The result of the edits, computed independently of the crate.
fn apply_edits(base: &str, edits: &[ByteEdit]) -> String {
    let mut out = String::new();
    let mut pos = 0;
    for edit in edits {
        out.push_str(&base[pos..edit.start]);
        out.push_str(&edit.text);
        pos = edit.end;
    }
    out.push_str(&base[pos..]);
    out
}

type State = BTreeMap<String, Option<Vec<u8>>>;

struct Case {
    ws: Ws,
    changes: Vec<FileChange>,
    original: State,
    proposed: State,
    ops: usize,
}

fn generate(rng: &mut Rng) -> Case {
    let count = 2 + rng.below(5);
    let files: Vec<(String, Vec<u8>)> = (0..count)
        .map(|i| {
            let dir = if rng.chance(50) { "src/" } else { "" };
            (format!("{dir}f{i}.txt"), text(rng, 12).into_bytes())
        })
        .collect();
    let refs: Vec<(&str, &[u8])> = files
        .iter()
        .map(|(p, b)| (p.as_str(), b.as_slice()))
        .collect();
    let ws = Ws::new(&refs);
    let mut original = State::new();
    for (path, bytes) in &files {
        original.insert(path.clone(), Some(bytes.clone()));
    }
    let mut proposed = original.clone();
    let mut changes = Vec::new();
    let mut ops = 0;
    for (i, (path, bytes)) in files.iter().enumerate() {
        let base = String::from_utf8(bytes.clone()).expect("generated text is UTF-8");
        match rng.below(5) {
            0 | 1 => {
                let edits = random_edits(rng, &base);
                proposed.insert(path.clone(), Some(apply_edits(&base, &edits).into_bytes()));
                changes.push(ws.edit(path, edits));
                ops += 1;
            }
            2 => {
                proposed.insert(path.clone(), None);
                changes.push(ws.delete(path));
                ops += 1;
            }
            3 => {
                let to = if rng.chance(50) {
                    format!("moved/deep/r{i}.txt")
                } else {
                    format!("r{i}.txt")
                };
                proposed.insert(path.clone(), None);
                original.insert(to.clone(), None);
                proposed.insert(to.clone(), Some(bytes.clone()));
                changes.push(ws.rename(path, &to));
                ops += 2;
            }
            _ => {}
        }
    }
    for c in 0..rng.below(3) {
        let path = if rng.chance(50) {
            format!("new/c{c}.txt")
        } else {
            format!("c{c}.txt")
        };
        let content = text(rng, 8);
        original.insert(path.clone(), None);
        proposed.insert(path.clone(), Some(content.clone().into_bytes()));
        changes.push(create(&path, &content));
        ops += 1;
    }
    // Shuffle the change order.
    for i in (1..changes.len()).rev() {
        let j = rng.below(i + 1);
        changes.swap(i, j);
    }
    Case {
        ws,
        changes,
        original,
        proposed,
        ops,
    }
}

#[derive(Clone, Copy, Debug)]
enum Fault {
    None,
    FailBefore(usize),
    FailAfter(usize),
    CrashBefore(usize),
    CrashAfter(usize),
    FailThenCrashInUndo(usize, usize),
    FailThenFailUndo(usize, usize),
}

fn files_of(state: &State) -> Vec<String> {
    state
        .iter()
        .filter(|(_, bytes)| bytes.is_some())
        .map(|(path, _)| path.clone())
        .collect()
}

fn disk_state(ws: &Ws, keys: &State) -> State {
    keys.keys()
        .map(|path| (path.clone(), ws.read(path)))
        .collect()
}

#[test]
fn random_proposals_with_injected_failures_keep_every_byte() {
    let mut outcomes: BTreeMap<String, usize> = BTreeMap::new();
    for seed in 1..=150u64 {
        let mut rng = Rng(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
        let case = generate(&mut rng);
        if case.changes.is_empty() {
            continue;
        }
        let k = rng.below(case.ops + 1);
        let j = rng.below(case.ops);
        let fault = match rng.below(8) {
            0 | 1 => Fault::None,
            2 => Fault::FailBefore(k),
            3 => Fault::FailAfter(k),
            4 => Fault::CrashBefore(k),
            5 => Fault::CrashAfter(k),
            6 => Fault::FailThenCrashInUndo(k, j),
            _ => Fault::FailThenFailUndo(k, j),
        };
        let ws = &case.ws;
        let proposal = ws.proposal(case.changes.clone());
        let mut hook = |point: ApplyPoint| match (fault, point) {
            (Fault::FailBefore(k), ApplyPoint::BeforeOp { index })
            | (Fault::FailThenCrashInUndo(k, _), ApplyPoint::BeforeOp { index })
            | (Fault::FailThenFailUndo(k, _), ApplyPoint::BeforeOp { index })
                if index == k =>
            {
                HookAction::Fail(format!("seed {seed}"))
            }
            (Fault::FailAfter(k), ApplyPoint::AfterOp { index }) if index == k => {
                HookAction::Fail(format!("seed {seed}"))
            }
            (Fault::CrashBefore(k), ApplyPoint::BeforeOp { index })
            | (Fault::CrashAfter(k), ApplyPoint::AfterOp { index })
                if index == k =>
            {
                HookAction::Crash
            }
            (Fault::FailThenCrashInUndo(_, j), ApplyPoint::BeforeUndo { index }) if index == j => {
                HookAction::Crash
            }
            (Fault::FailThenFailUndo(_, j), ApplyPoint::BeforeUndo { index }) if index == j => {
                HookAction::Fail(format!("seed {seed} undo"))
            }
            _ => HookAction::Continue,
        };
        let result = apply_with_hook(&proposal, &ws.view, &ws.state, &mut hook);
        let context = format!("seed {seed}, fault {fault:?}, changes {:?}", case.changes);
        let (label, txn_dir) = match result {
            Ok(report) => {
                let now = disk_state(ws, &case.original);
                match report.outcome {
                    Outcome::Applied => {
                        assert_eq!(now, case.proposed, "{context}");
                        assert_eq!(ws.files(), files_of(&case.proposed), "{context}");
                    }
                    Outcome::RolledBack | Outcome::Failed => {
                        assert_eq!(now, case.original, "{context}");
                        assert_eq!(ws.files(), files_of(&case.original), "{context}");
                    }
                    Outcome::PartiallyApplied => {
                        assert!(
                            matches!(fault, Fault::FailThenFailUndo(..)),
                            "only an undo failure leaves a partial state: {context}"
                        );
                        for (path, bytes) in &now {
                            assert!(
                                *bytes == case.original[path] || *bytes == case.proposed[path],
                                "{path} is neither original nor proposed: {context}"
                            );
                        }
                        assert!(!report.rollback_conflicts.is_empty());
                    }
                }
                (format!("{:?}", report.outcome), report.txn_dir)
            }
            Err(ApplyError::Interrupted { txn_dir, .. }) => {
                let recovered = recover(&ws.state).expect("recover");
                assert_eq!(recovered.transactions.len(), 1, "{context}");
                assert_eq!(
                    recovered.transactions[0].action,
                    RecoveryAction::RolledBack,
                    "{context}: {recovered:?}"
                );
                assert_eq!(disk_state(ws, &case.original), case.original, "{context}");
                assert_eq!(ws.files(), files_of(&case.original), "{context}");
                ("Recovered".to_string(), txn_dir)
            }
            Err(other) => panic!("unexpected error {other:?}: {context}"),
        };
        // Every byte sequence that existed is on disk or in the transaction directory.
        let staged: Vec<Vec<u8>> = all_files(&txn_dir).into_iter().map(|(_, b)| b).collect();
        for state in [&case.original, &case.proposed] {
            for (path, bytes) in state {
                if let Some(bytes) = bytes {
                    let on_disk = ws.read(path).as_ref() == Some(bytes);
                    assert!(
                        on_disk || staged.contains(bytes),
                        "{path}: {bytes:?} lost: {context}"
                    );
                }
            }
        }
        // Nothing unfinished is left behind.
        assert!(
            recover(&ws.state).expect("recover").transactions.is_empty(),
            "{context}"
        );
        *outcomes.entry(label).or_default() += 1;
    }
    // The seeds exercise every path (printed with --nocapture).
    println!("outcomes over the fixed seeds: {outcomes:?}");
    for label in ["Applied", "RolledBack", "PartiallyApplied", "Recovered"] {
        assert!(
            outcomes.get(label).copied().unwrap_or(0) > 0,
            "no case ended {label}: {outcomes:?}"
        );
    }
}
