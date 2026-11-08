#include <stdio.h>

union U {
    int i;
    char c[8];
};

struct S {
    char a;
    long long b;
};

int main(void) {
    union U a, b;
    a.i = 0x01020304;
    b = a;
    printf("%x %d %d\n", b.i, (int)sizeof(union U), (int)sizeof(struct S));
    a.c[0] = 9;
    printf("%d %d\n", b.c[0], a.c[0]);
    return 0;
}
