#!/usr/bin/env bash
# Sync TITAN's Unreal sources (source of truth: this repository) into the engine working copy:
#   unreal/Plugins/<P>  -> <engine>/Engine/Plugins/TITAN/<P>   (commit on the engine's titan/engine branch)
#   unreal/TitanHost    -> <engine>/../TitanHost               (host project, next to the engine fork)
# With --check, only report drift (exit 1 if anything differs).
# Usage: scripts/sync_ue_plugins.sh [--check] [engine_root]
set -euo pipefail
cd "$(dirname "$0")/.."
check=0
if [[ "${1:-}" == "--check" ]]; then check=1; shift; fi
engine="${1:-../engine/UE-5.8.4}"
if [[ ! -f "$engine/Engine/Build/Build.version" ]]; then
  echo "engine working copy not found: $engine" >&2; exit 2
fi
engine_real="$(cd "$engine" && pwd -P)"
dest="$engine_real/Engine/Plugins/TITAN"
project_dest="$(dirname "$engine_real")/TitanHost"
# Build products and caches that Unreal writes inside these folders are not sources.
excludes=(--exclude Binaries --exclude Intermediate --exclude Saved --exclude DerivedDataCache --exclude Content)

sync_dir() {  # <src> <dst> <label>
  if (( check )); then
    diff -r -x Binaries -x Intermediate -x Saved -x DerivedDataCache -x Content "$1" "$2" >/dev/null 2>&1 \
      || { echo "DRIFT: $3 differs from $2"; exit 1; }
    echo "in sync: $3"
  else
    mkdir -p "$2"
    # Compare by content and do NOT copy modification times (no -a / -t): a file that changes gets the
    # current time, so UBT always sees it as newer than objects built from the previous content. With
    # rsync -a an edit made in the repo before the last engine build kept that older time, and UBT
    # reported "Target is up to date" with a stale TitanStudioBridge (log 20261109T132033Z-build.log).
    # Unchanged files are skipped (--checksum), so a sync alone never triggers a rebuild.
    rsync -rlpD --checksum --delete "${excludes[@]}" "$1/" "$2/"
    echo "synced: $3 -> $2"
  fi
}

for plugin in unreal/Plugins/*/; do
  name="$(basename "$plugin")"
  sync_dir "${plugin%/}" "$dest/$name" "$name"
done
sync_dir unreal/TitanHost "$project_dest" TitanHost
