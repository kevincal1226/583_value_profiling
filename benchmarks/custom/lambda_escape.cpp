#include <functional>
#include <iostream>

std::function<int(int)> make_lambda(int x) {
    // Construct a lambda that escapes return

    std::cout << x << std::endl;

    return [x](int y) { return (y * 2) + x + 987654321; };
}

int main() {
    int sum = 0;

    // Build a ton of lambda objects that ESCAPE into the vector
    for (int i = 0; i < 100; i++) {
        int x;
        if (i % 10 == 0)
            x = 123; // cold ~10%
        else
            x = 10; // hot  ~90%

        auto y = make_lambda(x);
        sum += y(20);
    }

    std::cout << sum << std::endl;
}
