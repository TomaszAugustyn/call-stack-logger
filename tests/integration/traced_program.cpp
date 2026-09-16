/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Traced test program — compiled WITH -finstrument-functions.
 *
 * Contains a variety of call patterns to exercise the instrumentation:
 * - Regular free function chain (func_a -> func_b -> func_c)
 * - Static member method
 * - Template function
 * - Constructor and non-static member method
 * - Inline function
 * - Self-recursion (same callee address at several depths)
 * - Internal-linkage functions (static, anonymous namespace, lambda) — these
 *   have no dynamic symbol, so dladdr() cannot name them and the resolver must
 *   fall through to BFD's symtab / DWARF lookup
 *
 * The integration tests execute this program and parse its trace output.
 */

#include <algorithm>
#include <iostream>
#include <vector>

class TracedClass {
public:
    TracedClass() { std::cout << "TracedClass constructed\n"; }

    static void static_method() { std::cout << "static_method\n"; }

    void instance_method() { std::cout << "instance_method\n"; }
};

template <typename T>
void template_func(T value) {
    std::cout << "template_func: " << value << "\n";
}

inline int inline_func(int x) {
    return x * x;
}

// Exercises STL containers and algorithms — std library internals (std::vector,
// std::sort, __gnu_cxx::__normal_iterator, etc.) should NOT appear in the trace.
void func_with_stl() {
    std::vector<int> vec{1, 55, 78, 3, 11, 7, 90};
    std::sort(vec.begin(), vec.end());
}

// Self-recursive: produces repeated frames of the SAME callee at increasing
// depths, exercising the LIFO frame-resolution stack (and, in the LOG_ELAPSED
// variants, the per-frame enter-time/offset slots under same-callee stacking).
// The addition after the recursive call keeps it a genuine non-tail call.
int recursive_countdown(int n) {
    if (n <= 1) {
        return 1;
    }
    return n + recursive_countdown(n - 1);
}

// Internal linkage: no .dynsym entry, so dladdr() reports dli_sname == nullptr
// for both. They must still be traced, named via BFD (see
// InternalLinkageFunctionsResolved in test_integration.cpp).
static int file_static_func(int x) {
    return x + 1;
}

namespace {
int anon_ns_func(int x) {
    return x + 2;
}
} // namespace

void func_c() {
    std::cout << "func_c (leaf)\n";
}

void func_b() {
    func_c();
}

void func_a() {
    func_b();
}

int main() {
    // Free function chain: main -> func_a -> func_b -> func_c
    func_a();

    // Static member method
    TracedClass::static_method();

    // Constructor + instance method
    TracedClass obj;
    obj.instance_method();

    // Template function (int specialization)
    template_func(42);

    // Inline function
    int result = inline_func(3);
    (void)result; // suppress unused warning

    // STL usage — only func_with_stl should appear in trace, not std:: internals
    func_with_stl();

    // Recursion: 3 nested frames of the same function
    int sum = recursive_countdown(3);
    (void)sum; // suppress unused warning

    // Internal-linkage callees: static, anonymous-namespace, and a lambda
    // (its operator() is a member of a local class — also internal linkage).
    // The argument is read from a volatile so the lambda call cannot be
    // constant-folded: lambdas are implicitly constexpr, and GCC folds a call
    // with constant arguments even at -O0, emitting no operator() at all.
    volatile int seed = 1;
    auto lambda_func = [](int v) { return v + 3; };
    int internal = file_static_func(seed) + anon_ns_func(seed) + lambda_func(seed);
    (void)internal; // suppress unused warning

    return 0;
}
