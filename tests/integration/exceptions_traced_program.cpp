/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_EXCEPTIONS integration-test driver — compiled WITH -finstrument-functions
 * and linked against the callstacklogger_log_exceptions variant libraries.
 *
 * Every scenario makes instrumented frames leave WITHOUT their exit hook in a
 * different way (an exception caught above them, which runs no exit hooks on
 * Clang; longjmp, which runs none on either compiler) and then calls a marker
 * function from a known depth. The tests assert the markers' exact depth: a
 * stale record left by any scenario would shift every later line.
 *
 * Throw and catch statements each share a source line with a REPORT_LINE()
 * call that prints that line number to stdout, so the tests can assert the
 * exact "(thrown at: <file>:<line>)" and "(caught at: <file>:<line>)" text.
 *
 * All scenario functions are noinline: the reconciliation rules key frames by
 * their stack address, and an inlined copy shares its host's frame. Keeping
 * every frame physical makes the scenarios mean the same thing at every
 * optimization level; the inlined shape has its own driver.
 */

#include <alloca.h>
#include <csetjmp>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define NOINLINE __attribute__((noinline))

// Prints "<tag>=<line>" for the statement that shares this source line.
#define REPORT_LINE(tag) std::printf("%s=%d\n", tag, __LINE__)

extern "C" void cslg_throwing_lib_throw(); // throwing_lib.cpp, built without instrumentation

// The one-line "REPORT_LINE(...); throw ...;" and "catch (...) { REPORT_LINE(...); ... }"
// statements below are deliberate: the reported line must be the throw's / the
// catch clause's own line.
// clang-format off

// --- A. throw through two frames; a destructor calls a traced helper while unwinding ---

NOINLINE void exc_helper() {
    std::puts("exc_helper");
}

struct ExcGuard {
    NOINLINE ~ExcGuard() { exc_helper(); }
};

NOINLINE void exc_thrower() {
    REPORT_LINE("THROW_A"); throw std::runtime_error("bad header");
}

NOINLINE void exc_mid() {
    ExcGuard guard;
    exc_thrower();
}

NOINLINE void exc_catcher() {
    try {
        exc_mid();
    } catch (const std::exception& e) { REPORT_LINE("CATCH_A"); std::printf("A caught: %s\n", e.what()); }
}

NOINLINE void exc_marker_a() {
    std::puts("exc_marker_a");
}

// --- B. recursion: throw four levels down, catch-all at the root ---

NOINLINE void exc_rec(int n) {
    if (n == 0) {
        REPORT_LINE("THROW_B"); throw std::logic_error("deep");
    }
    exc_rec(n - 1);
}

NOINLINE void exc_rec_catcher() {
    try {
        exc_rec(3);
    } catch (...) { REPORT_LINE("CATCH_B"); std::puts("B caught"); }
}

NOINLINE void exc_marker_b() {
    std::puts("exc_marker_b");
}

// --- C. throw from inside libstdc++ (vector::at) ---

NOINLINE void exc_lib_throw() {
    std::vector<int> v;
    try {
        (void)v.at(5);
    } catch (const std::out_of_range& e) { REPORT_LINE("CATCH_C"); std::printf("C caught: %s\n", e.what()); }
}

// --- D. throw from a shared library built without instrumentation ---

NOINLINE void exc_so_throw() {
    try {
        cslg_throwing_lib_throw();
    } catch (const std::invalid_argument& e) { REPORT_LINE("CATCH_D"); std::printf("D caught: %s\n", e.what()); }
}

// --- E. rethrow with `throw;` ---

NOINLINE void exc_rethrow_stmt() {
    try {
        try {
            REPORT_LINE("THROW_E"); throw std::logic_error("inner");
        } catch (...) { REPORT_LINE("RETHROW_E"); throw; }
    } catch (const std::exception& e) { REPORT_LINE("CATCH_E"); std::printf("E caught: %s\n", e.what()); }
}

// --- F. std::rethrow_exception ---

NOINLINE void exc_rethrow_ptr() {
    std::exception_ptr saved = std::make_exception_ptr(std::out_of_range("via exception_ptr"));
    try {
        REPORT_LINE("RETHROW_F"); std::rethrow_exception(saved);
    } catch (const std::exception& e) { REPORT_LINE("CATCH_F"); std::printf("F caught: %s\n", e.what()); }
}

// --- G. a new exception thrown inside a catch handler ---

NOINLINE void exc_nested_in_handler() {
    try {
        REPORT_LINE("THROW_G1"); throw std::runtime_error("outer");
    } catch (const std::exception&) { REPORT_LINE("CATCH_G1");
        try {
            REPORT_LINE("THROW_G2"); throw std::logic_error("inner");
        } catch (...) { REPORT_LINE("CATCH_G2"); std::puts("G caught both"); }
    }
}

// --- H. longjmp over two frames, then a call at the same stack level ---

static jmp_buf exc_jump_buffer;

NOINLINE void exc_jump_leaf() {
    longjmp(exc_jump_buffer, 1);
}

NOINLINE void exc_jump_mid() {
    exc_jump_leaf();
}

