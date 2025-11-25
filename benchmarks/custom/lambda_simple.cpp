#include <functional>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::function<int(int)>> funcs;
    funcs.reserve(10000);

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
        funcs.push_back([x](int y) {
            return (y * 2) + x; // lambda operator()
        });
    }

    int sum = 0;
    for (auto &fn : funcs)
        sum += fn(5);

    std::cout << sum << "\n";
}
