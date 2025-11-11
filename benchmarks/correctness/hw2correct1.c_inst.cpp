#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("runtime_log.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <stdio.h>

int foo(int, int) { return 10; }

int bar(int x, int y) {
    g_log_file << "function bar" << " " << "x=" << x << " " << "y=" << y << std::endl;
    g_param_freq["bar::x"]++;
    g_param_freq["bar::y"]++;
    return 3;
}


int main() {
    int (*f)(int, int) = bar;
    int res = f(0, 0);

    printf("%d", res);
    return 0;
}
