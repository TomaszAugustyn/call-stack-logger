#include <stdio.h>
#include <time.h>
#include <dlfcn.h>  // for dladdr
#include <cxxabi.h> // for __cxa_demangle
#include "callStack.h"

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
void __cyg_profile_func_enter(void *func,  void *caller) {
    if(fp_trace != NULL) {
        fprintf(fp_trace, "e %p %p %lu\n", func, caller, time(NULL) );

        Dl_info info;
		if (dladdr(func, &info)) {
            int status;
            const char* name;
            char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, 0, &status);
            if (status == 0) {
                name = demangled ? demangled : "[not demangeled]";
            } else {
                name = info.dli_sname ? info.dli_sname : "[no dli_sname]";
            }

            fprintf(fp_trace, "%s (%s)\n", name, info.dli_fname);
            fprintf(fp_trace, "bdf: %s\n", instrumentation::resolve(func).c_str());

            if (demangled) {
                delete demangled;
                demangled = nullptr;
            }
		} else {
            fprintf(fp_trace, "%s\n", "unknown");
		}

    }
}

extern "C" __attribute__((no_instrument_function))
void __cyg_profile_func_exit (void *func, void *caller) {
    if(fp_trace != NULL) {
        fprintf(fp_trace, "x %p %p %lu\n", func, caller, time(NULL));
    }
}
