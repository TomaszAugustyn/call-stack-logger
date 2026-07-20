/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_ELAPSED crash-diagnostics driver — compiled WITH -finstrument-functions
 * and linked against the callstacklogger_log_elapsed variant library.
 *
 * First calls a function that completes normally (crash_completed_work — its
 * trace line must end up with a real, patched duration), then descends a
 * distinctive chain (crash_outer -> crash_middle -> crash_leaf) and abort()s
 * at the leaf. None of the chain's exit hooks run, so all four frames active
 * at the crash (main included) must keep their "[  pending ]" placeholders on
 * disk — the crash-diagnostic behavior documented in README's "Crash
 * diagnostics" section. No flush is needed for the lines to survive: the
 * trace file is line-buffered, so every enter line already reached the kernel
 * when abort() fires.
 *
 * Functions are deliberately NOT static — file-local linkage would hide them
 * from dladdr and they would silently vanish from the trace.
 */

#include <cstdlib>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

int crash_completed_work(int x) {
    // Give the frame a measurable duration so the test can assert a sensible
    // lower bound on the patched value.
    usleep(1'000);
    return x * 2;
}

void crash_leaf() {
    std::abort();
}

void crash_middle() {
    crash_leaf();
}

void crash_outer() {
    crash_middle();
}

int main() {
    // The abort() below is the point of this program, but the crash REPORT is
    // not: without the lines below, every test run feeds systemd-coredump /
    // abrt and pops "application crashed" desktop notifications on developer
    // machines (observed with the KDE and VS Code crash reporters, from native
    // and Docker runs alike). PR_SET_DUMPABLE=0 makes the kernel skip the
    // core_pattern handler entirely — RLIMIT_CORE=0 alone is NOT enough, since
    // systemd-coredump still gets invoked and logs the crash event to the
    // journal (storing no core), which is exactly what the desktop reporters
    // watch. RLIMIT_CORE=0 stays as belt-and-suspenders for hosts with a
    // plain-file core_pattern. The SIGABRT termination the test asserts is
    // unaffected. Deliberately placed in main's body: the tracer's own
    // /proc/self accesses (patch_fd reopen, cmdline, exe) all happen in main's
    // ENTER hook, i.e. before this line runs.
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);

    (void)crash_completed_work(21);
    crash_outer();
    return 0; // never reached
}
