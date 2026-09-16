/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Deep-recursion driver with an UNLOGGED frame at every level — compiled WITH
 * -finstrument-functions.
 *
 * Like overflow_depth_program, this recurses 3000 frames deep, well past the
 * per-thread frame stack's initial capacity (2048) in src/trace.cpp. The twist:
 * every recursion level also calls filtered_overflow_call(), an exported
 * function of a stripped, instrumented shared library (filtered_overflow_lib.cpp)
 * that in turn calls a file-local helper. After stripping, neither dladdr() nor
 * BFD can name that helper, so the resolver drops its frame — its enter hook
 * fires but writes no line. That makes a third of the frames beyond the
 * initial capacity "entered but not logged".
 *
 * The frame stack grows on demand, so every one of those frames still gets its
 * own record and the exit hook knows exactly which frames adjusted the depth.
 * A fixed-capacity stack that guessed "every deep frame was logged" would
 * decrement the depth once per filtered frame, drifting it thousands of levels
 * negative — every later line on the thread then rendered at depth 0. The
 * markers pin the accounting: marker_a and marker_b must trace at the same
 * depth as the first deep_recursion frame (all direct children of main), and
 * marker_b_child one level deeper.
 *
 * The library's noinline and asm barriers keep the helper a real call at every
 * optimization level, so the RelWithDebInfo suite exercises the same hook
 * sequence as -O0. At -O0 each
 * recursion frame is small (~100 bytes), so 3000 frames use well under the
 * default 8 MB stack.
 */

#include <cstdio>

// Exported by the stripped, instrumented shared library; calls the unnameable
// file-local helper whose frame is never logged.
extern "C" int filtered_overflow_call(int n);

// The `n % 7 +` after the recursive call keeps this a genuine non-tail call at
// any optimization level, so every level occupies a real stack frame.
int deep_recursion(int n) {
    if (n <= 1) {
        return 1;
    }
    const int h = filtered_overflow_call(n);
    return h + n % 7 + deep_recursion(n - 1);
}

void marker_a() {
    std::puts("marker_a");
}

void marker_b_child() {
    std::puts("marker_b_child");
}

void marker_b() {
    marker_b_child();
}

int main() {
    (void)deep_recursion(3000);
    marker_a();
    marker_b();
    return 0;
}