NOINLINE void exc_jump_after() {
    std::puts("exc_jump_after");
}

NOINLINE void exc_jump_root() {
    if (setjmp(exc_jump_buffer) == 0) {
        exc_jump_mid();
    }
    exc_jump_after();
}

NOINLINE void exc_marker_h() {
    std::puts("exc_marker_h");
}

// --- I. a loop re-calling a longjmp'ing function from the same call site ---

NOINLINE void exc_loop_marker() {
    std::puts("exc_loop_marker");
}

NOINLINE void exc_loop_jump() {
    for (int i = 0; i < 3; ++i) {
        if (setjmp(exc_jump_buffer) == 0) {
            exc_jump_leaf();
        }
    }
    exc_loop_marker();
}

// --- J. void recursion (the exit hook is tail-called at -O2) ---

NOINLINE void exc_void_rec(int n) {
    if (n > 0) {
        exc_void_rec(n - 1);
    }
}

// --- K. what() with control characters, a long what(), a non-std type ---

NOINLINE void exc_messages() {
    try {
        REPORT_LINE("THROW_K1"); throw std::runtime_error("line one\nline two\ttabbed");
    } catch (const std::exception&) { REPORT_LINE("CATCH_K1"); }
    try {
        REPORT_LINE("THROW_K2"); throw std::runtime_error(std::string(300, 'x'));
    } catch (const std::exception&) { REPORT_LINE("CATCH_K2"); }
    try {
        REPORT_LINE("THROW_K3"); throw 42;
    } catch (int) { REPORT_LINE("CATCH_K3"); }
}

// --- M. a catch inside a function whose frame size differs on every call ---
// alloca() (a variable-length array does the same) moves the stack pointer by a
// different amount per call, so the landing pad — and the tracer's catch
// interposer called from it — sits at a different distance from the frame's
// canonical address each time. The catcher's level must be exact every time: a
// distance cached from the first call overshoots on a later one and reclaims
// the catcher's own record, even main's, for the rest of the thread.

NOINLINE void exc_vla_thrower() {
    REPORT_LINE("THROW_M"); throw std::domain_error("in a variable frame");
}

NOINLINE void exc_vla_marker() {
    std::puts("exc_vla_marker");
}

NOINLINE void exc_vla_catcher(int bytes) {
    volatile char* buffer = static_cast<volatile char*>(alloca(bytes));
    buffer[0] = 1;
    try {
        exc_vla_thrower();
    } catch (const std::exception&) { REPORT_LINE("CATCH_M"); }
    exc_vla_marker();
}

// Keeps the catcher's frame well below the top of the stack: a level that
// overshoots the catcher's by the alloca difference then still lies on the
// thread's stack, where the rules apply, and reclaims live records — instead
// of being dismissed as off-stack when main's frame happens to sit near the
// end of the stack mapping, which would hide the defect on some runs.
NOINLINE void exc_vla_host() {
    volatile char pad[8192];
    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;
    exc_vla_catcher(4096);
    exc_vla_catcher(16);
    exc_vla_catcher(4096);
}

NOINLINE void exc_marker_m() {
    std::puts("exc_marker_m");
}

// --- N. an exception type whose what() is an instrumented override ---
// The tracer calls what() for the throw and catch lines; the override and the
// helper it calls are instrumented like everything else here, and those calls
// must leave no lines of their own — only the handler's own e.what() is traced.

NOINLINE const char* exc_custom_text() {
    return "custom what";
}

struct ExcCustom : std::runtime_error {
    using std::runtime_error::runtime_error;
    NOINLINE const char* what() const noexcept override { return exc_custom_text(); }
};

NOINLINE void exc_custom_what() {
    try {
        REPORT_LINE("THROW_N"); throw ExcCustom("ignored text");
    } catch (const std::exception& e) { REPORT_LINE("CATCH_N"); std::puts(e.what()); }
}

// --- L. an exception caught inside a worker thread (its own trace file) ---

NOINLINE void exc_thread_thrower() {
    REPORT_LINE("THROW_L"); throw std::runtime_error("in a worker");
}

NOINLINE void exc_thread_body() {
    try {
        exc_thread_thrower();
    } catch (const std::exception&) { REPORT_LINE("CATCH_L"); }
}

NOINLINE void exc_thread_marker() {
    std::puts("exc_thread_marker");
}

NOINLINE void exc_final_marker() {
    std::puts("exc_final_marker");
}

// clang-format on

int main() {
    exc_catcher();
    exc_marker_a();
    exc_rec_catcher();
    exc_marker_b();
    exc_lib_throw();
    exc_so_throw();
    exc_rethrow_stmt();
    exc_rethrow_ptr();
    exc_nested_in_handler();
    exc_jump_root();
    exc_marker_h();
    exc_loop_jump();
    exc_void_rec(3);
    exc_vla_host();
    exc_marker_m();
    exc_custom_what();
    exc_messages();
    std::thread worker([] {
        exc_thread_body();
        exc_thread_marker();
    });
    worker.join();
    exc_final_marker();
    return 0;
}
