/*
 * Copyright © 2020-2023 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include "callStack.h"
#include "format.h"
#include "prettyTime.h"
#include <mutex>
#include <stdio.h>

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

// Shared output file — protected by trace_mutex. Currently all threads write to the same
// file with serialized access. Per-thread trace files are a future enhancement for full
// multi-threaded support.
static FILE *fp_trace;
static std::mutex trace_mutex;

// Per-thread call stack tracking. Each thread has its own call stack depth, so these must
// be thread_local to prevent data races and incorrect depth tracking across threads.
static thread_local int current_stack_depth = -1;

// Maximum tracked call depth for the frame resolution stack. Each entry is 1 byte (bool).
// If call depth exceeds this limit, an overflow counter prevents stack desynchronization.
// This value can be increased if needed — the memory cost is MAX_TRACE_DEPTH bytes per thread.
static constexpr int MAX_TRACE_DEPTH = 2048;
static thread_local bool frame_resolved_stack[MAX_TRACE_DEPTH];
static thread_local int frame_resolved_top = -1;
// Counts frames that exceeded MAX_TRACE_DEPTH. On exit, overflow frames are handled
// before popping from the stack, keeping the stack in sync with the actual call stack.
static thread_local int frame_overflow_count = 0;

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
    {
        std::lock_guard<std::mutex> lock(trace_mutex);
        fp_trace = fopen("trace.out", "a");
        if (fp_trace != nullptr) {
            // Use line-buffered mode: flushes automatically after each '\n' (every trace
            // line ends with '\n'). This provides crash-safety without the overhead of
            // explicit fflush() on every function entry.
            setvbuf(fp_trace, NULL, _IOLBF, 0);
            fprintf(fp_trace,
                    "\n========================================\n"
                    "=== New trace run: %s\n"
                    "========================================\n",
                    utils::pretty_time().c_str());
            fflush(fp_trace);
        } else {
            fprintf(stderr, "[call-stack-logger] WARNING: Could not open trace.out for writing\n");
        }
    }
}

__attribute__ ((destructor))
NO_INSTRUMENT
void trace_end() {
    std::lock_guard<std::mutex> lock(trace_mutex);
    if(fp_trace != nullptr) {
        fclose(fp_trace);
        fp_trace = nullptr;
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if(fp_trace != nullptr) {
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        bool resolved = maybe_resolved.has_value();
        if (frame_resolved_top < MAX_TRACE_DEPTH - 1) {
            frame_resolved_stack[++frame_resolved_top] = resolved;
        } else {
            // Stack full — track overflow to keep exit handler in sync.
            // Indentation (current_stack_depth) remains correct regardless.
            frame_overflow_count++;
        }
        if (!resolved) { return; }
        current_stack_depth++;
        {
            std::lock_guard<std::mutex> lock(trace_mutex);
            fprintf(fp_trace, "%s\n", utils::format(*maybe_resolved, current_stack_depth).c_str());
        }
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    if (fp_trace != nullptr) {
        if (frame_overflow_count > 0) {
            // This exit corresponds to a frame that overflowed the stack.
            // Assume it was resolved (the common case at extreme depth) and
            // decrement depth to keep indentation consistent.
            frame_overflow_count--;
            current_stack_depth--;
        } else if (frame_resolved_top >= 0) {
            bool was_resolved = frame_resolved_stack[frame_resolved_top--];
            if (was_resolved) {
                current_stack_depth--;
            }
        }
    }
}

#endif
// clang-format on
