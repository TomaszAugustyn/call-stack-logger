/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Fixture for CompressedDebugSectionsTest: an instrumented shared library built
 * with compressed debug sections (-gz=zlib, and -gz=zstd where the toolchain
 * supports it). Compiled once per compression with CDL_NAME set to the
 * compression's name, which prefixes the two symbols, so both libraries can
 * be linked into one program. The helper's call site inside the entry
 * function must resolve to this file and a line.
 */

#define CDL_CAT2(a, b) a##b
#define CDL_CAT(a, b) CDL_CAT2(a, b)
#define CDL_HELPER CDL_CAT(CDL_NAME, _debug_helper)
#define CDL_ENTRY CDL_CAT(CDL_NAME, _debug_entry)

__attribute__((noinline)) int CDL_HELPER(int x) {
    __asm__ volatile("");
    return x * 3;
}

extern "C" int CDL_ENTRY(int x) {
    const int result = CDL_HELPER(x) + 1;
    __asm__ volatile("");
    return result;
}
