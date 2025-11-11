#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/not_basic.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <iostream>

int foo(int x) {
    g_log_file << "[LOG] foo" << " " << "x=" << x << std::endl;
    g_param_freq["foo::x"]++;
    return x + 8;
}
int bar(int y) {
    g_log_file << "[LOG] bar" << " " << "y=" << y << std::endl;
    g_param_freq["bar::y"]++;
    return y * 1000;
}

int main() {
    g_log_file << "[LOG] main no args" << std::endl;
    int z {};
    for (int i = 0; i < 1000; ++i) {
        if (i % 10 == 0) {
            z += foo(i);
        } else {
            z += bar(i);
        }
    }
    return z;
}
