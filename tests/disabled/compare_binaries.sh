#!/bin/sh
# Compares two executables built from the same source — one with
# callstacklogger::instrumented in its link line, one without — under
# DISABLE_INSTRUMENTATION=ON. They must agree on:
#   - the shared libraries they need (readelf -d, NEEDED entries),
#   - the debug sections they contain (readelf -S, .debug_* names),
#   - the symbols they export (nm -D, defined dynamic symbols).
# Any difference means the target still contributes something (a library,
# -g, exported hooks) and the script exits 1 with the differing lists.
#
# usage: compare_binaries.sh <with_target> <without_target>
set -u
with=$1
without=$2
status=0

needed() { readelf -d "$1" | grep NEEDED | sed 's/.*Shared library: //' | sort; }
debug_sections() { readelf -S -W "$1" | grep -o '\.debug_[a-z_]*' | sort -u; }
exported() { nm -D --defined-only "$1" 2>/dev/null | awk '{print $NF}' | sort; }

compare() {
    name=$1; fn=$2
    a=$($fn "$with"); b=$($fn "$without")
    if [ "$a" = "$b" ]; then
        echo "$name: identical"
        printf '%s\n' "$a" | sed 's/^/    /'
    else
        echo "$name: DIFFERENT"
        echo "  with the target:"; printf '%s\n' "$a" | sed 's/^/    /'
        echo "  without the target:"; printf '%s\n' "$b" | sed 's/^/    /'
        status=1
    fi
}

compare "NEEDED libraries" needed
compare "debug sections" debug_sections
compare "exported dynamic symbols" exported

if [ $status -eq 0 ]; then
    echo "callstacklogger::instrumented is inert with DISABLE_INSTRUMENTATION=ON"
fi
exit $status
