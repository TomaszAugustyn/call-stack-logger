#!/usr/bin/env bash
# Recapture the real terminal outputs and trace files that render.py embeds
# in the GIF. Run it after the codebase changed (demo output, CMake output,
# trace format), then run render.py.
#
# Each build in the GIF happens at a moment when main.cpp looks different, so
# the script builds and runs each version that demo_source.py derives from the
# real file, swapping it into src/ for the build:
#   act1/act2  intermediate (no fibonacci yet)  cmake .. && make run
#   act3/act4  the real file                    cmake -DLOG_ELAPSED=ON .. && make run
#   act5/act6  + the exception demo             cmake -DLOG_EXCEPTIONS=ON -DLOG_ELAPSED=ON .. && make run
#
# The commands run in the repo's build/ directory exactly as the GIF types
# them. make runs under `stdbuf -oL`, so the demo's stdout is line-buffered as
# on a terminal: the act5 demo dies in std::terminate, and a fully buffered
# stdout would lose its lines. It also runs under `prlimit --core=1:1`, so the
# abort writes no core dump and wakes no desktop crash reporter. Everything
# runs in an English locale: make translates its error messages into the
# system language, and the rest of the GIF is English.
#
# On exit, also after an error or Ctrl+C, the script puts back the real
# main.cpp, the LOG_* options cached in build/ and any build/trace.out that
# was there, and rebuilds runDemo from the real file.
set -euo pipefail
export LC_ALL=C.UTF-8

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IN="$HERE/inputs"
B="$ROOT/build"
MAIN="$ROOT/src/main.cpp"
MAIN_BAK="$MAIN.gifgen.bak"
TRACE_BAK="$B/trace.out.gifgen.bak"
OPTIONS=(LOG_ADDR LOG_ELAPSED LOG_EXCEPTIONS)

if [ -e "$MAIN_BAK" ]; then
    echo "error: $MAIN_BAK exists, so a previous run was interrupted. Check that" \
         "src/main.cpp is the real file, then delete the backup." >&2
    exit 1
fi
mkdir -p "$IN"
[ -f "$B/CMakeCache.txt" ] || cmake -S "$ROOT" -B "$B" > /dev/null

saved=()
for opt in "${OPTIONS[@]}"; do
    value=$(sed -n "s/^$opt:BOOL=//p" "$B/CMakeCache.txt")
    saved+=("-D$opt=${value:-OFF}")
done

restore() {
    set +e
    [ -f "$MAIN_BAK" ] && mv "$MAIN_BAK" "$MAIN"
    rm -f "$B/trace.out"
    [ -f "$TRACE_BAK" ] && mv "$TRACE_BAK" "$B/trace.out"
    # The demo object was last compiled from a swapped-in main.cpp that is
    # newer than the restored file, so make would keep using it. Delete it.
    rm -f "$B/src/CMakeFiles/runDemo.dir/main.cpp.o" "$B/src/runDemo"
    (cd "$B" && cmake "${saved[@]}" .. && make runDemo) > /dev/null 2>&1 \
        || echo "warning: restoring build/ failed; rerun cmake and make there" >&2
}
trap restore EXIT

cp -p "$MAIN" "$MAIN_BAK"
if [ -f "$B/trace.out" ]; then mv "$B/trace.out" "$TRACE_BAK"; fi

use_version() { python3 "$HERE/demo_source.py" "$1" "$MAIN_BAK" > "$MAIN"; }
in_build() { (cd "$B" && "$@"); }
make_run() { in_build prlimit --core=1:1 -- stdbuf -oL make run; }

# Start from the default configuration and a clean library and demo, so act1
# shows a full build ("Building CXX object ..." for every file).
in_build cmake -DLOG_ADDR=OFF -DLOG_ELAPSED=OFF -DLOG_EXCEPTIONS=OFF .. > /dev/null
make -C "$B/src" clean > /dev/null

# act1/act2: the first build, before fibonacci exists
use_version intermediate
in_build cmake .. > "$IN/act1-cmake.txt" 2>&1
make_run > "$IN/act1-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act2-trace.txt"

# act3/act4: the real file, with per-function durations
use_version full
rm -f "$B/trace.out"
in_build cmake -DLOG_ELAPSED=ON .. > "$IN/act3-cmake.txt" 2>&1
make_run > "$IN/act3-makerun.txt" 2>&1
cp "$B/trace.out" "$IN/act4-trace.txt"

# act5/act6: the exception demo. The exception nobody catches ends the demo
# in std::terminate, so make run fails by design.
use_version exceptions
rm -f "$B/trace.out"
in_build cmake -DLOG_EXCEPTIONS=ON -DLOG_ELAPSED=ON .. > "$IN/act5-cmake.txt" 2>&1
if make_run > "$IN/act5-makerun.txt" 2>&1; then
    echo "error: the exception demo should have terminated, but make run succeeded" >&2
    exit 1
fi
if ! grep -q "terminate called after throwing" "$IN/act5-makerun.txt"; then
    echo "error: act5 failed for another reason, see $IN/act5-makerun.txt" >&2
    exit 1
fi
cp "$B/trace.out" "$IN/act6-trace.txt"

echo "Inputs refreshed in $IN:"
ls -la "$IN"
