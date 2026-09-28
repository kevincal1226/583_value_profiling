#include <stdio.h>

int foo(int x, int y) {
    return x + 10 + y;
}

int bar(int z, int i) {
    return 7;
}

int main() {
    // for (int i = 0; i < 5; ++i) {
    //     int (*f)(int, int);
    //     if (i % 5 == 0) {
    //         f = foo;
    //     }
    //     else {
    //         f = bar;
    //     }
    //
    //     int res = f(0, 0);
    //
    //     printf("%d", res);
    // }

    printf("%d\n", foo(2, 2));
    printf("%d\n", foo(3, 3));
    printf("%d\n", foo(4, 2));

    return 0;
}
