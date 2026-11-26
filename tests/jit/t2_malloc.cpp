#include <cstdio>
#include <cstdlib>
int main() { void* p = std::malloc(100); std::free(p); std::puts("t2 ok"); return 0; }
