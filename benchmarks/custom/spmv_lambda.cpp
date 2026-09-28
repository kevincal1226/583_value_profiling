#include <functional>
#include <iostream>

extern "C" std::function<int(int)> *lambdaopt_init_function_from_fp(std::function<int(int)> *out, int (*fp)(int)) {
    new (out) std::function<int(int)>(fp);
    return out;
}

std::function<int(int)> multiply(int x) {
    return [x](int y) {
        int acc = 0;
        for (int i = 0; i < 35; ++i) {
            acc += (x * x * (i)) / (i % 3 + 1);
        }
        return acc;
    };
}

#define SIZE 1000

auto spmv() {
    int mtx[SIZE][SIZE];
    int vec[SIZE];
    for (int i = 0; i < SIZE; ++i) {
        vec[i] = i % 3 == 0 ? 0 : i;
        for (int j = 0; j < SIZE; ++j) {
            if (i % 2 == 0 || j % 2 == 0 || i > 10) {
                mtx[i][j] = 0;
            }
            else {
                mtx[i][j] = rand() % 200;
            }
        }
    }

    int res[SIZE];
    for (int row = 0; row < SIZE; ++row) {
        res[row] = 0;
        for (int col = 0; col < SIZE; ++col) {
            int x = mtx[row][col];
            int y = vec[row];
            int r = multiply(x)(y);
            res[row] += r;
        }
    }

    for (int i = 0; i < SIZE; ++i) {
        std::cout << res[i] << " ";
    }
    std::cout << "\n";
}

int main() {
    for (int i = 0; i < 100; ++i) {
        spmv();
    }
    return 0;
}
