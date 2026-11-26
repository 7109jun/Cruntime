#include <cstdio>
int thrower() { throw 42; }
int main() { try { thrower(); } catch (int v) { std::printf("t9 ok caught %d\n", v); } return 0; }
