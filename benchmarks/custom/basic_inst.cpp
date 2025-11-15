#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/basic.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <stdio.h>

int foo(int x) {
    g_log_file << "foo" << " " << "x=" << x << std::endl;
    g_param_freq["foo::x"]++;
    return x + 8;
}
int bar(int y) {
    g_log_file << "bar" << " " << "y=" << y << std::endl;
    g_param_freq["bar::y"]++;
    return y * 1000;
}

int main() {
    for (int i = 0; i < 10; ++i) {
        printf("%i\n", foo(i));
    }

    for (int i = 0; i < 10; ++i) {
        int x;
        if (i < 8) {
            x = bar(0);
        } else {
            x = bar(i);
        }
        printf("%i\n", x);
    }
}
