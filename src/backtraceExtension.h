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

// Creates a state for one object file: threaded, with libbacktrace's own lazy,
// executable-based module discovery switched off, so that only the file
// registered with cslg_backtrace_add_file() is ever consulted. Returns NULL
// when the state cannot be allocated (error_callback was called). A state
// cannot be freed (libbacktrace has no call for it).
struct backtrace_state* cslg_backtrace_create_state(backtrace_error_callback error_callback, void* data);

// Registers the ELF file at `path`, mapped by the dynamic loader with the load
// bias `load_bias` (link_map::l_addr), as a module of `state`: its symbol
// table and DWARF debug information — including a separate debug file found
// through .gnu_debuglink or the build ID — then answer backtrace_pcinfo() and
// backtrace_syminfo() for the RUNTIME addresses inside it. Returns 1 on success
// and 0 on failure, after calling error_callback (file not readable, not a
// regular file, not ELF, truncated or malformed — see the layout check in the
// implementation); a failed file adds nothing to the state. A file whose
// DWARF data cannot be read but whose symbol table can is a success with
// `has_dwarf` 0 (names without lines).
// `has_symtab` / `has_dwarf` report what libbacktrace found (a symbol table
// of any kind — .symtab, or .dynsym when there is no .symtab — and DWARF
// data, in the file or in its separate debug file);
// `has_own_symtab` reports whether the file ITSELF carries a symbol table:
// a .symtab section, which a `strip --strip-all` removes, or MiniDebugInfo
// (a .gnu_debugdata section, what Fedora's stripped binaries keep). Must be
// serialized by the caller with every other call of either function and
// every lookup, on any state (the resolver's mutex): opening the files keeps
// a little state of its own, the directory relative alt links are read from.
int cslg_backtrace_add_file(struct backtrace_state* state, const char* path, uintptr_t load_bias,
                            backtrace_error_callback error_callback, void* data, int* has_symtab,
                            int* has_dwarf, int* has_own_symtab);

#ifdef __cplusplus
}
#endif
