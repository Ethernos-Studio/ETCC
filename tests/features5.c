#include <stdio.h>

void greet(const char *name) {
    printf("hello %s\n", name);
}

static int twice(int x) {
    return x * 2;
}

int *pick(int *p) {
    return p;
}

int main(void) {
    greet("etcc");
    printf("%d\n", twice(21));

    int x = 33;
    int *p = pick(&x);
    printf("%d\n", *p);

    for (int i = 0; i < 1; i++) {
        int inner = i;
        printf("inner=%d\n", inner);
    }
    int i = 99;
    printf("i=%d\n", i);
    return 0;
}
