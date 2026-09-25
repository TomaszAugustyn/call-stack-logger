/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * LOG_EXCEPTIONS inlined-frames driver — compiled WITH -finstrument-functions
 * at -O2 and linked against the callstacklogger_log_exceptions_inlined variant.
 *
 * The frames of interest are always_inline, so they run inside their host's
 * physical frame: they share its level and its caller, and the level rules of
 * frameReconcile.h alone cannot tell a dead inlined frame (unwound by an
 * exception under Clang, skipped by longjmp) from a live inline host. The
 * DWARF inline chain at the catch site / enter site can, and that is what the
 * tests pin: right after the catch, and inside the retry loop, the next call
 * must sit at its true depth instead of under the stale inlined records.
 *
 * Hooks still fire for inlined copies on both compilers, so the inlined frames
 * appear in the trace like any other.
 */

#include <csetjmp>
#include <cstdio>
#include <stdexcept>

#define NOINLINE __attribute__((noinline))
#define ALWAYS_INLINE inline __attribute__((always_inline))

// Prints "<tag>=<line>" for the statement that shares this source line.
#define REPORT_LINE(tag) std::printf("%s=%d\n", tag, __LINE__)

// clang-format off

// --- A. thrower and middle inlined into the catcher ---

ALWAYS_INLINE void inl_thrower(int x) {
    if (x > 0) { REPORT_LINE("THROW_INL"); throw std::runtime_error("inlined throw"); }
}

ALWAYS_INLINE void inl_middle(int x) {
    inl_thrower(x + 1);
}

NOINLINE void inl_after_catch() {
    std::puts("inl_after_catch");
}

NOINLINE void inl_catcher(int x) {
    try {
        inl_middle(x);
    } catch (const std::exception& e) { REPORT_LINE("CATCH_INL"); std::printf("caught: %s\n", e.what()); }
    inl_after_catch();
}

NOINLINE void inl_marker_a() {
    std::puts("inl_marker_a");
}

// --- B. a retry loop whose longjmp'ing leaf is inlined into the loop ---

static jmp_buf inl_jump_buffer;

ALWAYS_INLINE void inl_jump_leaf(int i) {
    if (i >= 0) {
        longjmp(inl_jump_buffer, 1);
    }
}

NOINLINE void inl_loop_marker() {
    std::puts("inl_loop_marker");
}

NOINLINE void inl_loop_jump() {
    for (int i = 0; i < 3; ++i) {
        if (setjmp(inl_jump_buffer) == 0) {
            inl_jump_leaf(i);
        }
    }
    inl_loop_marker();
}

NOINLINE void inl_marker_b() {
    std::puts("inl_marker_b");
}

// clang-format on

int main(int argc, char**) {
    inl_catcher(argc);
    inl_marker_a();
    inl_loop_jump();
    inl_marker_b();
    return 0;
}
