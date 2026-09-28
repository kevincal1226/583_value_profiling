#include <functional>
#include <iostream>
#include <vector>

int main() {
    int sum = 0;
    for (int i = 0; i < 10000; i++) {
        int x;

        // x = 10 in 90% of cases
        if (i % 10 == 0) {
            x = 123; // cold capture value
        }
        else {
            x = 10; // HOT capture value
        }

        // each loop iteration constructs a NEW lambda closure
        auto z = [x](int y) {
            return (y * 2) + x; // lambda operator()
        };

        sum += z(100);
    }

    std::cout << sum << std::endl;
}
