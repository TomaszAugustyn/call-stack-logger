/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Fixture for DlopenReloadTest: the SMALLER of two instrumented plugins,
 * loaded after reload_lib_a was unloaded, into part of its old address range.
 * Its frames must be named after its own functions.
 */

#define NOINLINE __attribute__((noinline))

NOINLINE int reload_b_helper(int x) {
    __asm__ volatile("");
    return x * 5 + 2;
}

extern "C" int reload_b_entry(int x) {
    const int result = reload_b_helper(x) + 1;
    __asm__ volatile("");
    return result;
}
