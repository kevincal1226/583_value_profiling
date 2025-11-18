#include <cstdio>
#include <functional>

#include <stdio.h>

FILE* f = nullptr;

void hi() {
    // printf("memes");
}

void foo(int (*x)(int)) {
    x(3);
    // printf("%d", x(3));
}

int bar(int y) {
    return y * 1000;
}

int barf(int y) {
    // printf("barf");
    return y * 500;
}

int main() {
    // auto* q = fopen("logs/idfk.txt", "w");
    // printf("%p\n", q);
    // auto* q2 = fopen("logs/idfk.txt", "w");
    // printf("%p\n", q2);
    // fclose(q2);


    int (*x)(int) = bar;
    foo(bar);

    foo(barf);

    return 0;
}
