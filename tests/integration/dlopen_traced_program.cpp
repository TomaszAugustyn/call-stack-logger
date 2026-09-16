/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented driver for the dlopen()/chdir() regression test — compiled WITH
 * -finstrument-functions. It changes into the plugin's directory, dlopen()s
 * the instrumented plugin by a RELATIVE path ("./<file>"), then chdir()s to
 * "/" BEFORE the first traced call into the plugin.
 *
 * dladdr() reports the relative dlopen string as dli_fname. The resolver
 * opens the object file lazily, at first sight of an address inside it — here
 * after the chdir — so a resolver that hands dli_fname to bfd_openr() opens
 * nothing (or, if a same-named file exists in the new directory, the wrong
 * object) and every plugin frame degrades to "<could not open object file>".
 * DLOPEN_PLUGIN_DIR / DLOPEN_PLUGIN_FILE come from CMake.
 */

#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>

int main() {
    if (chdir(DLOPEN_PLUGIN_DIR) != 0) {
        std::perror("chdir to plugin dir");
        return 1;
    }
    void* handle = dlopen("./" DLOPEN_PLUGIN_FILE, RTLD_NOW);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    if (chdir("/") != 0) {
        std::perror("chdir /");
        return 1;
    }
    auto entry = reinterpret_cast<int (*)(int)>(dlsym(handle, "dlopen_plugin_entry"));
    if (entry == nullptr) {
        std::fprintf(stderr, "dlsym: %s\n", dlerror());
        return 1;
    }
    std::printf("plugin result=%d\n", entry(5));
    std::puts("DLOPEN_PLUGIN_DONE");
    return 0;
}
