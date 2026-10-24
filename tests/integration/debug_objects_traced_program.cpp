/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented driver for SeparateDebugFileTest and
 * CompressedDebugSectionsTest: calls into the fixture libraries whose debug
 * information lives in a separate file or in compressed sections. The tests
 * assert that the call sites inside those libraries resolve to file and line.
 */

#include <cstdio>

extern "C" int separate_debug_entry(int x);
extern "C" int zlib_debug_entry(int x);
#ifdef CSLG_HAVE_ZSTD_DEBUG
extern "C" int zstd_debug_entry(int x);
#endif

int main() {
    std::printf("separate=%d\n", separate_debug_entry(3));
    std::printf("zlib=%d\n", zlib_debug_entry(3));
#ifdef CSLG_HAVE_ZSTD_DEBUG
    std::printf("zstd=%d\n", zstd_debug_entry(3));
#endif
    std::puts("DEBUG_OBJECTS_DONE");
    return 0;
}
