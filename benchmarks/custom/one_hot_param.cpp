#include <stdio.h>

int foo(int x, int y) {
    return x * y;
}

int main() {
    for (int i = 0; i < 10; ++i) {
        printf("%i\n", foo(10, i));
    }
}
