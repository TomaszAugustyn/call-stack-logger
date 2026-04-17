/*
 * Traced test program — compiled WITH -finstrument-functions.
 *
 * Contains a variety of call patterns to exercise the instrumentation:
 * - Regular free function chain (func_a -> func_b -> func_c)
 * - Static member method
 * - Template function
 * - Constructor and non-static member method
 * - Inline function
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

    return 0;
}
