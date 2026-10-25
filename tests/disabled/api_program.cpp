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
// its ancestors resolved. Built and run by the consumer project next to this
// file; run_checks.sh looks for the three function names in the output.

#include "callStack.h"

#include <cstdio>
#include <optional>
#include <vector>

namespace {

// Printing lives in its own noinline function, as in the integration API
// driver: the leaf's own frame is named from the RETURN address after the
// get_call_stack() call, and in this Release build an inlined std helper as the
// next statement (the range-for's iterator constructor) would make DWARF
// attribute that address to the helper, naming the frame after it instead of
// after api_leaf. A plain out-of-line call as the next statement keeps the
// resume point in api_leaf.
__attribute__((noinline)) void print_frames(
        const std::vector<std::optional<instrumentation::ResolvedFrame>>& frames) {
    for (const auto& frame : frames) {
        if (frame) {
            std::printf("%s  (called from: %s:%s)\n", frame->callee_function_name.c_str(),
                        frame->caller_filename.c_str(), frame->caller_line_number ? "line" : "???");
        }
    }
}

__attribute__((noinline)) void api_leaf() {
    auto frames = instrumentation::get_call_stack();
    print_frames(frames);
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
