#include <cstdio>
struct S { int a[10]; };
int main() { S* s = new S; delete s; S* t = new S[4]; delete[] t; std::puts("t4 ok"); return 0; }
