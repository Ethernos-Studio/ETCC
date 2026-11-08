#include <stdio.h>

int main(void) {
    int a, b, c;
    a = b = c = 5;
    printf("%d %d %d\n", a, b, c);

    int x = (1, 2, 3);
    printf("comma=%d\n", x);

    int side = 0;
    if (0 && (side = 1)) puts("bad");
    if (1 || (side = 2)) puts("short");
    printf("side=%d\n", side);

    int y = 10;
    int r = (y > 5) ? (y = 20) : (y = 30);
    printf("r=%d y=%d\n", r, y);

    int n = 5;
    printf("post=%d ", n++);
    printf("pre=%d ", ++n);
    printf("dec=%d\n", n--);

    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    int *p = arr;
    printf("%d ", *p++);
    printf("%d ", *p);
    p += 1;
    printf("%d\n", *p);

    printf("ptr=%d\n", p == &arr[2]);
    return 0;
}

