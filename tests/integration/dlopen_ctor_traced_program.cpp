/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * dlopen()-constructor deadlock driver — compiled WITH -finstrument-functions.
 *
 * Thread A keeps the resolver on its cold path: cold<N>() is a chain of
 * distinct template instantiations, so every call is a never-seen callee at a
 * never-seen call site and goes through dladdr() plus BFD. Thread B meanwhile
 * dlopen()s and dlclose()s the instrumented plugin in a loop; each dlopen()
 * runs the plugin's static initializer with glibc's loader lock held, and that
 * initializer's enter hook takes the resolver mutex.
 *
 * A resolver that calls dladdr() (loader lock) while holding its own mutex
 * deadlocks against that: A holds the mutex and waits for the loader lock, B
 * holds the loader lock and waits for the mutex. The fixed resolver calls
 * dladdr() outside its mutex, so the program always finishes; the test runs it
 * under `timeout`.
 *
 * Thread B's loop is bounded. The two threads compete for the resolver mutex
 * on every iteration and the handoff is not fair: on some systems the cold
 * path is starved for a long time (the same binary measured between 1 s and
 * 42 s on Ubuntu 24.04, with or without sanitizers), which the test's timeout
 * would report as the deadlock. The deadlock itself strikes within the first
 * iterations, since the cold path starts with main() and every dlopen() runs
 * the initializer, and a deadlocked loader never reaches the bound — so the
 * bound keeps the run short without weakening what the test detects.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>

// One distinct function per N: a fresh callee and a fresh call site for every
// level, i.e. a resolver cache miss on each. noinline keeps every level a real
// function at any optimization level. The chain depth stays well below the
// compilers' default template-instantiation limit.
template <int N>
__attribute__((noinline)) int cold() {
    return cold<N - 1>() + 1;
}

template <>
__attribute__((noinline)) int cold<0>() {
    return 0;
}

std::atomic<bool> g_cold_done { false };

int main() {
    // Enough dlopen()/dlclose() rounds to overlap the whole cold path when the
    // handoff is fair (about two seconds of looping when it is not).
    constexpr int MAX_LOADER_ITERATIONS = 20000;
    std::thread loader([] {
        int iterations = 0;
        while (!g_cold_done.load() && iterations < MAX_LOADER_ITERATIONS) {
            void* handle = dlopen(DLOPEN_CTOR_PLUGIN_PATH, RTLD_NOW);
            if (handle == nullptr) {
                std::fprintf(stderr, "dlopen: %s\n", dlerror());
                std::fflush(stderr);
                std::_Exit(2);
            }
            dlclose(handle);
            ++iterations;
        }
        std::printf("loader iterations=%d\n", iterations);
    });

    // Three separate chains of 800 distinct functions each.
    int total = cold<800>();
    total += cold<799>();
    total += cold<798>();
    g_cold_done.store(true);
    loader.join();

    std::printf("cold total=%d\n", total);
    std::puts("DLOPEN_CTOR_DONE");
    return 0;
}
