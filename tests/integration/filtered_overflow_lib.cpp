/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Shared library for the filtered-overflow test. Compiled WITH
 * -finstrument-functions and stripped of its symbol table and debug info after
 * the build (see CMakeLists.txt). hidden_helper() is file-local: after stripping
 * it has neither a dynamic symbol nor a symbol-table entry, so dladdr() and BFD
 * both fail to name it and the resolver drops its frame — but its enter hook
 * still fires. That gives filtered_overflow_program an "entered but not logged"
 * frame at every recursion level, which is what the depth accounting must
 * survive past the frame stack's initial capacity. filtered_overflow_call() is
 * exported, keeps its dynamic symbol and is therefore logged (with the
 * "<bfd_error>" suffix, since the stripped object has no line info). noinline
 * and the asm barriers keep both calls genuine at any optimization level.
 */

__attribute__((noinline)) static int hidden_helper(int n) {
    __asm__ volatile("");
    return n & 1;
}

extern "C" int filtered_overflow_call(int n) {
    const int r = hidden_helper(n);
    __asm__ volatile("");
    return r;
}
