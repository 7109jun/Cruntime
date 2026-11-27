#include <stdio.h>
#include "config.h"
int main(void) {
    printf("version %s\n", APP_VERSION);
#ifdef HAVE_FOO
    puts("foo on");
#endif
#ifdef HAVE_BAR
    puts("bar on");
#else
    puts("bar off");
#endif
    return 0;
}
