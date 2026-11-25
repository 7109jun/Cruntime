#include <stdio.h>
#include "mathlib.h"

int main(void) {
    printf("add(2,3)=%d  mul(4,5)=%d\n", ml_add(2, 3), ml_mul(4, 5));
#ifdef ON_WINDOWS
    puts("built for Windows");
#endif
    return 0;
}
