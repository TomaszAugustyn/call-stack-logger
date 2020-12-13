#include <stdio.h>
#include <time.h>
#include "callStack.h"
#include "unwinder.h"

static FILE *fp_trace;

__attribute__ ((constructor))
void trace_begin() {
    fp_trace = fopen("trace.out", "a");
}

__attribute__ ((destructor))
__attribute__ ((no_instrument_function))
void trace_end() {
    if(fp_trace != NULL) {
        fclose(fp_trace);
    }
}

extern "C" __attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *callee, void *caller) {
    if(fp_trace != NULL) {
        instrumentation::Callback callback(caller);
        instrumentation::unwind_nth_frame(callback, 4);

        std::string resolved = instrumentation::resolve(callee, callback.caller);
        if (resolved.empty()) { return; }
        fprintf(fp_trace, "%s\n", resolved.c_str());
    }
}

extern "C" __attribute__((no_instrument_function))
void __cyg_profile_func_exit (void *callee, void *caller) {
    if(fp_trace != NULL) {
        //fprintf(fp_trace, "x %p %p %lu\n", callee, caller, time(NULL));
    }
}
