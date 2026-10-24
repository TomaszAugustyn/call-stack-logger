/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Fixture for SeparateDebugFileTest: an instrumented shared library whose
 * debug information is moved to a separate file after the build (objcopy
 * --only-keep-debug, --strip-debug, --add-gnu-debuglink — see CMakeLists.txt).
 * The host traces its functions; the helper's call site inside
 * separate_debug_entry must resolve to this file and a line, which only the
 * separate file can supply. noinline keeps the helper a real call at every
 * optimization level.
 */

__attribute__((noinline)) int separate_debug_helper(int x) {
    __asm__ volatile("");
    return x * 2;
}

extern "C" int separate_debug_entry(int x) {
    const int result = separate_debug_helper(x) + 1;
    __asm__ volatile("");
    return result;
}
