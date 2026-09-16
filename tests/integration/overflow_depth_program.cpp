/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Deep-recursion driver — compiled WITH -finstrument-functions.
 *
 * Recurses 3000 frames deep, well past the per-thread frame stack's initial
 * capacity (INITIAL_FRAME_CAPACITY = 2048 in src/trace.cpp), so the stack has
 * to grow on demand for the last ~950 frames. Every frame still gets a trace
 * line (indentation keeps growing; format() clamps the line to its buffer) and
 * its own record, and post_overflow_marker() proves the depth accounting
 * survived the growth: it must appear at the same depth as the first
 * deep_recursion frame (both are direct children of main).
 *
 * At -O0 (the default build) each frame is small (~100 bytes), so 3000 frames
 * use well under the default 8 MB stack.
 */

#include <cstdio>

// The `n % 7 +` after the recursive call keeps this a genuine non-tail call at
// any optimization level, so every level occupies a real stack frame.
int deep_recursion(int n) {
    if (n <= 1) {
        return 1;
    }
    return n % 7 + deep_recursion(n - 1);
}

void post_overflow_marker() {
    std::puts("post_overflow_marker done");
}

int main() {
    (void)deep_recursion(3000);
    post_overflow_marker();
    return 0;
}
