#include <stdio.h>

enum {
    A = 0 && (1 / 0),
    B = 1 ? 42 : (1 / 0),
    C = 1 || (1 / 0),
    D = -2 >> 1,
    E = (_Bool)2
};

int main(void) {
    int x = 2;

    switch (x) {
    case 0 && (1 / 0):
        puts("zero");
        break;
    case 1 ? 2 : (1 / 0):
        puts("two");
        break;
    default:
        puts("other");
    }

    printf("%d %d %d %d %d\n", A, B, C, D, E);
    return 0;
}
