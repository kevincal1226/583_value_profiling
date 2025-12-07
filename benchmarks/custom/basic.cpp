<<<<<<< HEAD
#include <cstdio>

int foo(int x) {
    return x + 8;
}
int bar(int y) {
    return y * 1000;
}

int main() {
    for (int i = 0; i < 10; ++i) {
        printf("%i\n", foo(i));
    }

    for (int i = 0; i < 10; ++i) {
        int x;
        if (i < 8) {
            x = bar(0);
        } else {
            x = bar(i);
        }
        printf("%i\n", x);
=======
#include <iostream>

int foo(int x) { return x + 8; }
int bar(int y) { return y * 1000; }

int main() {

    for (int i = 0; i < 1000; ++i) {
        std::cout << foo(i);
    }

    for (int i = 0; i < 1000; ++i) {
        std::cout << bar(0);
>>>>>>> d5bba08 (maybe a cook)
    }
}
