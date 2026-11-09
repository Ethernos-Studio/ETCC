#include <stdio.h>

struct Pair { int a; int b; };
struct Big { int a; int b; int c; };
struct One { char x; };

struct Pair make_pair(int a, int b);
struct Big make_big(int x);

struct Pair identity_pair(struct Pair p) {
    p.a = p.a + 1;
    return p;
}

struct Big identity_big(struct Big b) {
    b.a = b.a + 1;
    return b;
}

struct One identity_one(struct One o) {
    o.x = (char)(o.x + 1);
    return o;
}

int mix(struct Pair p, int x, struct Big b, struct One o) {
    return p.a + p.b + x + b.a + b.b + b.c + o.x;
}

struct Pair make_pair(int a, int b) {
    struct Pair p;
    p.a = a;
    p.b = b;
    return p;
}

struct Big make_big(int x) {
    struct Big b;
    b.a = x;
    b.b = x + 1;
    b.c = x + 2;
    return b;
}

int main(void) {
    struct Pair p = make_pair(1, 2);
    struct Pair q = identity_pair(p);
    printf("pair=%d %d -> %d %d\n", p.a, p.b, q.a, q.b);

    struct Big b = make_big(5);
    struct Big c = identity_big(b);
    printf("big=%d %d %d -> %d %d %d\n", b.a, b.b, b.c, c.a, c.b, c.c);

    struct One o;
    o.x = 'A';
    struct One o2 = identity_one(o);
    printf("one=%c -> %c\n", o.x, o2.x);

    printf("mix=%d\n", mix(p, 10, b, o));
    printf("rvalue=%d %d\n", make_pair(30, 40).a, make_big(50).c);
    return 0;
}
