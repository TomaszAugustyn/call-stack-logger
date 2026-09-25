/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * longjmp driver — compiled WITH -finstrument-functions, default library.
 *
 * longjmp() restores the stack without running anything on the way, so the
 * exit hooks of the frames it jumps over never fire, on both compilers. The
 * tracer keys every frame record by its stack level and reclaims the
 * jumped-over records at the next hook (include/frameReconcile.h), so the
 * calls that follow a jump sit at their true depth instead of one level deeper
 * per skipped frame. Two shapes: a jump over two frames followed by a call at
 * the jumper's level, and a retry loop whose leaf jumps out three times from
 * the same call site (see LongjmpTest).
 */

#include <csetjmp>
#include <cstdio>

#define NOINLINE __attribute__((noinline))

static jmp_buf jump_target;
static jmp_buf loop_target;

NOINLINE void jump_leaf() {
    longjmp(jump_target, 1);
}

NOINLINE void jump_mid() {
    jump_leaf();
    std::puts("never reached");
}

NOINLINE void post_jump_marker() {
    std::puts("post_jump_marker");
}

NOINLINE void jump_outer() {
    if (setjmp(jump_target) == 0) {
        jump_mid();
    }
    post_jump_marker();
}

NOINLINE void jump_loop_leaf(int i) {
    if (i >= 0) {
        longjmp(loop_target, 1);
    }
}

NOINLINE void loop_marker() {
    std::puts("loop_marker");
}

NOINLINE void jump_loop() {
    for (int i = 0; i < 3; ++i) {
        // `i` is not modified between setjmp() and the longjmp() that returns
        // here, so its value is well defined after the jump.
        if (setjmp(loop_target) == 0) {
            jump_loop_leaf(i);
        }
    }
    loop_marker();
}

NOINLINE void final_marker() {
    std::puts("final_marker");
}

int main() {
    jump_outer();
    jump_loop();
    final_marker();
    return 0;
}
