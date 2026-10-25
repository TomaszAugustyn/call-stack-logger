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
 * After the main-thread capture, two worker threads capture their own stacks
 * CONCURRENTLY (each through its own noinline chain), exercising the API's
 * thread safety: the per-thread re-entrancy guard and s_resolver_mutex contention
 * from non-main threads outside any hook. Each worker formats its result into
 * a private string ("W1_FRAME: <name>" / "W2_FRAME: <name>" lines, names
 * only); main prints both after join so stdout never interleaves.
 *
 * Compiled WITHOUT -finstrument-functions (we don't need trace output — we
 * exercise the on-demand stack-capture API directly). Must still be compiled
 * with -g so the symbolizer can resolve the symbols and lines.
 */

#include "callStack.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// The chain functions carry noinline so each keeps a physical stack frame at
// any optimization level — backtrace() only sees physical frames, and the test
// asserts these exact ancestors. The non-instrumented build of this program
// additionally needs -fno-optimize-sibling-calls (set in CMakeLists.txt):
// noinline does not stop a call in tail position from becoming a jump, which
// would recycle the caller's frame just the same. The instrumented build is
// immune to that by construction (the exit hook call sits after every call).
// The leaf prints the frames itself, with the range-for right after the
// get_call_stack() call ON PURPOSE. A captured frame is named from its RETURN
// address, and at -O2 the instruction after the call belongs to an inlined std
// helper (the range-for's __normal_iterator constructor); DWARF names that
// helper as the innermost function at the address. get_call_stack() names a
// frame after the OUTERMOST function at its return address, so the frame still
// reads "print_stack_from_leaf" — which the test asserts, making this shape
// the regression pin. (Before that rule the resolver took the innermost
// function, and printing had to live in a separate noinline function so that
// the resume point stayed in the leaf's own code.)
__attribute__((noinline)) void print_stack_from_leaf() {
    for (const auto& maybe : instrumentation::get_call_stack()) {
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

__attribute__((noinline)) void callstack_mid() {
    print_stack_from_leaf();
}

__attribute__((noinline)) void callstack_top() {
    callstack_mid();
}

// Worker-thread capture chain. Same noinline / physical-frame reasoning as the
// main chain above. Names only (no CALLER column) — the worker assertions need
// just the chain order, and keeping the format distinct from "FRAME:" lines
// leaves the main-thread caller-location assertions untouched.
//
// The worker leaf builds its ostringstream in place, right after the
// get_call_stack() call, ON PURPOSE: at -O2 the inlined ostringstream
// constructor is the innermost DWARF function at the leaf's resume point
// (std::basic_ios::basic_ios()), and the frame used to be named after it. With
// frames named after the outermost function at the return address the frame
// reads "worker_stack_leaf", which the test asserts — the second regression pin
// of that rule, on a worker thread.
__attribute__((noinline)) std::string worker_stack_leaf(const char* prefix) {
    std::ostringstream oss;
    for (const auto& maybe : instrumentation::get_call_stack()) {
        if (maybe.has_value()) {
            oss << prefix << "FRAME: " << maybe->callee_function_name << "\n";
        } else {
            oss << prefix << "FRAME: <unresolved>\n";
        }
    }
    return oss.str();
}

__attribute__((noinline)) std::string worker_stack_top(const char* prefix) {
    std::string result = worker_stack_leaf(prefix);
    return result;
}

// The worker section lives in its own noinline function. It used to be
// required: with the std::string / std::thread construction inlined directly
// into main() after the callstack_top() call, main's captured frame resolved
// as the inlined basic_string helper at -O2 (the resume-point attribution
// described on print_stack_from_leaf) and "FRAME: main" vanished from the
// output. The outermost-function rule names main either way; the separation
// is kept for readability.
__attribute__((noinline)) void run_worker_captures() {
    // Concurrent captures from two worker threads — both contend on
    // s_resolver_mutex inside the resolver while neither is the thread that ran
    // trace_begin(). Output is buffered per thread and printed after join.
    std::string out1;
    std::string out2;
    std::thread t1([&out1] { out1 = worker_stack_top("W1_"); });
    std::thread t2([&out2] { out2 = worker_stack_top("W2_"); });
    t1.join();
    t2.join();
    std::cout << out1 << out2;
}

int main() {
    callstack_top();
    run_worker_captures();
    return 0;
}
