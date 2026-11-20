#!/usr/bin/env bash
# TITAN: configure, build and test. Usage: scripts/build_and_test.sh [--headless]
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j "${TITAN_JOBS:-4}"
if [[ "${1:-}" == "--headless" ]]; then
  ctest --test-dir build -LE gui --output-on-failure
else
  ctest --test-dir build --output-on-failure
fi
