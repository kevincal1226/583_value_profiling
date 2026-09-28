#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/good2.cpp.txt");
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
    // For x == 1 → norm = 1 / (1 + 0) = 1
    double norm = (double)x / (1.0 + std::abs(x - 1));

    // Step 2: clamp to safe numeric range
    // For x == 1 → still 1
    double clamped = std::min(2.0, std::max(0.0, norm));

    // Step 3: smooth nonlinearity (logistic-ish)
    // For x == 1 → sigmoid(1) ≈ 0.731...
    double activation = 1.0 / (1.0 + std::exp(-clamped));

    // Convert to integer feature
    return (int)(activation * 100);
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
