#include <stdio.h>

typedef struct {
    int a;
    int b;
} Pair;

typedef struct {
    int a;
    int b;
    int c;
} Big;

typedef struct {
    char x;
} One;

Pair make_pair(int a, int b) {
    Pair p;
    p.a = a;
    p.b = b;
    return p;
}

int sum_pair(Pair p) { return p.a + p.b; }

Big make_big(int x) {
    Big b;
    b.a = x;
    b.b = x + 1;
    b.c = x + 2;
    return b;
}

int sum_big(Big b) { return b.a + b.b + b.c; }

One make_one(char x) {
    One o;
    o.x = x;
    return o;
}

char get_one(One o) { return o.x; }

int main(void) {
    Pair p = make_pair(2, 3);
    printf("pair=%d %d %d\n", p.a, p.b, sum_pair(p));
    printf("pair_call=%d\n", sum_pair(make_pair(7, 8)));

    Big b = make_big(10);
    printf("big=%d %d %d %d\n", b.a, b.b, b.c, sum_big(b));
    printf("big_call=%d\n", sum_big(make_big(20)));

    One o = make_one('Z');
    printf("one=%c %c\n", o.x, get_one(make_one('Q')));
    return 0;
}
