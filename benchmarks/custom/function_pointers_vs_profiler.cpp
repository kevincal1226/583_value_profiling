#include <functional>

#include <stdio.h>

void hi() {
    printf("memes");
}

void foo(int (*x)(int)) {
    printf("%d", x(3));
}

int bar(int y) {
    return y * 1000;
}

int barf(int y) {
    printf("barf");
    return y * 500;
}

int main() {
    int (*x)(int) = bar;
    foo(bar);

    foo(barf);

    return x(3);
}
