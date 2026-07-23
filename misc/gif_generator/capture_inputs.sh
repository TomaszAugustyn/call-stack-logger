#!/usr/bin/env bash
# Recapture the real terminal outputs and trace files that render.py embeds
# in the GIF. Run this when the codebase (demo output, CMake output, trace
# format) has changed, then run render.py.
#
# The GIF's first build happens at a story moment when the fibonacci function
# does not exist yet, so act1/act2 are captured from an INTERMEDIATE main.cpp
# (the real file minus the fibonacci function and its call). The file is
# temporarily swapped in src/ and restored afterwards (also on interrupt).
#
# Uses the repo's build/ directory. It reconfigures it twice (default, then
# LOG_ELAPSED=ON) and restores -DLOG_ELAPSED=OFF at the end. Any other cached
# CMake options in build/ are preserved by CMake.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IN="$HERE/inputs"
B="$ROOT/build"
MAIN="$ROOT/src/main.cpp"
BAK="$MAIN.gifgen.bak"
mkdir -p "$IN"

restore_main() { if [ -f "$BAK" ]; then mv "$BAK" "$MAIN"; fi; }
trap restore_main EXIT

# Intermediate main.cpp: drop the fibonacci function (with its leading blank
# line) and its call + comment. KEEP IN SYNC with FIB_*/CALL_* in render.py.
cp "$MAIN" "$BAK"
python3 - "$MAIN" <<'EOF'
import sys
path = sys.argv[1]
lines = open(path).read().split("\n")
del lines[63:65]   # "// Test logging constexpr function" + "fibonacci(6);"
del lines[28:34]   # blank line + constexpr unsigned fibonacci(...) {...}
open(path, "w").write("\n".join(lines))
EOF

# Act 1: default configure + full rebuild + run of the intermediate file
# (clean first so the GIF shows real "Building CXX object ..." lines).
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=OFF > "$IN/act1-cmake.txt" 2>&1
cmake --build "$B" --target clean > /dev/null
rm -f "$B/trace.out"
cmake --build "$B" --target run > "$IN/act1-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act2-trace.txt"

# Back to the full file for the LOG_ELAPSED act.
restore_main

# Act 3: LOG_ELAPSED reconfigure + rebuild + run (full file, with fibonacci).
rm -f "$B/trace.out"
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=ON > "$IN/act3-cmake.txt" 2>&1
cmake --build "$B" --target run > "$IN/act3-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act4-trace.txt"

# Restore the build dir to the default configuration.
rm -f "$B/trace.out"
cmake -S "$ROOT" -B "$B" -DLOG_ELAPSED=OFF > /dev/null 2>&1

echo "Inputs refreshed in $IN:"
ls -la "$IN"
