#!/bin/sh
# The checks ctest runs on the consumer project built with
# DISABLE_INSTRUMENTATION=ON:
#   1. compare_binaries.sh: the program linked with callstacklogger::instrumented
#      and the one linked without must be the same file, byte for byte;
#   2. api_program, linked with callstacklogger::callstacklogger, must still
#      resolve its own call stack through get_call_stack() — the three frames
#      api_leaf, api_middle and main by name, each a line of its own (a bare
#      "main" would also match __libc_start_call_main's frame).
#
# usage: run_checks.sh <with_target> <without_target> <api_program>
set -u
here=$(dirname "$0")
"$here/compare_binaries.sh" "$1" "$2" || exit 1

output=$("$3" 2>&1) || { echo "api_program failed:"; echo "$output"; exit 1; }
echo "$output"
for prefix in '(anonymous namespace)::api_leaf()  (called from: ' \
              '(anonymous namespace)::api_middle()  (called from: ' \
              'main  (called from: ' \
              'API_DONE'; do
    if ! printf '%s\n' "$output" | awk -v prefix="$prefix" 'index($0, prefix) == 1 { found = 1 } END { exit !found }'
    then
        echo "get_call_stack() output has no line starting with '$prefix' under DISABLE_INSTRUMENTATION"
        exit 1
    fi
done
echo "get_call_stack() resolves its ancestors with DISABLE_INSTRUMENTATION=ON"
