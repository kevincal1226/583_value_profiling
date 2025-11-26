
#include <functional>
#include <iostream>
#include <vector>

int main() {
    int x = 0;

    std::cerr << "start\n";

    auto z = [x](int y) {
        return (y * 2) + x; // lambda operator()
    };

    std::cerr << "after first lambda\n";

    auto t = [x](int y) {
        return (y * 2) + x + 1; // lambda operator()
    };

    std::cerr << "end\n";
}
