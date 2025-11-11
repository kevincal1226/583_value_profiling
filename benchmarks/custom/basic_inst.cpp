#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/basic.cpp.txt");
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

int baz(int x, int y) {
    g_log_file << "[LOG] baz" << " " << "x=" << x << " " << "y=" << y << std::endl;
    g_param_freq["baz::x"]++;
    g_param_freq["baz::y"]++;
    return 0;
}

int main() {
    g_log_file << "[LOG] main no args" << std::endl;

    for (int i = 0; i < 1000; ++i) {
        std::cout << foo(i);
    }

    for (int i = 0; i < 1000; ++i) {
        std::cout << bar(0);
    }
}
