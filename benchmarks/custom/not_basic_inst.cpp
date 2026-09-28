#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/not_basic.cpp.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <cstdlib>

int bar(int y) {
    g_log_file << "[LOG] bar" << " " << "y=" << y << std::endl;
    g_param_freq["bar::y"]++;
    return y * 1000;
}

int baz(int x, int y) {
    g_log_file << "[LOG] baz" << " " << "x=" << x << "," << "y=" << y << std::endl;
    g_param_freq["baz::x"]++;
    g_param_freq["baz::y"]++;
    return x + y;
}

int main() {
    g_log_file << "[LOG] main no args" << std::endl;
    srand(69);

    int z {};
    for (int i = 0; i < 100; ++i) {
        int tester;
        if (i % 10 == 0) {
            tester = bar(rand());
        } else {
            tester = bar(0);
        }
        baz(i, i % 2);
        z += tester;
    }
    return z;
}
