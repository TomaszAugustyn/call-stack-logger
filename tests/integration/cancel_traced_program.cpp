/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * pthread_cancel driver — compiled WITH -finstrument-functions.
 *
 * A worker thread loops on a traced leaf function while main cancels it with
 * deferred cancellation (the default type). The hooks contain cancellation
 * points (the trace line write, the lazy file open), and the compiler emits
 * the hook call as non-throwing, so a cancellation that lands inside a hook
 * cannot unwind out of it: the process used to die with SIGABRT. The hooks
 * now block cancellation for their duration, which makes them opaque to
 * pthread_cancel.
 *
 * Two modes, selected by the first argument:
 *   "testcancel" — the worker loop calls pthread_testcancel(), a cancellation
 *                  point of the program's own; the request must be honored
 *                  there and the thread ends with PTHREAD_CANCELED.
 *   (none)       — the loop has no cancellation point; instrumentation must
 *                  not add one, so the request is never honored, the timed
 *                  join expires, and main stops the worker cooperatively.
 * Either way the program must finish normally — never abort.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

std::atomic<bool> g_stop { false };

int traced_leaf(int x) {
    return x + 1;
}

void* worker(void* arg) {
    const bool own_cancellation_point = arg != nullptr;
    long acc = 0;
    while (!g_stop.load()) {
        acc += traced_leaf(static_cast<int>(acc & 0xff));
        if (own_cancellation_point) {
            pthread_testcancel();
        }
    }
    return nullptr;
}

int main(int argc, char** argv) {
    const bool testcancel = argc > 1 && std::strcmp(argv[1], "testcancel") == 0;

    pthread_t thread;
    if (pthread_create(&thread, nullptr, worker, testcancel ? &thread : nullptr) != 0) {
        std::perror("pthread_create");
        return 1;
    }
    usleep(20'000); // let the worker trace a few thousand calls first
    pthread_cancel(thread);

    timespec deadline {};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    void* result = nullptr;
    const int rc = pthread_timedjoin_np(thread, &result, &deadline);
    if (rc == 0) {
        std::printf("JOINED: %s\n", result == PTHREAD_CANCELED ? "PTHREAD_CANCELED" : "other");
    } else {
        std::puts("JOIN_TIMEOUT");
        g_stop.store(true);
        pthread_join(thread, &result);
    }
    std::puts("CANCEL_PROGRAM_DONE");
    return 0;
}
