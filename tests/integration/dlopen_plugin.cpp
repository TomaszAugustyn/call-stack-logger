/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented plugin for the dlopen()/chdir() regression test. Built as a
 * shared library WITH -finstrument-functions (see CMakeLists.txt), loaded by
 * dlopen_traced_program via a RELATIVE path, and first entered after the host
 * chdir()ed away — so the resolver must find this object's file through
 * /proc/self/maps rather than the relative dli_fname (see DlopenPluginTest).
 *
 * Both functions have external linkage: they are in .dynsym, so dladdr()
 * names them and they get traced. noinline plus the asm barrier keep the
 * entry → helper call a genuine (non-tail) call at every optimization level,
 * so the helper's caller location stays inside this file.
 */

__attribute__((noinline)) int dlopen_plugin_helper(int x) {
    __asm__ volatile("");
    return x * 2;
}

extern "C" int dlopen_plugin_entry(int x) {
    const int result = dlopen_plugin_helper(x) + 1;
    __asm__ volatile("");
    return result;
}
