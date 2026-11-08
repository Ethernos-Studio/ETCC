#include <stdio.h>

typedef struct {
    int x;
    int y;
} Pair;

struct Container {
    Pair pts[2];
    int tag;
};

int main(void) {
    struct Local {
        int value;
    };
    struct Local l;
    l.value = 11;
    printf("%d\n", l.value);

    Pair p = {1, 2};
    printf("%d %d\n", p.x, p.y);

    struct Container c = {{{3, 4}, {5, 6}}, 7};
    printf("%d %d %d %d %d\n", c.pts[0].x, c.pts[0].y, c.pts[1].x,
           c.pts[1].y, c.tag);
    printf("%d %d\n", (int)sizeof(Pair), (int)sizeof(struct Container));

    struct Container d = c;
    d.pts[0].x = 99;
    printf("%d %d\n", c.pts[0].x, d.pts[0].x);
    return 0;
}
