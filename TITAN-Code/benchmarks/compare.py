#!/usr/bin/env python3
"""Compare two titan-bench reports produced on the same machine in the same CI job.

Usage: compare.py BASE.json NEW.json [--threshold 1.5] [--min-ns 1000] [--budgets FILE]

Scenarios present in both reports are compared by median and p95. A median above
base * --threshold (for base medians of at least --min-ns; timer noise dominates below)
is reported as a GitHub warning annotation. Ratios never fail the build: single runs on
shared runners are too noisy for that.

The build fails only on --budgets: absolute, user-facing latency limits (see
benchmarks/budgets.json, each with its justification). A required budget whose scenario is
missing from NEW also fails.
"""
import json
import os
import sys


def load(path):
    with open(path) as f:
        report = json.load(f)
    return report, {(r["dataset"], r["scenario"]): r for r in report["results"]}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = dict(zip(argv[1:], argv[2:]))
    threshold = float(opts.get("--threshold", 1.5))
    budgets_path = opts.get("--budgets")
    min_ns = float(opts.get("--min-ns", 1000))
    if len(args) < 2:
        print(__doc__)
        return 2
    base_report, base = load(args[0])
    new_report, new = load(args[1])

    lines = [
        f"Baseline {base_report.get('gitCommit') or args[0]} vs new {new_report.get('gitCommit') or args[1]}",
        f"threshold {threshold}x on median, ignoring base medians < {min_ns:.0f} ns",
        "",
        "| dataset | scenario | base median ns | new median ns | ratio | base p95 | new p95 | allocs/op (new) |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    regressions = []
    for key in sorted(set(base) & set(new)):
        b, n = base[key], new[key]
        ratio = n["median"] / b["median"] if b["median"] else float("nan")
        flag = ""
        if b["median"] >= min_ns and ratio > threshold:
            flag = " **slower**"
            regressions.append((key, ratio))
        allocs = n.get("allocationsPerOp")
        allocs = f"{allocs:.2f}" if isinstance(allocs, (int, float)) else "-"
        lines.append(
            f"| {key[0]} | {key[1]} | {b['median']} | {n['median']} | {ratio:.2f}{flag} | {b['p95']} | {n['p95']} | {allocs} |"
        )
    only_new = sorted(set(new) - set(base))
    if only_new:
        lines += ["", "New scenarios (no baseline):", ""]
        lines += [
            f"- {d} / {s}: median {new[(d, s)]['median']} ns, p95 {new[(d, s)]['p95']} ns, "
            f"allocs/op {new[(d, s)].get('allocationsPerOp', '-')}"
            for d, s in only_new
        ]
    failures = []
    if budgets_path:
        with open(budgets_path) as f:
            budgets = json.load(f)["budgets"]
        lines += ["", "Latency budgets (fail the build when exceeded):", "",
                  "| dataset | scenario | metric | value ns | budget ns | result |", "|---|---|---|---:|---:|---|"]
        for budget in budgets:
            key = (budget["dataset"], budget["scenario"])
            row = new.get(key)
            metric = budget["metric"]
            if row is None or row.get(metric) is None:
                required = budget.get("required", True)
                lines.append(f"| {key[0]} | {key[1]} | {metric} | missing | {budget['maxNs']} | {'FAIL' if required else 'skipped (optional)'} |")
                if required:
                    failures.append(f"{key[0]}/{key[1]}: scenario missing")
                continue
            value = row[metric]
            ok = value <= budget["maxNs"]
            lines.append(f"| {key[0]} | {key[1]} | {metric} | {value} | {budget['maxNs']} | {'ok' if ok else 'FAIL'} |")
            if not ok:
                failures.append(f"{key[0]}/{key[1]} {metric} {value} ns > {budget['maxNs']} ns ({budget['why']})")
    text = "\n".join(lines)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write("## Benchmark comparison\n\n" + text + "\n")
    for (d, sc), r in regressions:
        print(f"::warning title=slower than baseline::{d}/{sc} median {r:.2f}x the baseline")
    for failure in failures:
        print(f"::error title=latency budget exceeded::{failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
