/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Fixture for DlopenReloadTest: the LARGER of two instrumented plugins. The
 * driver loads it, calls into it (so the resolver registers it), unloads it,
 * then loads the smaller reload_lib_b, which the kernel places at the top of
 * the freed hole: at a different base, over part of this library's old range.
 * A thousand padding functions give the library its size.
 */

#define NOINLINE __attribute__((noinline))

NOINLINE int reload_a_helper(int x) {
    return x * 3 + 1;
}

extern "C" int reload_a_entry(int x) {
    return reload_a_helper(x) + 1;
}

#define RELOAD_A_PAD(n) NOINLINE int reload_a_pad##n(int x) { volatile int y = x; return y * n + n; }
#define RELOAD_A_PAD10(n) \
    RELOAD_A_PAD(n##0) RELOAD_A_PAD(n##1) RELOAD_A_PAD(n##2) RELOAD_A_PAD(n##3) RELOAD_A_PAD(n##4) \
    RELOAD_A_PAD(n##5) RELOAD_A_PAD(n##6) RELOAD_A_PAD(n##7) RELOAD_A_PAD(n##8) RELOAD_A_PAD(n##9)
#define RELOAD_A_PAD100(n) \
    RELOAD_A_PAD10(n##0) RELOAD_A_PAD10(n##1) RELOAD_A_PAD10(n##2) RELOAD_A_PAD10(n##3) RELOAD_A_PAD10(n##4) \
    RELOAD_A_PAD10(n##5) RELOAD_A_PAD10(n##6) RELOAD_A_PAD10(n##7) RELOAD_A_PAD10(n##8) RELOAD_A_PAD10(n##9)
RELOAD_A_PAD100(1) RELOAD_A_PAD100(2) RELOAD_A_PAD100(3) RELOAD_A_PAD100(4) RELOAD_A_PAD100(5)
RELOAD_A_PAD100(6) RELOAD_A_PAD100(7) RELOAD_A_PAD100(8) RELOAD_A_PAD100(9)
