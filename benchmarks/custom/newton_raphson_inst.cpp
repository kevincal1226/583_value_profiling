#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/newton_raphson.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <iostream>

double constexpr nude_rafe_son(int n) {
    g_log_file << "nude_rafe_son" << " " << "n=" << n << std::endl;
    g_param_freq["nude_rafe_son::n"]++;
    if (n == 0) return 0;
    double x = n;
    double curr = x;
    double prev = 0;

    do {
        prev = curr;
        curr = 0.5 * (curr + x / curr);
    } while (std::abs(curr - prev) > .00001);

    return curr;
}

double constexpr squirt(int n) {
    g_log_file << "squirt" << " " << "n=" << n << std::endl;
    g_param_freq["squirt::n"]++;
    double z {};
    for (int i = 1; i < n; ++i) z += nude_rafe_son(i);
    return z;
}

auto main() -> int {
    double r {};
    for (int i = 0; i < 10000; ++i) {
        r += squirt(i % 4 == 0 && i == 0 ? rand() % 200 : 10000);
    }
    std::cout << r << '\n';
    return 0;
}
