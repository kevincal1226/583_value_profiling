int main() {
    int x = 0;
    int a = 4;
    int b = 5;

    for (int i = 0; i < 100; ++i) {
        x = i + 7;

        auto z = [x, a, b](int y) {
            return (y * 2) + x + a + b; // lambda operator()
        };

        auto t = [x, b](int y) {
            return (y * 2) + x + 1 + b; // lambda operator()
        };
    }
}
