#!/bin/sh
#
# Copyright © 2020-2026 Tomasz Augustyn
# All rights reserved.
#
# Project Name: Call Stack Logger
# GitHub: https://github.com/TomaszAugustyn/call-stack-logger
# Contact Email: t.augustyn@poczta.fm
#
# Strips a binary the way Fedora's find-debuginfo does (debugedit's
# add_minidebug): the symbol table and the debug information go, and a
# MiniDebugInfo section takes their place, .gnu_debugdata — an xz-compressed
# ELF file holding the symbols of the functions that the dynamic symbol table
# lacks. Used for the cslg_minidebuginfo_traced_program fixture.
#
# Usage: add_minidebuginfo.sh <binary> <xz>

set -eu
binary=$1
xz=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

objcopy --only-keep-debug "$binary" "$work/debuginfo"
strip --strip-all "$binary"
nm -D --format=posix --defined-only "$binary" | awk '{ print $1 }' | sort > "$work/dynsyms"
nm --format=sysv --defined-only "$work/debuginfo" \
    | awk -F '|' '$4 ~ "FUNC" { sub(/ +$/, "", $1); print $1 }' | sort > "$work/funcsyms"
comm -13 "$work/dynsyms" "$work/funcsyms" > "$work/keep_symbols"
objcopy -S --remove-section .gdb_index --remove-section .comment \
    --keep-symbols="$work/keep_symbols" "$work/debuginfo" "$work/mini_debuginfo"
"$xz" "$work/mini_debuginfo"
objcopy --add-section .gnu_debugdata="$work/mini_debuginfo.xz" "$binary"
