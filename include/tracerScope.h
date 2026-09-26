/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#pragma once

// Internal to the library: the per-thread re-entrancy guard the hooks consult,
// as the save-set / restore pair trace.cpp defines (no-ops when the hooks are
// compiled out) and an RAII scope over it. Not part of the public API; nothing
// outside the library's own sources includes it.

#ifndef NO_INSTRUMENT
    #define NO_INSTRUMENT __attribute__((no_instrument_function))
#endif

namespace instrumentation {

// Sets the guard and returns its previous value; restores that value. The
// save/restore semantics keep nesting correct: the enter hook already holds
// the guard when it calls instrumentation::resolve().
NO_INSTRUMENT bool enter_no_instrument_scope();
NO_INSTRUMENT void exit_no_instrument_scope(bool previous);

// RAII: while alive, the calling thread counts as inside the tracer, so
// __cyg_profile_func_enter/exit return at once. Held by
//   * the public API entry points (get_call_stack(), instrumentation::resolve(),
//     resolve_site(), inline_chain_at()) for their whole duration: the resolver
//     holds s_bfd_mutex while running std container/string template code, and
//     under Clang the linker may pick those templates' COMDAT instantiations
//     from the USER's instrumented TU — a hook firing there would re-lock
//     s_bfd_mutex on the same thread (a self-deadlock, observed via
//     unordered_map::find inside resolve_no_unwind). The scope also keeps the
//     resolver's own std internals out of the trace when the calling program
//     is instrumented;
//   * the LOG_EXCEPTIONS interposers, around their calls into the program's
//     own code — the thrown object's what(), possibly an override compiled
//     with instrumentation — which must leave no trace lines of their own.
// The destructor restores the guard even if the wrapped code throws
// (get_call_stack's backtrace-failure path).
struct TracerScope {
    bool previous;
    NO_INSTRUMENT TracerScope() : previous(enter_no_instrument_scope()) {}
    NO_INSTRUMENT ~TracerScope() { exit_no_instrument_scope(previous); }
    TracerScope(const TracerScope&) = delete;
    TracerScope& operator=(const TracerScope&) = delete;
};

} // namespace instrumentation
