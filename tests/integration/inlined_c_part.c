/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * The C part of the inlined-frames driver (scenario D, see
 * inlined_traced_program.cpp): C names are not mangled, so neither compiler
 * emits a DW_AT_linkage_name for a C function, and a C function inlined into
 * its host is in the position an internal-linkage C++ function is in on GCC.
 * Compiled with the driver's flags (-finstrument-functions, -O2).
 */

#include <stdio.h>

#define NOINLINE __attribute__((noinline))
#define ALWAYS_INLINE inline __attribute__((always_inline))

NOINLINE void inl_c_child(void) {
    puts("inl_c_child");
}

NOINLINE void inl_c_sibling(void) {
    puts("inl_c_sibling");
}

static ALWAYS_INLINE void inl_c_parent(void) {
    inl_c_child();
    __asm__ volatile("");
}

NOINLINE void inl_c_host(void) {
    inl_c_parent();
    inl_c_sibling();
}
