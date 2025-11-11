#include <stdio.h>

int foo(int, int) {
    return 10;
}

int bar(int, int) {
    return 3;
}

int main() {
    for (int i = 0; i < 5; ++i) {
        int (*f)(int, int);
        if (i % 5 == 0) {
            f = foo;
        } else {
            f = bar;
        }

        int res = f(0, 0);

        printf("%d", res);
    }
    return 0;
}
