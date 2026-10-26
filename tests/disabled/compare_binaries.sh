#!/bin/sh
# Compares two executables built from the same source — one with
# callstacklogger::instrumented in its link line, one without — under
# DISABLE_INSTRUMENTATION=ON. The target must contribute nothing, so the two
# files must be identical byte for byte. When they are not, the script says
# where they differ in the ways the target could make them differ:
#   - the shared libraries they need (readelf -d, NEEDED entries),
#   - the debug sections they contain (readelf -S, .debug_* names),
#   - the symbols they export (nm -D, defined dynamic symbols),
# and exits 1. Both files must exist and be ELF files: a missing program
# would otherwise yield two empty lists, which agree.
#
# usage: compare_binaries.sh <with_target> <without_target>
set -u
with=$1
without=$2
status=0

for program in "$with" "$without"; do
    if [ ! -f "$program" ] || ! readelf -h "$program" > /dev/null 2>&1; then
        echo "not an ELF file: $program"
        exit 1
    fi
done
if cmp -s "$with" "$without"; then
    echo "the two programs are identical byte for byte"
    echo "callstacklogger::instrumented is inert with DISABLE_INSTRUMENTATION=ON"
    exit 0
fi
echo "the two programs differ:"

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
exit 1
