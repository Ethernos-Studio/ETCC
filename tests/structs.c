#include <stdio.h>

struct Point {
    int x;
    int y;
};

struct Rect {
    struct Point a;
    struct Point b;
};

struct Node {
    int value;
    struct Node *next;
};

int main(void) {
    struct Point p;
    p.x = 3;
    p.y = 4;
    printf("%d %d %d\n", p.x, p.y, (int)sizeof(struct Point));

    struct Point q = {10, 20};
    printf("%d %d\n", q.x, q.y);

    struct Point r = q;
    r.x = 99;
    printf("%d %d %d %d\n", q.x, q.y, r.x, r.y);

    struct Rect rect = {{1, 2}, {3, 4}};
    printf("%d %d %d %d %d\n", rect.a.x, rect.a.y, rect.b.x, rect.b.y,
           (int)sizeof(struct Rect));

    struct Rect *pr = &rect;
    printf("%d %d\n", pr->b.y, (*pr).a.x);

    struct Node n2;
    n2.value = 2;
    n2.next = 0;
    struct Node n1;
    n1.value = 1;
    n1.next = &n2;
    printf("nodes=%d %d\n", n1.value, n1.next->value);
    return 0;
}
