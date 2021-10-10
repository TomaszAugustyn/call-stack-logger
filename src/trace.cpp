#include "callStack.h"
#include "format.h"
#include <stdio.h>
#include <time.h>

// clang-format off
#ifndef DISABLE_INSTRUMENTATION

static FILE *fp_trace;
static int current_stack_depth = -1;
static bool last_frame_was_resolved = false;

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
    fp_trace = fopen("trace.out", "a");
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
        last_frame_was_resolved = false;
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        if (!maybe_resolved) { return; }
        last_frame_was_resolved = true;
        current_stack_depth++;
        fprintf(fp_trace, "%s\n", utils::format(*maybe_resolved, current_stack_depth).c_str());
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit (void *callee, void *caller) {
    if(fp_trace != nullptr && last_frame_was_resolved) {
        current_stack_depth--;
        //fprintf(fp_trace, "x %p %p %lu\n", callee, caller, time(nullptr));
    }
}

#endif
// clang-format on
