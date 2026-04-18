/*
 * Multi-threaded traced test program — compiled WITH -finstrument-functions.
 *
 * Spawns N worker threads, each of which executes a small distinguishable call
 * chain (worker_top -> worker_mid -> worker_leaf). After joining, main also runs
 * its own call chain so the main trace file has post-join content.
 *
 * The integration tests run this program and verify:
 * - N+1 trace files exist (one main + N worker)
 * - Workers' call chains appear only in their own file (no cross-contamination)
 * - Main's post-join content appears only in main's file
 * - Each file's "thread ID: <n>" header matches the file's _tid_<n> suffix
 *
 * Stdout: the first line prints "MAIN_TID=<n>" so the fixture can correlate main's
 * file (which has no _tid_ suffix) with its expected thread ID value.
 */

#include <iostream>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

int worker_leaf(int id) {
    return id * 2;
}

int worker_mid(int id) {
    return worker_leaf(id) + 1;
}

int worker_top(int id) {
    return worker_mid(id);
}

void main_only_post_join(int id) {
    // A function called ONLY from main after join, to prove isolation.
    (void)worker_top(id);
}

int main() {
    constexpr int kNumWorkers = 4;

    // Print the main thread's TID so the integration test can verify the main
    // file's header contains the correct thread ID. Using syscall(SYS_gettid)
    // directly (same mechanism trace.cpp uses for the filename/header).
    const long main_tid = syscall(SYS_gettid);
    std::cout << "MAIN_TID=" << main_tid << std::endl;

    // Spawn workers. Each worker calls worker_top(i) exactly once.
    std::vector<std::thread> threads;
    threads.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
        threads.emplace_back([i]() { worker_top(i); });
    }
    for (auto& t : threads) {
        t.join();
    }

    // After all workers joined, exercise the main thread again so the main
    // file has content beyond the run-separator header.
    main_only_post_join(99);

    return 0;
}
