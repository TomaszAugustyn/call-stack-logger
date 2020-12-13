#include <iostream>
#include <vector>
#include <algorithm>

#include "callStack.h"

class A {
    public:
        static void foo() {
            std::cout << "static foo \n";
        }
};

class B {
    public:
        void foo() {
            std::cout << "non-static foo \n";
            std::vector<int> vec{ 1, 55, 78, 3, 11, 7, 90 };
            std::sort(vec.begin(), vec.end());
            A::foo();
        }
};

int main()
{
    // Test logging static member methods.
    A::foo();

    // Test logging non-static member methods with calls
    // to std functions/containers (which should not be instrumented).
    B b;
    b.foo();
    std::cout << "HELLO WORLD!" << std::endl;
}