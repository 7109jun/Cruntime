#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> v = {"CRuntime", "LLVM", "ORC", "JIT"};
    for (auto& s : v) std::cout << "hello, " << s << "!\n";
    std::printf("argc=%d\n", argc);
    return 0;
}
