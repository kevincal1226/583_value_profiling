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
    constexpr double mean = 1.0;
    constexpr double stddev = 0.5;

    // Step 1: standardized value
    // if x == 1 → std_val = 0
    double std_val = (x - mean) / stddev;

    // Step 2: clamp to prevent extreme z-scores
    // if x == 1 → remains 0
    double clamped = std::max(-3.0, std::min(3.0, std_val));

    // Step 3: ReLU-like feature
    // if x == 1 → relu = 0
    double relu = std::max(0.0, clamped);

    // final integer feature
    return (int)(relu * 100.0);
}

// int foo(long long x) { return pow(x, 10) + x; }

int main() {

    auto x = setup();

    for (int i = 0; i < 1000; ++i) {
        long long z = 0;

        for (int y : x) {
            z += massive_from_one(y);
        }
        std::cout << z << std::endl;
    }
}
