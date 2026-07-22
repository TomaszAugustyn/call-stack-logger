#!/usr/bin/env bash
# Recapture the real terminal outputs and trace files that render.py embeds
# in the GIF. Run this when the codebase (demo output, CMake output, trace
# format) has changed, then run render.py.
#
# Uses the repo's build/ directory. It reconfigures it twice (default, then
# LOG_ELAPSED=ON) and restores -DLOG_ELAPSED=OFF at the end. Any other cached
# CMake options in build/ are preserved by CMake.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IN="$HERE/inputs"
B="$ROOT/build"
mkdir -p "$IN"

# Act 1: default configure + full rebuild + run (clean first so the GIF shows
# real "Building CXX object ..." lines, not just "Built target").
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=OFF > "$IN/act1-cmake.txt" 2>&1
cmake --build "$B" --target clean > /dev/null
rm -f "$B/trace.out"
cmake --build "$B" --target run > "$IN/act1-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act2-trace.txt"

# Act 3: LOG_ELAPSED reconfigure + rebuild + run.
rm -f "$B/trace.out"
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=ON > "$IN/act3-cmake.txt" 2>&1
cmake --build "$B" --target run > "$IN/act3-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act4-trace.txt"

# Restore the build dir to the default configuration.
rm -f "$B/trace.out"
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=OFF > /dev/null 2>&1

echo "Inputs refreshed in $IN:"
ls -la "$IN"
