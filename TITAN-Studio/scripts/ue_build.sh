#!/usr/bin/env bash
# Monitored build steps for the TITAN fork of Unreal Engine 5.8.4 on macOS (Apple Silicon).
#
#   scripts/ue_build.sh deps    # macOS-only GitDependencies (no download cache)
#   scripts/ue_build.sh plan    # build UBT, evaluate target/module/plugin rules (-SkipBuild, no compile)
#   scripts/ue_build.sh bridge-check  # compile only the TITAN plugin + project sources (fast feedback)
#   scripts/ue_build.sh build   # TitanHostEditor (project-scoped UnrealEditor), Development, no debug info
#   scripts/ue_build.sh status  # free space, running steps, last log lines
#
# Every step:
#   * runs tools/ue_build_preflight.py --stage <step> first and refuses to start on FAIL; each stage is
#     budgeted for what it writes (dependencies, UBT, the TITAN modules, or the whole editor) on every
#     storage pool it touches (engine tree, derived data / Zen, temporary directory, swap),
#   * writes its full, unfiltered output to $TITAN_UE_LOGS/<UTC timestamp>-<step>.log,
#   * samples free space of every affected pool and swap every 15 s into <log>.metrics.csv,
#   * is stopped (SIGTERM, then SIGKILL) if any affected pool drops below --floor-gb (35, cannot be lowered).
# Derived data is budgeted where Unreal itself will write it (the preflight follows UE 5.8.4's own
# resolution: UE-ZenDataPath, UE-LocalDataCachePath, KeyValueStore.ini, else
# ~/Library/Application Support/Epic/UnrealEngine/Common/Zen/Data). There is deliberately no
# TITAN-only override: to move the cache, set Unreal's own variables so the engine moves it too.
#
# Reviewed Epic scripts this calls: Engine/Build/BatchFiles/RunUBT.sh (returns UBT's real exit code;
# Mac/Build.sh maps exit codes 2/254/255 to success, so it is not used here) and the GitDependencies
# binary directly (Setup.sh would also rewrite .git/hooks; FixDependencyFiles.sh is a no-op on
# case-insensitive APFS apart from copying a Services app into ~/Library, which TITAN does not need).
set -euo pipefail

TITAN_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${TITAN_UE_ENGINE:-$HOME/TitanEngine/UE-5.8.4}"
PROJECT="${TITAN_UE_PROJECT:-$HOME/TitanEngine/TitanHost/TitanHost.uproject}"
LOGS="${TITAN_UE_LOGS:-$HOME/TitanEngine/logs}"
MIN_FLOOR_GB=35
FLOOR_GB=$MIN_FLOOR_GB
JOBS=3
# GitDependencies' per-request HTTP timeout is (largest pack / 250 kB/s) x multiplier x threads, and each
# request must deliver the whole pack in that time (HttpClient.GetAsync reads the full body). With 12
# threads and the 303 MB pack that is ~4 h, which let a stalled connection sit at 0.00 MiB/s for hours
# (log 20261105T231605Z-deps.log). 4 x 0.25 (~20 min) proved too short on this link: three ~300 MB packs
# shared 150-400 KB/s and every attempt timed out (log 20261106T173840Z-deps.log). 4 x 0.75 gives ~60 min
# for the largest pack (completes at >= 85 KB/s per connection); GitDependencies retries each pack 4 times.
THREADS=4
HTTP_TIMEOUT_MULTIPLIER=0.75
EXCLUDES="$TITAN_ROOT/unreal/BuildConfig/gitdeps-macos.exclude"

step="${1:-}"; shift || true
while [[ $# -gt 0 ]]; do
  case "$1" in
    --engine) ENGINE="$2"; shift 2 ;;
    --project) PROJECT="$2"; shift 2 ;;
    --floor-gb) FLOOR_GB="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --http-timeout-multiplier) HTTP_TIMEOUT_MULTIPLIER="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
if ! [[ "$FLOOR_GB" =~ ^[0-9]+$ ]] || (( FLOOR_GB < MIN_FLOOR_GB )); then
  echo "--floor-gb must be a whole number of at least $MIN_FLOOR_GB (got $FLOOR_GB)" >&2; exit 2
fi
mkdir -p "$LOGS"
GUARD_PATHS=("$ENGINE")

