#include <stdio.h>

struct Point {
    int x;
    int y;
};

struct Pair {
    char a;
    int b;
};

union U {
    int i;
    char c[4];
};

struct Mixed {
    char name[8];
    int value;
};

struct Point gp = {7, 8};
struct Point garr[3] = {{1, 2}, {3, 4}, {5, 6}};
char gs[] = "hello";
char gs2[8] = "hi";
union U gu = {0x11223344};
struct Mixed gm = {"abc", 42};

int main(void) {
    printf("%d %d\n", gp.x, gp.y);
    printf("%d %d %d\n", garr[0].x, garr[1].y, garr[2].x);
    printf("%s %s %d\n", gs, gs2, (int)sizeof(gs));

    struct Pair p = {1, 2};
    printf("%d %d %d\n", p.a, p.b, (int)sizeof(struct Pair));

    struct Point local[2] = {{9, 10}, {11, 12}};
    printf("%d %d %d\n", local[0].x, local[0].y, local[1].y);

    char s[] = "world";
    char buf[10] = "abc";
    printf("%s %d %s\n", s, (int)sizeof(s), buf);

    union U u;
    u.i = 0x01020304;
    printf("%d %d\n", u.i, (int)(unsigned char)u.c[0]);
    printf("%x\n", gu.i);

    int a[] = {1, 2, 3, 4};
    printf("%d %d\n", (int)(sizeof(a) / sizeof(a[0])), a[3]);

    printf("%s %d\n", gm.name, gm.value);
    return 0;
}
