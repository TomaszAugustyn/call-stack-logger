/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include "callStack.h"
#include "format.h"
#include "prettyTime.h"
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <stdio.h>
#include <unistd.h>

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

// Maximum tracked call depth for the frame resolution stack. Each entry is 1 byte (bool).
// If call depth exceeds this limit, an overflow counter prevents stack desynchronization.
// This value can be increased if needed — the memory cost is MAX_TRACE_DEPTH bytes per thread.
static constexpr int MAX_TRACE_DEPTH = 2048;

// All per-thread state bundled in one struct for readability.
//
// IMPORTANT member order: the re-entrancy guard `in_instrumentation` is declared FIRST
// so it is destroyed LAST (members are destroyed in reverse declaration order). That
// keeps the guard alive during the destruction of any other member that might itself
// be instrumented by Clang (e.g. the future `trace_file` member with a non-trivial
// destructor added in a later step).
struct PerThreadState {
    // Re-entrancy guard: prevents recursive instrumentation when the resolve/format
    // pipeline calls std library functions that may themselves be instrumented
    // (especially with Clang). Also set during trace_begin/trace_end.
    bool in_instrumentation = false;

    // Current call stack depth for indentation (starts at -1 = top level).
    int current_stack_depth = -1;

    // Per-frame "was this frame resolved?" LIFO stack for the exit handler.
    bool frame_resolved_stack[MAX_TRACE_DEPTH] = {};
    int frame_resolved_top = -1;

    // Counts frames that exceeded MAX_TRACE_DEPTH. On exit, overflow frames are
    // handled before popping from the stack, keeping the stack in sync.
    int frame_overflow_count = 0;
};

static thread_local PerThreadState t_state;

// All process-wide globals bundled in one struct. Only `fp_trace` and `trace_mutex`
// exist in this step — additional fields for the multi-threaded refactor are added
// in later steps.
struct TraceGlobals {
    // Shared output file — protected by trace_mutex. Currently all threads write to
    // the same file with serialized access (soon to be replaced by per-thread files).
    FILE* fp_trace = nullptr;
    std::mutex trace_mutex;
};

static TraceGlobals g_trace;

// Closes the trace file and permanently disables instrumentation. Called via atexit()
// to ensure it runs before static object destructors (like s_bfds map). With Clang,
// static destructors may be instrumented — without this early shutdown, instrumented
// destructors would call resolve() on already-destroyed BFD objects.
NO_INSTRUMENT
static void trace_shutdown() {
    t_state.in_instrumentation = true;  // Permanently disable — never cleared
    std::lock_guard<std::mutex> lock(g_trace.trace_mutex);
    if (g_trace.fp_trace != nullptr) {
        fclose(g_trace.fp_trace);
        g_trace.fp_trace = nullptr;
    }
}

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
    t_state.in_instrumentation = true;
    {
        std::lock_guard<std::mutex> lock(g_trace.trace_mutex);

        // Output path is configurable via CSLG_OUTPUT_FILE environment variable.
        // Defaults to "trace.out" in the current working directory.
        const char* trace_path = std::getenv("CSLG_OUTPUT_FILE");
        if (trace_path == nullptr || trace_path[0] == '\0') {
            trace_path = "trace.out";
        }

        // Use open() with O_NOFOLLOW to prevent symlink-based file overwrite attacks,
        // then wrap the file descriptor with fdopen() for buffered I/O.
        // Permissions 0600: owner read/write only — trace output may contain internal
        // source paths and function names that should not be world-readable.
        int fd = open(trace_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            g_trace.fp_trace = fdopen(fd, "a");
            if (g_trace.fp_trace == nullptr) {
                close(fd);
            }
        }

        if (g_trace.fp_trace != nullptr) {
            // Use line-buffered mode: flushes automatically after each '\n' (every trace
            // line ends with '\n'). This provides crash-safety without the overhead of
            // explicit fflush() on every function entry.
            setvbuf(g_trace.fp_trace, NULL, _IOLBF, 0);
            fprintf(g_trace.fp_trace,
                    "\n========================================\n"
                    "=== New trace run: %s\n"
                    "========================================\n",
                    utils::pretty_time().c_str());
            fflush(g_trace.fp_trace);
        } else {
            fprintf(stderr, "[call-stack-logger] WARNING: Could not open %s for writing\n", trace_path);
        }
    }
    // Register shutdown via atexit so it runs before static destructors.
    // This is critical for Clang where static destructors (like s_bfds map) may be
    // instrumented — the shutdown must close the trace file and disable instrumentation
    // before those destructors fire.
    std::atexit(trace_shutdown);
    t_state.in_instrumentation = false;
}

__attribute__ ((destructor))
NO_INSTRUMENT
void trace_end() {
    // Fallback cleanup in case atexit didn't run (e.g., _exit() or abort()).
    trace_shutdown();
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if (t_state.in_instrumentation) { return; }
    // Set the guard BEFORE any work and clear it AFTER all local variables (especially
    // maybe_resolved, which is a std::optional<ResolvedFrame>) have been destroyed.
    // With Clang, destructors of std library types may be instrumented, so
    // in_instrumentation must remain true until all destructors have run.
    t_state.in_instrumentation = true;
    if (g_trace.fp_trace != nullptr) {
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        bool resolved = maybe_resolved.has_value();
        if (t_state.frame_resolved_top < MAX_TRACE_DEPTH - 1) {
            t_state.frame_resolved_stack[++t_state.frame_resolved_top] = resolved;
        } else {
            // Stack full — track overflow to keep exit handler in sync.
            // Indentation (current_stack_depth) remains correct regardless.
            t_state.frame_overflow_count++;
        }
        if (resolved) {
            t_state.current_stack_depth++;
            {
                std::lock_guard<std::mutex> lock(g_trace.trace_mutex);
                // Re-check under lock: trace_end() may have closed fp_trace between the
                // outer check and here. The outer check is a fast path to skip work when
                // tracing is disabled; this is the authoritative check.
                if (g_trace.fp_trace != nullptr) {
                    fprintf(g_trace.fp_trace, "%s\n",
                            utils::format(*maybe_resolved, t_state.current_stack_depth).c_str());
                }
            }
        }
    } // maybe_resolved and lock_guard destructors run here, still under guard
    t_state.in_instrumentation = false;
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    if (t_state.in_instrumentation) { return; }
    if (g_trace.fp_trace != nullptr) {
        if (t_state.frame_overflow_count > 0) {
            // This exit corresponds to a frame that overflowed the stack.
            // Assume it was resolved (the common case at extreme depth) and
            // decrement depth to keep indentation consistent.
            t_state.frame_overflow_count--;
            t_state.current_stack_depth--;
        } else if (t_state.frame_resolved_top >= 0) {
            bool was_resolved = t_state.frame_resolved_stack[t_state.frame_resolved_top--];
            if (was_resolved) {
                t_state.current_stack_depth--;
            }
        }
    }
}

#endif
// clang-format on
