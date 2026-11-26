#include <cstdio>
#include <memory>
int main() { std::allocator<int> a; int* p = a.allocate(100); a.deallocate(p, 100); std::puts("t11 ok"); return 0; }
