/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * chdir() traced test program — compiled WITH -finstrument-functions.
 *
 * The main thread's trace file is opened lazily on main()'s enter hook, in the
 * startup working directory. The program then creates and chdir()s into a
 * subdirectory ("sub") BEFORE spawning a worker thread, whose own trace file is
 * opened lazily from the new directory. With a RELATIVE CSLG_OUTPUT_FILE this
 * used to scatter one run's files across two directories; trace_begin() now
 * anchors the base path to the startup directory, so ChdirTest asserts both
 * files land there and "sub" holds no trace file.
 *
 * Stdout: "CHDIR_OK" once the subdirectory switch succeeded.
 */

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int chdir_worker_leaf(int id) {
    return id * 3;
}

int chdir_worker_top(int id) {
    return chdir_worker_leaf(id);
}

void chdir_main_post_join(int id) {
    (void)chdir_worker_top(id);
}

int main() {
    // EEXIST is fine: the test runs the program in a fresh temp directory, but a
    // manual re-run in the same directory should not fail on the leftover "sub".
    if (mkdir("sub", 0700) != 0 && errno != EEXIST) {
        std::perror("mkdir");
        return 1;
    }
    if (chdir("sub") != 0) {
        std::perror("chdir");
        return 1;
    }
    std::cout << "CHDIR_OK" << std::endl;

    std::thread worker([]() { chdir_worker_top(1); });
    worker.join();

    chdir_main_post_join(2);
    return 0;
}
