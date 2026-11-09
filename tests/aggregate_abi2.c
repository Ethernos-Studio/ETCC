#include <stdio.h>

struct TwoBytes { short a; };
struct FourBytes { char a; char b; char c; char d; };
struct EightBytes { int a; int b; };
struct Mixed { char c; int i; };
struct Long { long long x; };
struct Three { char a[3]; };
struct Twelve { int a; int b; int c; };
struct Sixteen { long long a; long long b; };
union SmallUnion { int i; char c[4]; };

struct EightBytes make_eight(int a, int b) {
    struct EightBytes s;
    s.a = a;
    s.b = b;
    return s;
}

int modify_eight(struct EightBytes s) {
    int old = s.a;
    s.a = 999;
    return old + s.b;
}

struct Twelve make_twelve(int x) {
    struct Twelve s;
    s.a = x;
    s.b = x + 1;
    s.c = x + 2;
    return s;
}

int modify_twelve(struct Twelve s) {
    int old = s.a;
    s.a = 777;
    return old + s.b + s.c;
}

struct Three make_three(char a, char b, char c) {
    struct Three s;
    s.a[0] = a;
    s.a[1] = b;
    s.a[2] = c;
    return s;
}

int sum_three(struct Three s) { return s.a[0] + s.a[1] + s.a[2]; }

struct Sixteen make_sixteen(long long a, long long b) {
    struct Sixteen s;
    s.a = a;
    s.b = b;
    return s;
}

long long sum_sixteen(struct Sixteen s) { return s.a + s.b; }

struct TwoBytes make_two(short a) {
    struct TwoBytes s;
    s.a = a;
    return s;
}

short get_two(struct TwoBytes s) { return s.a; }

struct FourBytes make_four(char a, char b, char c, char d) {
    struct FourBytes s;
    s.a = a;
    s.b = b;
    s.c = c;
    s.d = d;
    return s;
}

int sum_four(struct FourBytes s) {
    return s.a + s.b + s.c + s.d;
}

union SmallUnion make_union(int value) {
    union SmallUnion u;
    u.i = value;
    return u;
}

int get_union(union SmallUnion u) { return u.i; }

int main(void) {
    struct EightBytes e = make_eight(3, 4);
    printf("eight=%d %d %d\n", e.a, e.b, modify_eight(e));
    printf("eight_after=%d %d\n", e.a, e.b);

    struct Twelve t = make_twelve(10);
    printf("twelve=%d %d %d %d\n", t.a, t.b, t.c, modify_twelve(t));
    printf("twelve_after=%d\n", t.a);

    struct Three th = make_three('A', 'B', 'C');
    printf("three=%d %d\n", sum_three(th), sum_three(make_three(1, 2, 3)));

    struct Sixteen s = make_sixteen(10000000000LL, 20000000000LL);
    printf("sixteen=%lld %lld\n", s.a, s.b);
    printf("sixteen_sum=%lld\n", sum_sixteen(s));

    struct Mixed m;
    m.c = 'x';
    m.i = 42;
    printf("mixed=%c %d %d\n", m.c, m.i, (int)sizeof(m));

    union SmallUnion u;
    u.i = 0x41424344;
    printf("union=%c %c %c %c\n", u.c[0], u.c[1], u.c[2], u.c[3]);

    struct Long l;
    l.x = 9000000000LL;
    printf("long=%lld\n", l.x);

    struct TwoBytes two = make_two(1234);
    printf("two=%d\n", get_two(two));

    struct FourBytes four = make_four(1, 2, 3, 4);
    printf("four=%d\n", sum_four(four));

    union SmallUnion un = make_union(0x11223344);
    printf("union_int=%x\n", get_union(un));
    return 0;
}
