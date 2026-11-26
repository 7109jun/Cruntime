#include <cstdio>
int main() { int* a = new int[32]; delete[] a; int* b = new int(5); delete b; std::puts("t3 ok"); return 0; }
