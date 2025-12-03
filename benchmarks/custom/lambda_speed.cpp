#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <vector>

using NormFunc = std::function<double(double, double, double)>;

NormFunc make_norm(bool use_z) {
    return [use_z](double x, double y, double z) {
        if (use_z) {
            // cold path: full 3D magnitude
            return std::sqrt(x * x + y * y + z * z);
        }

        // hot path: 2D magnitude — significantly cheaper
        return std::sqrt(x * x + y * y);
    };
}

int main() {
    constexpr int N_FUNCS = 1'000'000;
    constexpr int N_CALLS = 3; // calls per function

    std::vector<NormFunc> funcs;
    funcs.reserve(N_FUNCS);

    // 95% of the time, use only XY normalization (hot)
    for (int i = 0; i < N_FUNCS; i++) {
        bool use_z = (i % 20 == 0); // ~5% true → cold
        funcs.emplace_back(make_norm(use_z));
    }

    double sum = 0.0;
    auto start = std::chrono::high_resolution_clock::now();

    for (int rep = 0; rep < N_CALLS; rep++) {
        double t = rep + 1.0;
        for (auto &f : funcs) {
            sum += f(t, t * 0.5, t * 0.1);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "sum = " << sum << "\n";
    std::cout << "dt = " << std::chrono::duration<double>(end - start).count() << " s\n";
}
