#!/usr/bin/env python3
"""Accounts for every TITAN baseline suite in the CTest JUnit results of each CI job. Original TITAN code.

usage: ctest_report.py --baseline tools/ci/baseline-suites.json --results <dir> [--summary <file>]
                       [--jobs core,studio] [--label core="macOS 15"] [--only REGEX] [--allow-unreal]

<dir> holds, per job, `junit-<job>.xml` (ctest --output-junit), optionally `notrun-<job>.json`
({"suite": "reason"}) for suites the job deliberately did not run, and optionally `qmllint-<job>.json`
(written by qmllint_summary.py).

Per job, each expected suite is: PASS, FAIL, SKIPPED (CTest exit 77), NOT RUN (not registered or
deliberately excluded, with a reason) or MISSING. The run fails on FAIL or MISSING, and on SKIPPED /
NOT RUN unless the baseline's 'requires' explains it. With --allow-unreal (the gated self-hosted
workflow) Unreal suites must actually pass. Skipped and not-run suites are never counted as passed.
"""
import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

CATEGORY = {
    "unreal-source": "Not available: Unreal source absent (self-hosted Mac only)",
    "unreal-editor": "Requires self-hosted Mac with a built Unreal Editor",
    "gpu-session": "Requires a Metal device and a GUI session",
}


def load_junit(path):
    out = {}
    for tc in ET.parse(path).getroot().iter("testcase"):
        name, secs = tc.get("name"), float(tc.get("time") or 0)
        if tc.find("failure") is not None or tc.find("error") is not None or tc.get("status") == "fail":
            out[name] = ("FAIL", secs, "")
        elif tc.find("skipped") is not None or tc.get("status") in ("notrun", "disabled"):
            first = (tc.findtext("system-out") or "").strip().splitlines()
            out[name] = ("SKIPPED", secs, first[0] if first else "")
        else:
            out[name] = ("PASS", secs, "")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--summary")
    ap.add_argument("--jobs", default="core,studio")
    ap.add_argument("--label", action="append", default=[], help="job=column title")
    ap.add_argument("--only", help="account only for baseline suites matching this regex (expected in every --jobs job)")
    ap.add_argument("--allow-unreal", action="store_true")
    a = ap.parse_args()

    suites = json.loads(Path(a.baseline).read_text())["suites"]
    jobs = [j for j in a.jobs.split(",") if j]
    labels = dict(kv.split("=", 1) for kv in a.label)
    res, notrun, lint = {}, {}, {}
    for job in jobs:
        jp = Path(a.results) / f"junit-{job}.xml"
        res[job] = load_junit(jp) if jp.exists() else None
        np_ = Path(a.results) / f"notrun-{job}.json"
        notrun[job] = json.loads(np_.read_text()) if np_.exists() else {}
        lp = Path(a.results) / f"qmllint-{job}.json"
        if lp.exists():
            lint[job] = json.loads(lp.read_text())

    rows, bad = [], []
    for name, spec in suites.items():
        if a.only and not re.search(a.only, name):
            continue
        expected = jobs if a.only else [j for j in spec["jobs"] if j in jobs]
        if not expected:
            continue
        req, why = spec.get("requires"), spec.get("why", "")
        tolerated = req is not None and not (a.allow_unreal and req.startswith("unreal"))
        cells, notes = {}, []
        for job in expected:
            r = res[job]
            if r is None:
                state, note = "MISSING", f"job '{job}' produced no results"
            elif name in r:
                state, secs, note = r[name]
                if state == "PASS":
                    note = f"{secs:.2f} s"
            elif name in notrun[job]:
                state, note = "NOT RUN", notrun[job][name]
            elif req == "unreal-source":
                state, note = "NOT RUN", f"not registered: {why}"
            else:
                state, note = "MISSING", "not registered by CMake"
            if state == "FAIL" or state == "MISSING" or (state in ("SKIPPED", "NOT RUN") and not tolerated):
                bad.append(f"{name}@{job}")
            cells[job] = state
            if state != "PASS":
                notes.append((labels.get(job, job), note))
        states = set(cells.values())
        if states == {"PASS"}:
            cat = "Passed"
        elif "FAIL" in states:
            cat = "Failed"
        elif "MISSING" in states:
            cat = "Missing"
        elif "PASS" in states:
            cat = "Partly run: " + CATEGORY.get(req, "see note")
        elif req in CATEGORY and not (a.allow_unreal and req.startswith("unreal")):
            cat = CATEGORY[req]
        else:
            cat = "Skipped" if "SKIPPED" in states else "Not run"
        texts = {n for _, n in notes}
        note_text = next(iter(texts)) if len(texts) == 1 else "; ".join(f"{j}: {n}" for j, n in notes)
        rows.append((name, cells, cat, note_text))

    order = ["Passed", "Failed", "Missing"]
    counts = {}
    for _, _, cat, _ in rows:
        counts[cat] = counts.get(cat, 0) + 1
    icon = {"PASS": "✅ pass", "FAIL": "❌ FAIL", "SKIPPED": "⏭️ skipped", "NOT RUN": "⏸️ not run", "MISSING": "❌ MISSING"}
    lines = ["## TITAN test results", "", f"**{len(rows)} baseline suites**", ""]
    for cat in order + sorted(c for c in counts if c not in order):
        if counts.get(cat):
            lines.append(f"- {cat}: **{counts[cat]}**")
    for job in jobs:
        r = res[job]
        if r is not None:
            n = {s: sum(1 for v in r.values() if v[0] == s) for s in ("PASS", "FAIL", "SKIPPED")}
            lines.append(f"- {labels.get(job, job)}: {len(r)} executed by CTest — {n['PASS']} passed, "
                         f"{n['FAIL']} failed, {n['SKIPPED']} skipped (exit 77)")
    for job, l in lint.items():
        lines.append(f"- QML lint ({labels.get(job, job)}): {l['files']} files, **{l['critical']} critical**, "
                     f"{l['warnings']} warnings (warnings do not gate)")
    lines += ["", "> Skipped and not-run suites are **not** evidence that the Unreal integration works; they run only in "
              "the gated *TITAN Unreal* workflow on a self-hosted Mac that holds the private engine.", ""]
    head = "| Suite | " + " | ".join(labels.get(j, j) for j in jobs) + " | Category | Note |"
    lines += [head, "|" + "---|" * (len(jobs) + 3)]
    for name, cells, cat, note in rows:
        cols = " | ".join(icon[cells[j]] if j in cells else "—" for j in jobs)
        lines.append(f"| `{name}` | {cols} | {cat} | {note.replace('|', '/')} |")
    seen = set().union(*[set(r) for r in res.values() if r])
    extra = sorted(seen - set(suites))
    if extra:
        lines += ["", "Suites not yet in `tools/ci/baseline-suites.json`: " + ", ".join(f"`{e}`" for e in extra)]
    text = "\n".join(lines) + "\n"
    print(text)
    if a.summary:
        with open(a.summary, "a", encoding="utf-8") as f:
            f.write(text)
    if bad:
        print("FAILED or unaccounted: " + ", ".join(bad), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
