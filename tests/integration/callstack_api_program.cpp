/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Exercises the public `instrumentation::get_call_stack()` API.
 *
 * Walks a small, known nested call chain and at the leaf calls get_call_stack(),
 * printing each resolved frame's function name to stdout (one per line, prefixed
 * with "FRAME: "). The integration test verifies the stack contains the expected
 * ancestor functions.
 *
 * Compiled WITHOUT -finstrument-functions (we don't need trace output — we
 * exercise the on-demand stack-capture API directly). Must still be compiled
 * with -g + -rdynamic so BFD / dladdr can resolve the symbols.
 */

#include "callStack.h"
#include <iostream>
#include <string>

// The chain functions carry noinline so each keeps a physical stack frame at
// any optimization level — backtrace() only sees physical frames, and the test
// asserts these exact ancestors. The non-instrumented build of this program
// additionally needs -fno-optimize-sibling-calls (set in CMakeLists.txt):
// noinline does not stop a call in tail position from becoming a jump, which
// would recycle the caller's frame just the same. The instrumented build is
// immune to that by construction (the exit hook call sits after every call).
// Printing lives in its own noinline function, deliberately: frame names for a
// captured stack come from resolving RETURN addresses, and at -O2 the resume
// point after the get_call_stack() call would otherwise sit in code attributed
// (via DWARF inline info) to an inlined std helper — e.g. the range-for's
// __normal_iterator constructor — so the innermost frame would resolve to that
// name instead of print_stack_from_leaf(). With a plain out-of-line call as the
// next statement, the resume point attributes to this function itself.
__attribute__((noinline)) void print_frames(
        const std::vector<std::optional<instrumentation::ResolvedFrame>>& stack) {
    for (const auto& maybe : stack) {
        if (maybe.has_value()) {
            std::cout << "FRAME: " << maybe->callee_function_name
                      << " | CALLER: " << maybe->caller_filename << ":";
            if (maybe->caller_line_number) {
                std::cout << *maybe->caller_line_number;
            } else {
                std::cout << "?";
            }
            std::cout << "\n";
        } else {
            std::cout << "FRAME: <unresolved>\n";
        }
    }
}

__attribute__((noinline)) void print_stack_from_leaf() {
    auto stack = instrumentation::get_call_stack();
    print_frames(stack);
}

__attribute__((noinline)) void callstack_mid() {
    print_stack_from_leaf();
}

__attribute__((noinline)) void callstack_top() {
    callstack_mid();
}

int main() {
    callstack_top();
    return 0;
}
