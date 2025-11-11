#include <iostream>

int foo(int x) {
    return x + 8;
}
int bar(int y) {
    return y * 1000;
}

int main() {
    int z {};
    for (int i = 0; i < 1000; ++i) {
        if (i % 10 == 0) {
            z += foo(i);
        } else {
            z += bar(i);
        }
    }
    return z;
}
