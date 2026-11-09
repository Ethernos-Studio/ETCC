#include <stdio.h>

int main(void) {
    int x = 7;
    int *p = &x;
    void *v = p;
    int *q = v;
    char *c = (char *)p;
    int *nullp = 0;

    _Bool b0 = (_Bool)0;
    _Bool b1 = (_Bool)p;
    _Bool b2 = (_Bool)q;
    _Bool b3 = 5;
    _Bool b4 = 0;
    _Bool b5 = nullp;

    printf("%d %d %d %d %d %d %d %d\n",
           v == p, q == p, c != 0, b0, b1, b2, b3, b5);
    printf("%d\n", b4);
    return 0;
}
