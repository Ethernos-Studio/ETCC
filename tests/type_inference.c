#include <stdio.h>

int main(void) {
    long long x = 1 ? 1 + 4294967296 : 0;
    long long y = 0 ? 1 : 4294967296;

    printf("%lld %lld\n", x, y);
    printf("%d %d %d %d\n",
           (int)sizeof(1 + 4294967296),
           (int)sizeof(1 ? 1 : 0),
           (int)sizeof(1 ? 1 : 4294967296),
           (int)sizeof(1 ? "a" : "b"));
    printf("%d %d\n", 1 ? 1 : 0, 0 ? 1 : 2);

    int i = 0;
    int *p = &i;
    printf("%d %d\n", (int)sizeof(p + 1), (int)sizeof(p - p));
    printf("%d %d %d %d %d %d %d\n",
           (int)sizeof(2147483648),
           (int)sizeof(4294967295),
           (int)sizeof(0x80000000),
           (int)sizeof(0xFFFFFFFF),
           (int)sizeof(0x100000000),
           (int)sizeof(1LL),
           (int)sizeof(1L));
    return 0;
}