# One Unreal step at a time: a second GitDependencies or UBT run against the same tree would corrupt it
# and double the disk and memory load. Exit 4 when another run is active:
#   1. an exclusive flock(2) on $TITAN_UE_LOGS/ue_build.flock, taken atomically by the kernel and held on fd 9
#      by this script for its whole life. The kernel drops it when the holder exits or is killed, so there
#      is no stale lock to reclaim and no reclaim race. The file itself is never removed. Children run
#      with fd 9 closed so a lingering tool (clang, a .NET build server) cannot keep the lock.
#   2. a live <log>.pid (also written by older copies of this script, which did not take the lock),
#   3. any GitDependencies/UBT process of this engine (covers a step whose script was killed).
if [[ -n "$step" && "$step" != status ]]; then
  LOCK="$LOGS/ue_build.flock"
  exec 9<>"$LOCK"
  if ! perl -MFcntl=:flock -e 'open(my $f, ">&=", 9) or die "fd 9: $!\n"; exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)'; then
    echo "another ue_build.sh run holds $LOCK (pid $(cat "$LOCK" 2>/dev/null)); refusing to start $step" >&2; exit 4
  fi
  echo "$$" > "$LOCK"   # informational; a new open file description does not affect the flock
  for p in "$LOGS"/*.pid; do
    [[ -e "$p" ]] || continue
    if kill -0 "$(cat "$p" 2>/dev/null || echo 0)" 2>/dev/null; then
      echo "another step is still running: ${p%.pid} (pid $(cat "$p")); refusing to start $step" >&2; exit 4
    fi
  done
  busy=$(pgrep -f -- "$ENGINE/Engine/(Binaries/DotNET|Build/BatchFiles)" 2>/dev/null 9>&- || true)
  if [[ -n "$busy" ]]; then
    echo "engine tool already running for $ENGINE (pid $(echo $busy)); refusing to start $step" >&2; exit 4
  fi
fi

# free_kb <path>: available KB on the pool holding <path>. TITAN_TEST_GUARD_FREE_KB (tests only) can
# only lower the figure, never raise it.
free_kb() {
  local f; f=$(df -k "$1" | awk 'NR==2 {print $4}')
  if [[ -n "${TITAN_TEST_GUARD_FREE_KB:-}" && "$TITAN_TEST_GUARD_FREE_KB" -lt "$f" ]]; then f=$TITAN_TEST_GUARD_FREE_KB; fi
  echo "$f"
}

preflight() {  # <json-out> <stage> <log>: fills GUARD_PATHS with one path per pool the stage writes to
  local out="$1" stage="$2" log="$3" rc=0
  local args=(--engine "$ENGINE" --exclude-file "$EXCLUDES" --stage "$stage" --headroom-gb "$FLOOR_GB"
              --out "$out" --guard-out "$out.guard")
  [[ -n "${TITAN_TEST_SIMULATE_FREE_GB:-}" ]] && args+=(--simulate-free-gb "$TITAN_TEST_SIMULATE_FREE_GB")
  set +e
  python3 -I "$TITAN_ROOT/tools/ue_build_preflight.py" "${args[@]}" 2>&1 9>&- | tee -a "$log"
  rc=${PIPESTATUS[0]}
  set -e
  if [[ $rc -ne 0 ]]; then echo "# TITAN preflight ($stage) refused to start: exit $rc" | tee -a "$log"; return "$rc"; fi
  GUARD_PATHS=()
  while IFS= read -r g; do [[ -n "$g" ]] && GUARD_PATHS+=("$g"); done < "$out.guard"
  [[ ${#GUARD_PATHS[@]} -gt 0 ]] || GUARD_PATHS=("$ENGINE")
}

# run_guarded <log> <cmd...>: run cmd in its own process group with the disk floor enforced.
run_guarded() {
  local log="$1"; shift
  local metrics="$log.metrics.csv" floor_kb=$((FLOOR_GB * 1000000))
  echo "utc,free_kb,swap_used,min_free_path" > "$metrics"
  {
    echo "# TITAN ue_build.sh $step  $(date -u +%FT%TZ)"
    echo "# engine=$ENGINE"
    echo "# guarded pools (floor ${FLOOR_GB} GB each): ${GUARD_PATHS[*]}"
    echo "# cmd: $*"
  } >> "$log"
  # perl setpgrp gives the step its own process group so the guard can stop every child.
  # .NET tools (GitDependencies, UBT) parse numbers with the current culture: under this Mac's ar_EG
  # locale GitDependencies rejects "--http-timeout-multiplier=0.25" (measured), so run them as en_US.
  # fd 9 (the run lock) is closed for the step so only this script holds the lock.
  LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 perl -e 'setpgrp(0,0); exec @ARGV or die "exec: $!"' "$@" >> "$log" 2>&1 9>&- &
  local pid=$! stopped="" i
  echo "$pid" > "$log.pid"
  # Wait until perl has made the step its own process-group leader, so a guard stop reaches every child.
  for i in $(seq 50); do
    [[ "$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')" == "$pid" ]] && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  while kill -0 "$pid" 2>/dev/null; do
    local f="" fp="" g gf
    for g in "${GUARD_PATHS[@]}"; do
      gf=$(free_kb "$g")
      if [[ -z "$f" || "$gf" -lt "$f" ]]; then f=$gf; fp=$g; fi
    done
    echo "$(date -u +%FT%TZ),$f,$(sysctl -n vm.swapusage | awk '{print $6}'),\"$fp\"" >> "$metrics"
    if [[ "$f" -lt "$floor_kb" ]]; then
      stopped="disk floor ${FLOOR_GB} GB reached on $fp (free ${f} KB)"
      echo "# TITAN GUARD: $stopped; stopping process group $pid" >> "$log"
      kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
      for i in $(seq 20); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
      kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
      break
    fi
    for i in $(seq "${TITAN_TEST_GUARD_INTERVAL:-15}"); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  done
  local rc=0
  wait "$pid" || rc=$?
  rm -f "$log.pid"
  echo "# TITAN exit=$rc ${stopped:+guard=\"$stopped\"} $(date -u +%FT%TZ)" >> "$log"
  [[ -n "$stopped" ]] && return 75
  return "$rc"
}

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
case "$step" in
  deps)
    log="$LOGS/$stamp-deps.log"
    preflight "$LOGS/$stamp-preflight.json" deps "$log" || exit 3
    bin="$ENGINE/Engine/Binaries/DotNET/GitDependencies/osx-arm64"
    # Same quarantine handling as Engine/Build/BatchFiles/Mac/GitDependencies.sh.
    xattr -d com.apple.quarantine "$bin/GitDependencies" "$bin"/*.dylib 2>/dev/null || true
    args=(--force --no-cache "--threads=$THREADS" "--http-timeout-multiplier=$HTTP_TIMEOUT_MULTIPLIER" "--root=$ENGINE")
    while IFS= read -r line; do
      [[ -z "$line" || "$line" == \#* ]] && continue
      args+=("--exclude=$line")
    done < "$EXCLUDES"
    run_guarded "$log" "$bin/GitDependencies" "${args[@]}"
    ;;
  build)
    [[ -f "$PROJECT" ]] || { echo "project not found: $PROJECT (run scripts/sync_ue_plugins.sh)" >&2; exit 2; }
    log="$LOGS/$stamp-build.log"
    preflight "$LOGS/$stamp-preflight.json" build "$log" || exit 3
    # ShaderCompileWorker is a separate program target that the editor launches to compile shaders; the
    # editor target does not build it, and without it UnrealEditor quits at startup ("Couldn't launch
    # .../ShaderCompileWorker", log 20261109T135534Z-conformance.TitanHost.log). Build both in one UBT run.
    run_guarded "$log" "$ENGINE/Engine/Build/BatchFiles/RunUBT.sh" \
      "-Target=TitanHostEditor Mac Development -Project=$PROJECT" "-Target=ShaderCompileWorker Mac Development" \
      -NoDebugInfo "-MaxParallelActions=$JOBS" -WaitMutex -NoHotReloadFromIDE
    ;;
  plan)
    # Build UBT, then evaluate the TitanHostEditor target, module and plugin rules and the dependency
    # graph without compiling anything (-SkipBuild). Catches rules/descriptor errors early.
    [[ -f "$PROJECT" ]] || { echo "project not found: $PROJECT (run scripts/sync_ue_plugins.sh)" >&2; exit 2; }
    log="$LOGS/$stamp-plan.log"
    preflight "$LOGS/$stamp-preflight.json" plan "$log" || exit 3
    run_guarded "$log" "$ENGINE/Engine/Build/BatchFiles/RunUBT.sh" TitanHostEditor Mac Development \
      "-Project=$PROJECT" -NoDebugInfo -WaitMutex -NoHotReloadFromIDE -SkipBuild
    ;;
  bridge-check)
    # Compile only the TITAN sources (bridge plugin + host project) and the PCHs they need: fast compile
    # feedback against the real UE 5.8.4 headers before the multi-hour full build.
    [[ -f "$PROJECT" ]] || { echo "project not found: $PROJECT (run scripts/sync_ue_plugins.sh)" >&2; exit 2; }
    log="$LOGS/$stamp-bridge-check.log"
    # Only UHT output + the PCHs the TITAN modules use + ~10 object files: estimate 6 GB, no shaders.
    preflight "$LOGS/$stamp-preflight.json" bridge-check "$log" || exit 3
    files=()
    while IFS= read -r f; do files+=("-SingleFile=$f"); done < <(
      find "$ENGINE/Engine/Plugins/TITAN" "$(dirname "$PROJECT")/Source" -name '*.cpp' | sort)
    run_guarded "$log" "$ENGINE/Engine/Build/BatchFiles/RunUBT.sh" TitanHostEditor Mac Development \
      "-Project=$PROJECT" -NoDebugInfo "-MaxParallelActions=$JOBS" -WaitMutex -NoHotReloadFromIDE "${files[@]}"
    ;;
  status)
    for g in "$ENGINE" "$HOME/Library/Application Support/Epic" "$(getconf DARWIN_USER_TEMP_DIR)"; do
      [[ -e "$g" ]] && echo "free:$(( $(free_kb "$g") / 1000000 )) GB on $(df -k "$g" | awk 'NR==2 {print $1}') ($g)"
    done
    echo "swap: $(sysctl -n vm.swapusage)"
    for p in "$LOGS"/*.pid; do [[ -e "$p" ]] && echo "running: ${p%.pid} (pid $(cat "$p"))"; done
    last=$(ls -t "$LOGS"/*.log 2>/dev/null | head -1 || true)
    [[ -n "$last" ]] && { echo "last log: $last"; tail -5 "$last"; }
    ;;
  *) sed -n '2,8p' "$0"; exit 2 ;;
esac
