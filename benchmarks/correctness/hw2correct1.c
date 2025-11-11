#include <stdio.h>

int foo(int, int) {
    return 10;
}

int bar(int, int) {
    return 3;
}

int main() {
    int (*f)(int, int) = bar;
    int res = f(0, 0);

    printf("%d", res);
    return 0;
}
