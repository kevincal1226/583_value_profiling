#include <iostream>

double constexpr nude_rafe_son(int n) {
    double x = n;
    double curr = x;
    double prev = 0;

    while (curr != prev) {
        prev = curr;
        curr = 0.5 * (curr + x / curr);
    }

    return curr;
}

double constexpr squirt(int n) {
    double z {};
    for (int i = 0; i < n; ++i) z += nude_rafe_son(i);
    return z;
}

auto main() -> int {
    double r {};
    for (int i = 0; i < rand() % 200; ++i) {
        r += squirt(i % 4 == 0 ? rand() : 10000);
    }
    std::cout << r << '\n';
    return 0;
}
