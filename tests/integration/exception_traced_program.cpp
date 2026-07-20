/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Exception-unwind driver — compiled WITH -finstrument-functions.
 *
 * Throws through two instrumented frames (exception_thrower, exception_mid)
 * and catches in exception_catcher. On GCC, -finstrument-functions emits
 * __cyg_profile_func_exit on the exceptional path too (as a cleanup), so
 * enter/exit stay paired and the depth counter is intact after the catch —
 * post_catch_marker() must trace at the same depth as exception_catcher.
 *
 * On Clang the exit hooks of unwound frames are silently skipped (documented
 * limitation, see README "Compiler-specific instrumentation"), so the
 * corresponding integration test only asserts under GCC.
 */

#include <cstdio>
#include <stdexcept>

void exception_thrower() {
    throw std::runtime_error("probe");
}

void exception_mid() {
    exception_thrower();
}

void exception_catcher() {
    try {
        exception_mid();
    } catch (const std::exception&) {
        std::puts("caught");
    }
}

void post_catch_marker() {
    std::puts("post_catch_marker done");
}

int main() {
    exception_catcher();
    post_catch_marker();
    return 0;
}
