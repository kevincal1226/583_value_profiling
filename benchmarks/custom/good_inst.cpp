#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/good.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <cmath>
#include <iostream>
#include <random>
#include <vector>

std::vector<int> setup() {

    std::vector<int> x;

    // Deterministic RNG
    std::mt19937 rng(12345); // fixed seed
    std::uniform_int_distribution<int> dist(500000, 1500000);

    // Random number of iterations (but reproducible each run)
    int N = dist(rng);
    std::cout << "Iterations: " << N << std::endl;

    for (int i = 0; i < N; i++) {
        // Make value distributions biased (great for value profiling)
        if (i % 7 == 0)
            x.push_back(dist(rng));
        else
            x.push_back(1);
    }

    return x;
}

int massive_from_one(long long x) {
    g_log_file << "massive_from_one" << " " << "x=" << x << std::endl;
    g_param_freq["massive_from_one::x"]++;

    double y = pow((double)x, 123456789.0) + // 1^N -> 1
               log(1.0 * x) +                // log(1) -> 0
               exp((double)x - 1.0) +        // exp(0) -> 1
               sin(M_PI * (double)x) +       // sin(pi) -> ~0
               cos(2.0 * M_PI * (double)x) + // cos(2π) -> 1
               tanh((double)x - 1.0) +       // tanh(0) -> 0
               sqrt((double)x * (double)x) + // sqrt(1) -> 1
               atan((double)x - 1.0) +       // atan(0) -> 0
               erf((double)x - 1.0);         // erf(0) -> 0

    // Mathematically:
    // y = 1 + 0 + 1 + 0 + 1 + 0 + 1 + 0 + 0 = 4

    return (int)y; // compiler can fold to `return 4;`
}

// int foo(long long x) { return pow(x, 10) + x; }

int main() {

    auto x = setup();

    for (int i = 0; i < 100; ++i) {
        long long z = 0;

        for (int y : x) {
            z += massive_from_one(y);
        }
        std::cout << z << std::endl;
    }
}
