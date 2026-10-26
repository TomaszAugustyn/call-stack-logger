/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

// The on-demand API under DISABLE_INSTRUMENTATION=ON: with the hooks compiled
// out, a program that calls get_call_stack() links callstacklogger::callstacklogger
// (the instrumented target contributes nothing in such a build) and still gets
// its ancestors resolved. Built (Release, so -O2 inlining applies) and run by
// the consumer project next to this file; run_checks.sh looks for the three
// function names in the output.

#include "callStack.h"

#include <cstdio>

namespace {

// The leaf prints the frames itself, with the range-for right after the
// get_call_stack() call ON PURPOSE: in this Release build the instruction the
// call returns to belongs to an inlined std helper (the vector's iterator
// constructor), and DWARF names that helper as the innermost function there.
// A frame of get_call_stack() is named after the OUTERMOST function at its
// return address, so the frame still reads "api_leaf"; run_checks.sh requires
// it. (Releases before v1.0.0 named it after the innermost function, and the
// frame read as the __normal_iterator constructor.)
__attribute__((noinline)) void api_leaf() {
    for (const auto& frame : instrumentation::get_call_stack()) {
        if (frame) {
            std::printf("%s  (called from: %s:%s)\n", frame->callee_function_name.c_str(),
                        frame->caller_filename.c_str(), frame->caller_line_number ? "line" : "???");
        }
    }
}

__attribute__((noinline)) void api_middle() {
    api_leaf();
    __asm__ volatile("");
}

} // namespace

int main() {
    api_middle();
    __asm__ volatile("");
    std::puts("API_DONE");
    return 0;
}
