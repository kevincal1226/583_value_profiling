#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/good.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <iostream>
#include <random>
using namespace std;

std::vector<int> setup() {

    std::vector<int> x;

    // Deterministic RNG
    std::mt19937 rng(12345); // fixed seed
    std::uniform_int_distribution<int> dist(500000, 1500000);

    // Random number of iterations (but reproducible each run)
    int N = dist(rng);
    cout << "Iterations: " << N << endl;

    for (int i = 0; i < N; i++) {
        // Make value distributions biased (great for value profiling)
        if (i % 7 == 0)
            x.push_back(67891201);
        else
            x.push_back(0);
    }

    return x;
}

int foo(int x) {
    g_log_file << "foo" << " " << "x=" << x << std::endl;
    g_param_freq["foo::x"]++;
    return sqrt(x) + x;
}


int main() {
    auto x = setup();

    long long z = 0;

    for (int y : x) {
        z += foo(y);
    }

    std::cout << z << std::endl;
}
