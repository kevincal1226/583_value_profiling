#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/one_hot_param.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <stdio.h>

int foo(int x, int y) {
    g_log_file << "foo" << " " << "x=" << x << "," << "y=" << y << std::endl;
    g_param_freq["foo::x"]++;
    g_param_freq["foo::y"]++;
    return x * y;
}

int main() {
    for (int i = 0; i < 10; ++i) {
        printf("%i\n", foo(10, i));
    }
}
