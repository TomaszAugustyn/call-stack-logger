#!/bin/bash
# Builds and runs the cost benchmark against one checkout, in every option
# combination the README's cost table lists, and prints one table.
#
#   bench/run_bench.sh <checkout> [label] [iterations]
#
# Example — compare master with a branch on the same machine:
#   bench/run_bench.sh ~/projects/call-stack-logger master
#   bench/run_bench.sh ~/projects/my-branch branch
#
# Each configuration is built in /tmp/cslg-bench-<label>-<name> (reused when it
# exists) and run three times; the median of each measurement is printed. The
# trace goes to /dev/null. Needs CMake, a C++17 compiler and the dependency
# downloads the library makes at configure time (or
# FETCHCONTENT_SOURCE_DIR_LIBBACKTRACE in the environment for offline use).
set -eu
checkout=$(cd "$1" && pwd)
label=${2:-$(basename "$checkout")}
iterations=${3:-50}
bench_dir=$(cd "$(dirname "$0")" && pwd)
extra_cmake=${CSLG_BENCH_CMAKE_ARGS:-}
if [ -n "${FETCHCONTENT_SOURCE_DIR_LIBBACKTRACE:-}" ]; then
    extra_cmake="$extra_cmake -DFETCHCONTENT_SOURCE_DIR_LIBBACKTRACE=$FETCHCONTENT_SOURCE_DIR_LIBBACKTRACE"
fi

# name  build type      options
configs="
O0-default        Debug           -DLOG_EXCEPTIONS=OFF -DLOG_ELAPSED=OFF
O0-exceptions     Debug           -DLOG_EXCEPTIONS=ON  -DLOG_ELAPSED=OFF
O0-elapsed        Debug           -DLOG_EXCEPTIONS=OFF -DLOG_ELAPSED=ON
O0-both           Debug           -DLOG_EXCEPTIONS=ON  -DLOG_ELAPSED=ON
O2-default        RelWithDebInfo  -DLOG_EXCEPTIONS=OFF -DLOG_ELAPSED=OFF
O2-exceptions     RelWithDebInfo  -DLOG_EXCEPTIONS=ON  -DLOG_ELAPSED=OFF
O2-elapsed        RelWithDebInfo  -DLOG_EXCEPTIONS=OFF -DLOG_ELAPSED=ON
O2-both           RelWithDebInfo  -DLOG_EXCEPTIONS=ON  -DLOG_ELAPSED=ON
"

median() { sort -g | awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }'; }

printf '%-16s %-8s %-10s %-12s %-12s %-12s %-10s %s\n' "config ($label)" "warm ns" "cold us" "1st lib A us" "1st lib B us" "RSS after kB" "peak kB" "(per traced call / per new function / first call into each library)"
echo "$configs" | while read -r name type opts; do
    [ -n "$name" ] || continue
    build=/tmp/cslg-bench-$label-$name
    # shellcheck disable=SC2086
    cmake -S "$bench_dir" -B "$build" -DCSLG_SOURCE_DIR="$checkout" -DCMAKE_BUILD_TYPE=$type $opts $extra_cmake > "$build.cmake.log" 2>&1 \
        || { echo "$name: configure failed, see $build.cmake.log"; continue; }
    cmake --build "$build" -j"$(nproc)" >> "$build.cmake.log" 2>&1 \
        || { echo "$name: build failed, see $build.cmake.log"; continue; }
    rm -f "$build.runs"
    for run in 1 2 3; do
        CSLG_OUTPUT_FILE=/dev/null "$build/cslg_bench" "$iterations" >> "$build.runs"
    done
    field() { grep "^$1=" "$build.runs" | cut -d= -f2 | median; }
    printf '%-16s %-8s %-10s %-12s %-12s %-12s %-10s\n' "$name" "$(field warm_per_call_ns)" "$(field cold_per_new_function_us)" \
        "$(field first_call_into_library_a_us)" "$(field first_call_into_library_b_us)" "$(field rss_after_kb)" "$(field rss_peak_kb)"
done
