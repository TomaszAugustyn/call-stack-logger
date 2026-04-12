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
#include <stdio.h>

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

static FILE *fp_trace;
static int current_stack_depth = -1;
static constexpr int MAX_TRACE_DEPTH = 1024;
static bool frame_resolved_stack[MAX_TRACE_DEPTH];
static int frame_resolved_top = -1;

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
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

__attribute__ ((destructor))
NO_INSTRUMENT
void trace_end() {
    if(fp_trace != nullptr) {
        fclose(fp_trace);
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if(fp_trace != nullptr) {
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        bool resolved = maybe_resolved.has_value();
        if (frame_resolved_top < MAX_TRACE_DEPTH - 1) {
            frame_resolved_stack[++frame_resolved_top] = resolved;
        }
        if (!resolved) { return; }
        current_stack_depth++;
        fprintf(fp_trace, "%s\n", utils::format(*maybe_resolved, current_stack_depth).c_str());
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit(void *callee, void *caller) {
    if (fp_trace != nullptr && frame_resolved_top >= 0) {
        bool was_resolved = frame_resolved_stack[frame_resolved_top--];
        if (was_resolved) {
            current_stack_depth--;
        }
    }
}

#endif
// clang-format on
