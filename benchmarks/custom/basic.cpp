#include <iostream>

int foo(int x) { return x + 8; }
int bar(int y) { return y * 1000; }

int main() {

    for (int i = 0; i < 1000; ++i) {
        std::cout << foo(i);
    }

    for (int i = 0; i < 1000; ++i) {
        std::cout << bar(0);
    }
}
