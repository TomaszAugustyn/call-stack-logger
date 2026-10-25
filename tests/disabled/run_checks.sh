#!/bin/sh
# The checks ctest runs on the consumer project built with
# DISABLE_INSTRUMENTATION=ON:
#   1. compare_binaries.sh: the program linked with callstacklogger::instrumented
#      and the one linked without must be the same binary (NEEDED libraries,
#      debug sections, exported symbols);
#   2. api_program, linked with callstacklogger::callstacklogger, must still
#      resolve its own call stack through get_call_stack() — the three frames
#      api_leaf, api_middle and main by name.
#
# usage: run_checks.sh <with_target> <without_target> <api_program>
set -u
here=$(dirname "$0")
"$here/compare_binaries.sh" "$1" "$2" || exit 1

output=$("$3" 2>&1) || { echo "api_program failed:"; echo "$output"; exit 1; }
echo "$output"
for name in api_leaf api_middle main API_DONE; do
    case "$output" in
        *"$name"*) ;;
        *) echo "get_call_stack() output lacks '$name' under DISABLE_INSTRUMENTATION"; exit 1 ;;
    esac
done
echo "get_call_stack() resolves its ancestors with DISABLE_INSTRUMENTATION=ON"
