#include <cstdlib>

int bar(int y) {
    return y * 1000;
}

int baz(int x, int y) {
    return x + y;
}

int main() {
    srand(69);

    int z {};
    for (int i = 0; i < 100; ++i) {
        int tester;
        if (i % 10 == 0) {
            tester = bar(rand());
        } else {
            tester = bar(0);
        }
        baz(i, i % 2);
        z += tester;
    }
    return z;
}
