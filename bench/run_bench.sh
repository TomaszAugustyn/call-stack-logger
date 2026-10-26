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
# Each configuration is built in <build root>/<label>-<name>, reused when it
# exists, and run three times; the median of each measurement is printed. The
# build root is CSLG_BENCH_BUILD_ROOT, by default ~/.cache/cslg-bench (a
# directory of the user's own: a fixed name under /tmp would let another user
# plant a link where the logs are written). The trace goes to /dev/null.
# Extra CMake arguments come from CSLG_BENCH_CMAKE_ARGS (split at spaces). Needs
# CMake, a C++17 compiler and the dependency downloads the library makes at
# configure time (or FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE in the
# environment for offline use).
set -eu
checkout=$(cd "$1" && pwd)
label=${2:-$(basename "$checkout")}
iterations=${3:-50}
bench_dir=$(cd "$(dirname "$0")" && pwd)
build_root=${CSLG_BENCH_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/cslg-bench}
mkdir -p "$build_root"

extra_cmake=()
read -r -a extra_cmake <<< "${CSLG_BENCH_CMAKE_ARGS:-}"
if [ -n "${FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE:-}" ]; then
    extra_cmake+=("-DFETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE=$FETCHCONTENT_SOURCE_DIR_CSLG_LIBBACKTRACE")
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

columns='%-16s %-8s %-10s %-12s %-12s %-12s %-10s'
# shellcheck disable=SC2059
printf "$columns %s\n" "config ($label)" "warm ns" "cold us" "1st lib A us" "1st lib B us" "RSS after kB" \
    "peak kB" "(per traced call / per new function / first call into each library)"
echo "$configs" | while read -r name type opts; do
    [ -n "$name" ] || continue
    read -r -a options <<< "$opts"
    build=$build_root/$label-$name
    log=$build/bench-build.log
    mkdir -p "$build"
    cmake -S "$bench_dir" -B "$build" -DCSLG_SOURCE_DIR="$checkout" -DCMAKE_BUILD_TYPE="$type" \
        "${options[@]}" ${extra_cmake[@]+"${extra_cmake[@]}"} > "$log" 2>&1 \
        || { echo "$name: configure failed, see $log"; continue; }
    cmake --build "$build" -j"$(nproc)" >> "$log" 2>&1 \
        || { echo "$name: build failed, see $log"; continue; }
    runs=$build/bench-runs.txt
    rm -f "$runs"
    for run in 1 2 3; do
        CSLG_OUTPUT_FILE=/dev/null "$build/cslg_bench" "$iterations" >> "$runs"
    done
    field() { grep "^$1=" "$runs" | cut -d= -f2 | median; }
    # shellcheck disable=SC2059
    printf "$columns\n" "$name" "$(field warm_per_call_ns)" "$(field cold_per_new_function_us)" \
        "$(field first_call_into_library_a_us)" "$(field first_call_into_library_b_us)" \
        "$(field rss_after_kb)" "$(field rss_peak_kb)"
done
