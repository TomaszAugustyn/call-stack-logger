#include "callStack.h"
#include "utils.h"
#include <stdio.h>
#include <time.h>

// clang-format off

static FILE *fp_trace;

__attribute__ ((constructor))
NO_INSTRUMENT
void trace_begin() {
    fp_trace = fopen("trace.out", "a");
}

__attribute__ ((destructor))
NO_INSTRUMENT
void trace_end() {
    if(fp_trace != NULL) {
        fclose(fp_trace);
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_enter(void *callee, void *caller) {
    if(fp_trace != NULL) {
        auto maybe_resolved = instrumentation::resolve(callee, caller);
        if (!maybe_resolved) { return; }
        fprintf(fp_trace, "%s\n", utils::format(*maybe_resolved).c_str());
    }
}

extern "C" NO_INSTRUMENT
void __cyg_profile_func_exit (void *callee, void *caller) {
    if(fp_trace != NULL) {
        //fprintf(fp_trace, "x %p %p %lu\n", callee, caller, time(NULL));
    }
}
// clang-format on