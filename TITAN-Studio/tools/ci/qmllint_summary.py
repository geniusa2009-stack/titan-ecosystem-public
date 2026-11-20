#!/usr/bin/env python3
"""Summarises qmllint JSON output (CMake target all_qmllint_json). Original TITAN code.

usage: qmllint_summary.py <titan-studio_qmllint.json> [--summary <file>] [--json-out <file>]

Fails only on 'critical' diagnostics (syntax errors, unresolvable imports). Warnings are reported
with counts per category; they are pre-existing style findings and do not gate CI.
"""
import collections
import json
import sys
from pathlib import Path


def main():
    path = Path(sys.argv[1])
    summary = sys.argv[sys.argv.index("--summary") + 1] if "--summary" in sys.argv else None
    json_out = sys.argv[sys.argv.index("--json-out") + 1] if "--json-out" in sys.argv else None
    data = json.loads(path.read_text())
    count, critical = collections.Counter(), []
    for f in data.get("files", []):
        if not f.get("success", True) and not f.get("warnings"):
            critical.append(f"{f.get('filename')}: qmllint could not process the file")
        for w in f.get("warnings", []):
            count[(w.get("type", "?"), w.get("id", "?"))] += 1
            if w.get("type") == "critical":
                critical.append(f"{Path(f.get('filename', '?')).name}:{w.get('line')}: {w.get('message')}")
    files = len(data.get("files", []))
    warnings = sum(v for (t, _), v in count.items() if t == 'warning')
    lines = ["## QML lint (qmllint)", "",
             f"{files} QML files checked; {len(critical)} critical, "
             f"{warnings} warnings (not gating).", "",
             "| Type | Category | Count |", "|---|---|---|"]
    for (t, i), v in sorted(count.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"| {t} | `{i}` | {v} |")
    lines += [""] + [f"- ❌ {c}" for c in critical]
    text = "\n".join(lines) + "\n"
    print(text)
    if json_out:
        Path(json_out).write_text(json.dumps({"files": files, "critical": len(critical), "warnings": warnings,
                                              "byCategory": {f"{t}/{i}": v for (t, i), v in count.items()}}, indent=1))
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(text)
    return 1 if critical or files == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
