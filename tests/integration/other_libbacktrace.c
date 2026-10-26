/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * A second libbacktrace in the traced program, as when the program links one
 * of its own statically (a vendored copy, Boost.Stacktrace's libbacktrace
 * backend): these definitions stand in for that copy's public functions. The
 * tracer must never call them, so each one says so on stderr and aborts (see
 * OtherLibbacktraceTest). The signatures are libbacktrace's, written without
 * its header.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void called(const char* name) {
    fprintf(stderr, "OTHER_LIBBACKTRACE %s was called by the tracer\n", name);
    abort();
}

void* backtrace_create_state(const char* filename, int threaded, void* error_callback, void* data) {
    (void)filename;
    (void)threaded;
    (void)error_callback;
    (void)data;
    called("backtrace_create_state");
    return NULL;
}

int backtrace_pcinfo(void* state, uintptr_t pc, void* callback, void* error_callback, void* data) {
    (void)state;
    (void)pc;
    (void)callback;
    (void)error_callback;
    (void)data;
    called("backtrace_pcinfo");
    return 0;
}

int backtrace_syminfo(void* state, uintptr_t addr, void* callback, void* error_callback, void* data) {
    (void)state;
    (void)addr;
    (void)callback;
    (void)error_callback;
    (void)data;
    called("backtrace_syminfo");
    return 0;
}
