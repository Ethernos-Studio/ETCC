#include <stdio.h>

struct A {
    int x;
    int y;
};

struct B {
    struct A a;
    int z;
};

struct B gb = {{1}, 2};
struct A ga[2] = {{3}};

int main(void) {
    printf("%d %d %d\n", gb.a.x, gb.a.y, gb.z);
    printf("%d %d %d %d\n", ga[0].x, ga[0].y, ga[1].x, ga[1].y);

    struct B b = {{4}, 5};
    printf("%d %d %d\n", b.a.x, b.a.y, b.z);

    char s[5] = "ab";
    printf("%s %d %d\n", s, s[2], s[4]);
    return 0;
}
