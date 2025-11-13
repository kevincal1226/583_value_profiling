#include <fstream>
#include <unordered_map>
#include <string>
std::ofstream g_log_file("logs/hw2correct1.c.txt");
std::unordered_map<std::string, size_t> g_param_freq;

#include <stdio.h>

int foo(int x, int y) {
    g_log_file << "foo" << " " << "x=" << x << " " << "y=" << y << std::endl;
    g_param_freq["foo::x"]++;
    g_param_freq["foo::y"]++;
    return x + 10 + y;
}


int bar(int z, int i) {
    g_log_file << "bar" << " " << "z=" << z << " " << "i=" << i << std::endl;
    g_param_freq["bar::z"]++;
    g_param_freq["bar::i"]++;
    return 7;
}


int main() {
    // for (int i = 0; i < 5; ++i) {
    //     int (*f)(int, int);
    //     if (i % 5 == 0) {
    //         f = foo;
    //     }
    //     else {
    //         f = bar;
    //     }
    //
    //     int res = f(0, 0);
    //
    //     printf("%d", res);
    // }

    foo(1, 2);
    foo(2, 2);
    foo(3, 3);
    foo(4, 2);

    return 0;
}
