#include <cstdio>
int main() { char* p = (char*)__builtin_operator_new(400); __builtin_operator_delete(p, (unsigned long long)400); std::puts("t12 ok"); return 0; }
