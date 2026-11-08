#include <stdio.h>

int add(int a, int b) {
    return a + b;
}

int main(void) {
    int x = 40;
    int y = 2;
    printf("%d %d %s %c\n", add(x, y), x * y, "ok", 'A');

    if (x < y)
        puts("bad");
    else
        puts("good");

    int i;
    int sum = 0;
    for (i = 0; i < 5; i++) {
        sum += i;
    }
    printf("sum=%d\n", sum);

    while (sum > 10) sum--;
    printf("sum=%d\n", sum);

    int a[3];
    a[0] = 7;
    a[1] = 8;
    a[2] = 9;
    printf("a=%d\n", a[0] + a[2]);

    printf("hex=%x ptr=%p\n", 0xBEEF, a);
    return 0;
}
