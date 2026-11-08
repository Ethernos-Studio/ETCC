#include <stdio.h>

int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

int main(void) {
    int s = 0;
    for (int i = 0; i < 10; i++) {
        if (i % 2) continue;
        if (i > 6) break;
        s += i;
    }
    printf("s=%d fact=%d\n", s, fact(5));

    int j = 0;
    do {
        j++;
    } while (j < 3);
    printf("j=%d\n", j);

    int k = 0;
    while (k < 5) {
        k++;
        if (k == 2) continue;
        if (k == 4) break;
        printf("k=%d ", k);
    }
    printf("\n");

    int a = 10;
    {
        int a = 20;
        printf("inner=%d\n", a);
    }
    printf("outer=%d\n", a);

    printf("cmp=%d %d %d\n", a == 10, a != 10, a >= 10);
    printf("bits=%d %d %d\n", (6 & 3), (6 | 3), (6 ^ 3));
    printf("shift=%d %d\n", 1 << 4, 32 >> 2);
    printf("unary=%d %d %d\n", -5, ~0, !7);
    printf("sizeof=%d\n", (int)sizeof(long long));

    unsigned int u = 4000000000U;
    printf("u=%u\n", u);
    long long big = 1234567890123LL;
    printf("big=%lld\n", big);
    return 0;
}
