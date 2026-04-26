/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_ELAPSED integration-test driver — compiled WITH -finstrument-functions
 * and linked against callstacklogger_log_elapsed.
 *
 * Exercises two properties of the elapsed-duration feature:
 *   1. A known nested call chain (elapsed_outer -> elapsed_middle ->
 *      elapsed_inner) whose reported durations must be monotonically
 *      non-increasing (parent >= child).
 *   2. A usleep(10000) inside elapsed_inner, giving that frame a
 *      predictable lower-bound duration of ~10 ms that LogElapsedFlagTest
 *      asserts.
 *
 * Names intentionally distinctive so the test fixture can grep for them
 * unambiguously in the trace file.
 */

#include <iostream>
#include <unistd.h>

void elapsed_inner() {
    // The sentinel: sleeps 10 ms so the test fixture can assert that this
    // frame's reported elapsed duration is >= 10 ms (with a generous upper
    // bound to absorb CI scheduling jitter).
    usleep(10'000);
    std::cout << "elapsed_inner\n";
}

void elapsed_middle() {
    elapsed_inner();
}

void elapsed_outer() {
    elapsed_middle();
}

int main() {
    elapsed_outer();
    return 0;
}
