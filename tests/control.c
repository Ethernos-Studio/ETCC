#include <stdio.h>

enum Color {
    RED,
    GREEN = 5,
    BLUE
};

int classify(int x) {
    switch (x) {
    case 0:
        return 100;
    case 1:
        return 101;
    case GREEN:
        return 105;
    default:
        return 999;
    }
}

int main(void) {
    printf("%d %d %d\n", RED, GREEN, BLUE);
    printf("%d %d %d %d\n", classify(0), classify(1), classify(5), classify(2));

    int x = 2;
    switch (x) {
    case 1:
        puts("one");
        break;
    case 2:
        puts("two");
        /* fall through */
    case 3:
        puts("three");
        break;
    default:
        puts("other");
    }

    int i = 0;
    goto skip;
    i = 99;
skip:
    printf("i=%d\n", i);

    int n = 0;
loop:
    n++;
    if (n < 3) goto loop;
    printf("n=%d\n", n);

    return 0;
}
