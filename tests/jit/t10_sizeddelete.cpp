#include <cstdio>
#include <new>
int main() { void* p = ::operator new(400); ::operator delete(p, (size_t)400); std::puts("t10 ok"); return 0; }
