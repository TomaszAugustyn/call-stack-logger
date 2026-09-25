/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_EXCEPTIONS terminate driver — compiled WITH -finstrument-functions and
 * linked against the callstacklogger_log_exceptions_elapsed variant library.
 *
 * First calls a function that completes normally (its line must end up with a
 * real, patched duration), then terminates the process by the path named on
 * the command line (no argument: the plain uncaught throw):
 *
 *   (none)           uncaught_outer -> uncaught_leaf throws and nobody catches:
 *                    the runtime finds no handler and gives up inside __cxa_throw
 *   rethrow          a handler rethrows with `throw;` and no outer handler exists
 *                    (the runtime gives up inside __cxa_rethrow)
 *   rethrow_ptr      the same through std::rethrow_exception
 *   noexcept         the exception escapes a noexcept function: GCC's landing pad
 *                    calls __cxa_call_terminate, Clang's the __clang_call_terminate
 *                    stub in the executable — the runtime never searches further
 *   catch_terminate  a handler calls std::terminate() itself
 *   no_exception     std::terminate() with no exception at all
 *
 * std::terminate aborts the process without unwinding, so no exit hook runs:
 * every frame active at that point keeps its "[  pending ]" placeholder. The
 * throw line is written by the tracer's __cxa_throw interposer before the
 * runtime looks for a handler; each of the paths above ends in the "terminate"
 * line — the last line of the trace, naming what killed the process and why
 * (README, "Exceptions in the trace tree"). The terminate handler's own rethrow
 * and catch, done to print its message, are not traced.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#define NOINLINE __attribute__((noinline))

// Prints "<tag>=<line>" for the statement that shares this source line.
#define REPORT_LINE(tag) std::printf("%s=%d\n", tag, __LINE__)

NOINLINE int uncaught_completed_work(int x) {
    usleep(1'000);
    return x * 2;
}

// clang-format off
NOINLINE void uncaught_leaf() {
    REPORT_LINE("THROW_UNCAUGHT"); throw std::logic_error("nobody catches this");
}

NOINLINE void uncaught_outer() {
    uncaught_leaf();
}

// The catch clause and the statement inside it sit on separate lines so that
// the reported catch line and rethrow / terminate line differ.
NOINLINE void rethrow_with_no_outer_handler() {
    try {
        uncaught_outer();
    } catch (const std::exception&) { REPORT_LINE("CATCH_RETHROW");
        REPORT_LINE("RETHROW"); throw;
    }
}

NOINLINE void rethrow_exception_with_no_outer_handler() {
    try {
        uncaught_outer();
    } catch (const std::exception&) { REPORT_LINE("CATCH_RETHROW_PTR");
        REPORT_LINE("RETHROW_PTR"); std::rethrow_exception(std::current_exception());
    }
}

NOINLINE void noexcept_wall() noexcept {
    uncaught_outer();
}

NOINLINE void exception_escapes_noexcept() {
    try {
        noexcept_wall();
    } catch (const std::exception&) { std::puts("never reached: the exception cannot leave noexcept_wall"); }
}

NOINLINE void handler_calls_terminate() {
    try {
        uncaught_outer();
    } catch (const std::exception&) { REPORT_LINE("CATCH_TERMINATE");
        REPORT_LINE("TERMINATE_CALL"); std::terminate();
    }
}

NOINLINE void terminate_without_exception() {
    REPORT_LINE("TERMINATE_CALL"); std::terminate();
}
// clang-format on

int main(int argc, char** argv) {
    // Same no-core-dump setup as crash_traced_program.cpp: the abort is the
    // point of this program, the crash report is not (see the comment there).
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);
    // The abort below would discard a buffered stdout: the reported lines must
    // reach the test even though the process never exits normally.
    setvbuf(stdout, nullptr, _IONBF, 0);

    (void)uncaught_completed_work(21);
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "") == 0) {
        uncaught_outer();
    } else if (std::strcmp(mode, "rethrow") == 0) {
        rethrow_with_no_outer_handler();
    } else if (std::strcmp(mode, "rethrow_ptr") == 0) {
        rethrow_exception_with_no_outer_handler();
    } else if (std::strcmp(mode, "noexcept") == 0) {
        exception_escapes_noexcept();
    } else if (std::strcmp(mode, "catch_terminate") == 0) {
        handler_calls_terminate();
    } else if (std::strcmp(mode, "no_exception") == 0) {
        terminate_without_exception();
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }
    return 0; // never reached
}
