/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

// A small ordinary program: a few functions, a container, an exception. Built
// twice by the CMake project next to it — with callstacklogger::instrumented
// in its link line and without — and the two binaries must come out alike
// when DISABLE_INSTRUMENTATION is on.

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

int leaf(int x) { return x * 2 + 1; }

int middle(int x) {
    if (x > 1000) {
        throw std::runtime_error("too large");
    }
    return leaf(x) + 1;
}

} // namespace

int main(int argc, char**) {
    std::vector<int> values;
    for (int i = 0; i < 10; ++i) {
        values.push_back(middle(i + argc));
    }
    try {
        middle(5000);
    } catch (const std::exception& e) {
        std::printf("%s\n", e.what());
    }
    return values.size() == 10 ? 0 : 1;
}
