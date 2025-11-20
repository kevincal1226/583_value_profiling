#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/newton_raphson.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <iostream>

double constexpr nude_rafe_son(int n) {
    g_log_file << "nude_rafe_son" << " " << "n=" << n << std::endl;
    g_param_freq["nude_rafe_son::n"]++;
    double x = n;
    double curr = x;
    double prev = 0;

    while (curr != prev) {
        prev = curr;
        curr = 0.5 * (curr + x / curr);
    }

    return curr;
}

double constexpr squirt(int n) {
    g_log_file << "squirt" << " " << "n=" << n << std::endl;
    g_param_freq["squirt::n"]++;
    double z {};
    for (int i = 0; i < n; ++i) z += nude_rafe_son(i);
    return z;
}

auto main() -> int {
    double r {};
    for (int i = 0; i < rand() % 200; ++i) {
        r += squirt(i % 4 == 0 ? rand() : 10000);
    }
    std::cout << r << '\n';
    return 0;
}
