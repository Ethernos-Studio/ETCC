#include <stdio.h>

int pick(int x) {
    if (x)
        return 1;
    goto finish;
    return 99;
finish:
    return 2;
}

int main(void) {
    int i = 0;
    goto inside;
    while (i < 3) {
        i++;
    inside:
        printf("%d ", i);
    }
    printf("\n");

    goto out;
    printf("unreachable\n");
out:
    printf("done %d\n", i);

    int x = 1;
    switch (x) {
    case 1:
        goto after;
    default:
        puts("bad");
    }
after:
    printf("pick=%d\n", pick(1) + pick(0));

    int j = 0;
again:
    j++;
    if (j < 2) goto again;
    printf("j=%d\n", j);
    return 0;
}
