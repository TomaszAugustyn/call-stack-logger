/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_EXCEPTIONS crash-diagnostics driver — compiled WITH -finstrument-functions
 * and linked against the callstacklogger_log_exceptions_elapsed variant library.
 *
 * First calls a function that completes normally (its line must end up with a
 * real, patched duration), then descends uncaught_outer -> uncaught_leaf and
 * throws an exception nobody catches. std::terminate aborts the process without
 * unwinding, so no exit hook runs: every frame active at that point keeps its
 * "[  pending ]" placeholder. The throw line is written by the tracer's
 * __cxa_throw interposer before the runtime looks for a handler; when it finds
 * none, the runtime calls __cxa_begin_catch itself before terminating, which
 * the tracer recognizes and turns into the "terminate" line — the last line of
 * the trace, naming what killed the process (README, "Exceptions in the trace
 * tree"). The default terminate handler's own rethrow and catch, done to print
 * its message, are not traced.
 */

#include <cstdio>
#include <cstdlib>
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
// clang-format on

NOINLINE void uncaught_outer() {
    uncaught_leaf();
}

int main() {
    // Same no-core-dump setup as crash_traced_program.cpp: the abort is the
    // point of this program, the crash report is not (see the comment there).
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);
    // The abort below would discard a buffered stdout: the reported line must
    // reach the test even though the process never exits normally.
    setvbuf(stdout, nullptr, _IONBF, 0);

    (void)uncaught_completed_work(21);
    uncaught_outer();
    return 0; // never reached
}
