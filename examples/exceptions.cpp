#include <cstdio>
#include <stdexcept>

static int risky(int x) {
    if (x > 2) throw std::runtime_error("x too big");
    return x * 10;
}

int main() {
    for (int i = 1; i <= 4; ++i) {
        try {
            std::printf("risky(%d)=%d\n", i, risky(i));
        } catch (const std::exception& e) {
            std::printf("caught at %d: %s\n", i, e.what());
        }
    }
    return 0;
}
