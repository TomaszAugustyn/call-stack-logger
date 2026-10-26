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

#include <algorithm>
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

// --- C. a lambda inlined into the function that defines it throws; that function catches ---
// The lambda's operator() is a local entity of inl_lambda_catcher, so its
// demangled name begins with the catcher's own ("inl_lambda_catcher(int)::{lambda
// (int)#1}::operator()..."): the base names of the two must differ for the
// catch-site inline chain to find the catcher's record below the dead lambda's.

NOINLINE void inl_after_lambda_catch() {
    std::puts("inl_after_lambda_catch");
}

NOINLINE void inl_lambda_catcher(int x) {
    auto thrower = [](int v) __attribute__((always_inline)) {
        if (v > 0) { REPORT_LINE("THROW_LAMBDA"); throw std::runtime_error("lambda throw"); }
    };
    try {
        thrower(x);
    } catch (const std::exception& e) { REPORT_LINE("CATCH_LAMBDA"); std::printf("caught: %s\n", e.what()); }
    inl_after_lambda_catch();
}

NOINLINE void inl_marker_c() {
    std::puts("inl_marker_c");
}

// --- D. functions without a linkage name inlined into their hosts make normal calls ---
// DWARF gives no DW_AT_linkage_name to a C function (its name is not mangled)
// nor, on GCC, to a C++ function with internal linkage, so the symbol table,
// which only knows the host, must not name the call site: each inlined
// parent's record is live while its callee runs, and the callee must sit
// under it. The C function lives in inlined_c_part.c.

NOINLINE void inl_child() {
    std::puts("inl_child");
}

NOINLINE void inl_sibling() {
    std::puts("inl_sibling");
}

namespace {
ALWAYS_INLINE void inl_anon_parent() { inl_child(); __asm__ volatile(""); }
} // namespace

static ALWAYS_INLINE void inl_static_parent() { inl_child(); __asm__ volatile(""); }

NOINLINE void inl_anon_host() {
    inl_anon_parent();
    inl_sibling();
}

NOINLINE void inl_static_host() {
    inl_static_parent();
    inl_sibling();
}

NOINLINE void inl_lambda_host() {
    auto parent = []() __attribute__((always_inline)) { inl_child(); __asm__ volatile(""); };
    parent();
    inl_sibling();
}

// The everyday shape: a lambda passed to a standard algorithm. At -O2 both
// std::for_each and the lambda are inlined into the host (GCC does not
// instrument std::for_each at all; Clang's std filter drops its frames), so
// each lambda activation is an inlined internal-linkage frame at the host's
// level, entered three times in a row.
NOINLINE void inl_for_each_host() {
    const int values[3] = { 1, 2, 3 };
    std::for_each(values, values + 3, [](int value) {
        inl_child();
        __asm__ volatile("" ::"r"(value));
    });
    inl_sibling();
}

// The C part (inlined_c_part.c): a C function inlined into its host.
extern "C" void inl_c_host(void);

NOINLINE void inl_marker_d() {
    std::puts("inl_marker_d");
}

// clang-format on

int main(int argc, char**) {
    inl_catcher(argc);
    inl_marker_a();
    inl_loop_jump();
    inl_marker_b();
    inl_lambda_catcher(argc);
    inl_marker_c();
    inl_anon_host();
    inl_static_host();
    inl_lambda_host();
    inl_for_each_host();
    inl_c_host();
    inl_marker_d();
    return 0;
}
