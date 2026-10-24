/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

// Internal: the two entry points of src/backtraceExtension.c, the registration
// of object files with libbacktrace's state at their runtime load bias. Used by
// src/callStack.cpp only; not part of the public API and not installed with
// include/ (it needs libbacktrace's backtrace.h, which consumers do not have).
#pragma once

#include <backtrace.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the one state the resolver uses: threaded, with libbacktrace's own
// lazy, executable-based module discovery switched off, so that only the files
// registered with cslg_backtrace_add_file() are ever consulted. Returns NULL
// when the state cannot be allocated (error_callback was called).
struct backtrace_state* cslg_backtrace_create_state(backtrace_error_callback error_callback, void* data);

// Registers the ELF file at `path`, mapped by the dynamic loader with the load
// bias `load_bias` (link_map::l_addr), as a module of `state`: its symbol
// table and DWARF debug information — including a separate debug file found
// through .gnu_debuglink or the build ID — then answer backtrace_pcinfo() and
// backtrace_syminfo() for the RUNTIME addresses inside it. Returns 1 on success
// and 0 on failure, after calling error_callback (file not readable, not ELF,
// truncated — see the layout check in the implementation); a failed file adds
// nothing to the state. `has_symtab` / `has_dwarf` report what the file
// contributed. Must be serialized with every other call on the state by the
// caller (the resolver's mutex).
int cslg_backtrace_add_file(struct backtrace_state* state, const char* path, uintptr_t load_bias,
                            backtrace_error_callback error_callback, void* data, int* has_symtab,
                            int* has_dwarf);

#ifdef __cplusplus
}
#endif
