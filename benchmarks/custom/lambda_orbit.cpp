/**
 * ============================================================================
 *  Benchmark: Specializing Captured Loop Bounds in Escaping Lambdas
 * ============================================================================
 *
 *  Real-world analogy:
 *  -------------------
 *  Many physics / simulation / rendering systems apply "update rules" that
 *  iterate a state function multiple times. Example domains:
 *
 *      • Game physics (relaxation iterations for soft bodies)
 *      • Fluid simulation refinement passes
 *      • Orbital mechanics approximation in space games
 *      • Iterated function systems in graphics (fractals, chaos visualization)
 *
 *  Typical pattern:
 *
 *      state = F(state) repeated <iters> times
 *
 *  Performance insight:
 *  --------------------
 *  In *almost all* cases, a low iteration count (e.g., iters=2) is used because
 *  accuracy is "good enough." Rarely (e.g., ~2% of cases), we perform a
 *  refinement pass such as iters=20 which is much more expensive but rarely
 *  triggered.
 *
 *  This benchmark models that exactly:
 *
 *      - We create 1,000,000 update-rule lambdas
 *      - Each lambda *captures* the iteration bound `iters` (int)
 *      - ~98% have iters = HOT_ITERS (cheap)
 *      - ~2% have iters = COLD_ITERS (expensive)
 *
 *  We measure two behaviors:
 *
 *      (1) Direct calls to the lambda (non-escaping case)
 *          → Should trigger the DirectLambdaSpecializer in your pass.
 *
 *      (2) Wrapping the lambda in std::function and storing in a vector
 *          → Lambda object *escapes* into dynamic type erasure
 *          → Should trigger the EscapingLambdaSpecializer in your pass:
 *              * Branch *once* at construction
 *              * Use a clone of operator() with the hot field baked in
 *
 *  Why this test helps your pass:
 *  ------------------------------
 *  • The captured field is an integer → exactly what your pass specializes
 *  • The hot value dominates → profile freq >= 0.8 → kept by loadCaptureProfiles()
 *  • Specialization removes loads and constant-folds the loop bound
 *       → Fewer instructions
 *       → Smaller working set
 *       → Better branch prediction
 *  • std::function call-through becomes cheap in the common case
 *  • Result should show a clear speedup after applying your pass
 *
 *  TL;DR:
 *  ------
 *  Realistic simulation-style compute, dominated by the captured iteration bound.
 *  Hot specialization collapses the loop to a tiny unrolled body.
 *  This perfectly demonstrates the benefit of your escaping lambda optimization.
 *
 * ============================================================================
 */

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <vector>

using OrbitFunc = std::function<double(double)>;

// Expected hot capture value from profiling
constexpr int HOT_ITERS = 2;
constexpr int COLD_ITERS = 20;

int main() {
    constexpr int N_FUNCS = 1'000'000; // number of distinct lambdas
    constexpr int N_CALLS = 200;       // calls per function in the hot loop

    std::vector<OrbitFunc> funcs;
    funcs.reserve(N_FUNCS);

    double direct_sum = 0.0;

    for (int i = 0; i < N_FUNCS; ++i) {
        // ~98% chance of HOT_ITERS
        int iters = (i % 50 == 0) ? COLD_ITERS : HOT_ITERS;

        double r = 3.7; // logistic map parameter (chaotic system)

        // Lambda capturing the iteration count → this is the optimization target
        OrbitFunc f = [iters, r](double x) {
            double v = x;
            for (int k = 0; k < iters; ++k) {
                v = r * v * (1.0 - v);
            }
            return std::sin(v) + std::cos(2.0 * v);
        };

        // Non-escaping case: direct call
        direct_sum += f(0.123 * (i + 1));

        // Escaping case: convert into std::function
        funcs.emplace_back(std::move(f));
    }

    auto start = std::chrono::high_resolution_clock::now();

    double sum = direct_sum;

    // Hot loop: repeatedly call std::function objects
    for (int rep = 0; rep < N_CALLS; ++rep) {
        double x = 0.001 * (rep + 1);
        for (auto &f : funcs) {
            sum += f(x);
            x = std::fmod(x + 0.000001 * sum, 1.0); // keep x bounded
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(end - start).count();

    std::cout << "sum = " << sum << "\n";
    std::cout << "time = " << dt << " s\n";
}
