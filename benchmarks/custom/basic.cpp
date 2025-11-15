#include <stdio.h>

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
        printf("%i\n", bar(0));
    }
}
