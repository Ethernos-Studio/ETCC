#include <stdio.h>

int main(void) {
    int a[2][3];
    a[0][0] = 1;
    a[0][1] = 2;
    a[1][2] = 7;
    printf("%d %d %d\n", a[0][0], a[0][1], a[1][2]);

    int x = 5;
    int *p = &x;
    *p = 9;
    printf("%d %d\n", x, *p);

    int **pp;
    int *q = &x;
    pp = &q;
    **pp = 11;
    printf("%d\n", x);

    printf("%d %d %d\n", (int)sizeof(a), (int)sizeof(a[0]),
           (int)sizeof(a[0][0]));
    printf("%d\n", (int)(char)300);
    printf("%d\n", (int)(unsigned char)300);
    printf("%d\n", (int)(short)70000);
    printf("%d\n", (int)(unsigned short)70000);
    return 0;
}
