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

void print_stack_from_leaf() {
    auto stack = instrumentation::get_call_stack();
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

void callstack_mid() {
    print_stack_from_leaf();
}

void callstack_top() {
    callstack_mid();
}

int main() {
    callstack_top();
    return 0;
}
