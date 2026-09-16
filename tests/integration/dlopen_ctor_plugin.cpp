/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented plugin for the dlopen()-constructor deadlock test. Built as a
 * shared library WITH -finstrument-functions and not linked against the
 * library: its hooks bind to the host executable's exported
 * __cyg_profile_func_* symbols when the host dlopen()s it.
 *
 * The global object below gives the plugin a static initializer. glibc runs
 * it from inside dlopen() with the loader lock held, and — because the
 * initializer and the constructor are instrumented — the enter hook fires
 * right there and takes the resolver mutex on the dlopen() thread. See
 * DlopenConstructorTest in test_integration.cpp.
 */

int dlopen_ctor_traced(int x) {
    return x + 1;
}

struct InitOnLoad {
    InitOnLoad() { (void)dlopen_ctor_traced(1); }
};

InitOnLoad g_init_on_load;

extern "C" int dlopen_ctor_plugin_entry() {
    return 7;
}
