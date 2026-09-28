#include <iostream>

inline int bar(int a, int b, int c, int d) {
    return a * b * c * d;
}

inline int foo(int a, int b, int c, int d) {
    return sqrt(a * b * c * d);
}

int main() {
    int volatile total = 0;
    int (*x)(int, int, int, int) = bar;
    for (int i = 0; i < 1000000000; ++i) {
        total += x(i, i + 1, i + 2, i + 3);

        if (i % 100000) {
            x = foo;
        } else {
            x = bar;
        }
    }

    printf("%d", total);

    return 0;
}
