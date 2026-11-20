#!/usr/bin/env bash
# Engine-side integration tests against the real Unreal Engine 5.8.4 editor (TitanHost project).
#   run_ue_bridge.sh conformance <editor> <uproject> <protocol-check> <workdir>
#   run_ue_bridge.sh automation  <editor> <uproject> <workdir>
# Exit 77 (CTest SKIP) when the editor has not been built, so a missing build is never reported as PASS.
set -uo pipefail
mode="$1"; editor="$2"; project="$3"
if [[ ! -x "$editor" || ! -f "$project" ]]; then
  echo "SKIP: Unreal Editor not built ($editor) or project missing ($project)"; exit 77
fi
# Unreal's first launch writes derived data (shaders) to the same volume: keep the 35 GB safety floor.
FLOOR_KB=$((35 * 1000000))
free_kb() { df -k "$(dirname "$editor")" | awk 'NR==2 {print $4}'; }
if (( $(free_kb) < FLOOR_KB + 5000000 )); then echo "FAIL: less than 40 GB free; not starting Unreal"; exit 1; fi
case "$mode" in
  conformance)
    check="$4"; work="$5"
    rm -rf "$work"; mkdir -p "$work"
    sock="${TMPDIR:-/tmp}/titan-ue-ctest-$$.sock"
    log="$work/unreal-stdout.log"
    t0=$(date +%s)
    "$editor" "$project" "-TitanSocket=$sock" "-TitanParentPid=$$" -TitanMinimizeEditor -nosplash -unattended -NoSound \
      > "$log" 2>&1 &
    pid=$!
    # First launch compiles shaders and can take a long time on an 8 GB M3.
    for ((i = 0; i < 3600; i++)); do
      grep -q "^TITAN_HOST_READY" "$log" && break
      kill -0 "$pid" 2>/dev/null || { echo "FAIL: Unreal exited before the bridge was ready"; tail -40 "$log"; exit 1; }
      if (( $(free_kb) < FLOOR_KB )); then echo "FAIL: disk floor reached; stopping Unreal"; kill "$pid"; exit 1; fi
      sleep 1
    done
    grep -q "^TITAN_HOST_READY" "$log" || { echo "FAIL: bridge not ready after 3600 s"; kill "$pid"; exit 1; }
    echo "Unreal + bridge ready after $(( $(date +%s) - t0 )) s"
    "$check" --socket "$sock" --workdir "$work/check" --viewport --call-timeout 600000 --frame-timeout 900000 \
      --report "$work/conformance-report.json" --shutdown
    rc=$?
    for ((i = 0; i < 120; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then echo "Unreal did not exit after engine.shutdown; terminating"; kill "$pid"; rc=1; fi
    wait "$pid" 2>/dev/null
    for f in "$work"/check/*.bmp; do [[ -e "$f" ]] && sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null 2>&1; done
    exit $rc
    ;;
  automation)
    work="$4"; rm -rf "$work"; mkdir -p "$work"
    "$editor" "$project" -ExecCmds="Automation RunTests TITAN.Bridge;Quit" -unattended -nosplash -NoSound -NullRHI \
      -ReportExportPath="$work" -log > "$work/unreal-stdout.log" 2>&1
    rc=$?
    python3 -I - "$work/index.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding="utf-8-sig"))
except OSError:
    print("FAIL: no automation report"); sys.exit(1)
tests = d.get("tests", [])
for t in tests:
    print(f"[{t.get('state')}] {t.get('fullTestPath')}")
    for e in t.get("entries", []):
        ev = e.get("event", {})
        if ev.get("type") in ("Error", "Warning"): print("   ", ev.get("type"), ev.get("message"))
ok = tests and all(t.get("state") == "Success" for t in tests)
print(f"{len(tests)} automation tests, {'all passed' if ok else 'FAILURES'}")
sys.exit(0 if ok else 1)
PY
    prc=$?
    (( prc == 0 )) || exit 1
    exit $(( rc == 0 ? 0 : 1 ))
    ;;
esac
