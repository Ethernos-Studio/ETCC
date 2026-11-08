#include <stdio.h>
#include <stdbool.h>

#define N 5
#define GREETING "hi"
#define ZERO 0

int g = 3;
static int hidden = 7;
char *msg = "global";

int is_even(int x) {
    return x % 2 == ZERO;
}

int main(void) {
    printf("%d %d %s %s\n", g, hidden, msg, GREETING);

    int i = 0;
    while (i < N) {
        if (is_even(i)) printf("%d ", i);
        i++;
    }
    printf("\n");

    printf("%d %d %d %d\n", 1 && 0, 1 || 0, !0, (3 > 2) ? 10 : 20);
    printf("size=%d %d %d\n", (int)sizeof(int), (int)sizeof(char),
           (int)sizeof(long));

    bool b = true;
    printf("bool=%d\n", b);

    char *p = "abc";
    printf("%c %c %c\n", *p, *(p + 1), p[2]);
    p++;
    printf("%c\n", *p);

    int value = 0;
    value = (g > hidden) ? 100 : 200;
    printf("value=%d\n", value);
    return 0;
}
