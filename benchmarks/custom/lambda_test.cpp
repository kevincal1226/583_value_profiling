
#include <functional>
#include <iostream>
#include <vector>

int main() {
    int x = 0;

    auto z = [x](int y) {
        return (y * 2) + x; // lambda operator()
    };
}
