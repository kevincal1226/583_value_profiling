#include <cstdio>
#include <functional>

#include <stdio.h>

FILE* f = nullptr;

void hi() {
    // printf("memes");
}

// void foo(int (*x)(int), int a, int b, int c, int d) {
//     x(a, b, c, d);
//     // printf("%d", x(3));
// }

inline int bar(int a, int b, int c, int d) {
    return a * b * c * d;
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

    int (*x)(int, int, int, int) = bar;
    for (int i = 0; i < 500000000; ++i) {
        int total = x(i, i + 1, i + 2, i + 3);
        total * 2;

        // foo(barf);
        printf("%d", total);
    }

    return 0;
}
