#include <iostream>
#include <string>

class Klaska {
    public:
        static void foo() {
            std::cout<<"LOL\n";
        }
};

class Klaska2 {
    public:
        void foo() {
            std::cout<<"LOL2\n";
        }
};



int main()
{
    Klaska::foo();
    Klaska2 k2;
    k2.foo();
    std::cout<<"HELLO WORLD!"<<std::endl;
}