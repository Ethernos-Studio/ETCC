#include <stdio.h>

struct P {
    int x;
    int y;
};

struct Q {
    struct P *p;
};

struct P ga[2] = {{7, 8}, {9, 10}};
struct Q gq = {0};

int main(void) {
    struct P a[2] = {{1, 2}, {3, 4}};
    struct P *p = a;
    printf("%d %d %d %d\n", p->x, p->y, (p + 1)->x, (p + 1)->y);
    p++;
    printf("%d %d\n", p->x, (int)sizeof(*p));
    struct Q q = {&ga[1]};
    printf("%d %d %d\n", q.p->x, ga[0].y, gq.p == 0);

    struct P copy = ga[1];
    printf("%d %d\n", copy.x, copy.y);

    ga[0] = copy;
    printf("%d %d\n", ga[0].x, ga[0].y);

    printf("%d\n", (&a[0])->y);
    return 0;
}

