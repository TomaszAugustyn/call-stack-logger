/*
 * Copyright © 2020-2023 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

#include <algorithm>
#include <iostream>
#include <vector>

#include "callStack.h"

class A {
public:
    static void foo() { std::cout << "static foo \n"; }
};

class B {
public:
    B() { std::cout << "constructing B ...\n"; };
    void foo() {
        std::cout << "non-static foo \n";
        std::vector<int> vec{ 1, 55, 78, 3, 11, 7, 90 };
        std::sort(vec.begin(), vec.end());
        A::foo();
    }
};

constexpr unsigned fibonacci(unsigned n) {
    if (n <= 1)
        return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

void print() {
    std::cout << "empty function, called last.\n";
}

template <typename T, typename... Types>
void print(T var1, Types... var2) {
    std::cout << var1 << std::endl;
    print(var2...);
}

inline int cube(int s) {
    return s * s * s;
}

int main() {
    // Test logging static member methods.
    A::foo();

    // Test logging lambdas
    auto isLessThan = [](auto a, auto b) { return a < b; };
    bool out = isLessThan(3, 3.14);
    std::cout << "isLessThan: " << std::boolalpha << out << std::endl;

    // Test logging user-defined default constructor
    B b;
    // Test logging non-static member methods with calls to std
    // functions/containers (which should not be instrumented).
    b.foo();
    // Test logging constexpr function
    fibonacci(6);
    // Test logging variadic function templates
    print(44, 3.14159, "whatever\n");
    // Test logging inline function
    cube(3);

    return 0;
}
