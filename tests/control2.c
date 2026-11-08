#include <stdio.h>

enum Mode {
    MODE_A = 1,
    MODE_B = 2,
    MODE_C = MODE_A + MODE_B
};

int f(int x) {
    switch (x) {
    case 1:
        return 10;
    default:
        return 20;
    }
}

int main(void) {
    printf("mode=%d\n", MODE_C);
    printf("f=%d %d\n", f(1), f(9));

    int total = 0;
    for (int i = 0; i < 6; i++) {
        switch (i) {
        case 0:
            total += 1;
            break;
        case 1:
        case 2:
            total += 10;
            continue;
        default:
            total += 100;
            break;
        }
        total += 1000;
    }
    printf("total=%d\n", total);

    int x = 2;
    int y = 1;
    switch (x) {
    case 2:
        switch (y) {
        case 1:
            puts("nested");
            break;
        }
        puts("after nested");
        break;
    default:
        puts("no");
    }

    switch (x) {
    case -1:
        puts("minus");
        break;
    case 2:
        puts("two");
        break;
    }
    return 0;
}
