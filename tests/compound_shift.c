#include <stdio.h>

int main(void) {
    long long x = -2;
    unsigned long long n = 1;
    x >>= n;

    unsigned int u = 0x80000000u;
    u >>= 1;

    int s = -8;
    unsigned int k = 2;
    s >>= k;

    short h = -4;
    int sh = 1;
    h >>= sh;

    printf("%lld %u %d %d\n", x, u, s, (int)h);
    return 0;
}
