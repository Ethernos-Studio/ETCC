#include <stdio.h>
#include "mathx.h"
#include "mathx.h"

int mathx_add(int a, int b) {
    return a + b;
}

int main(void) {
    printf("%d %d\n", MATHX_ANSWER, mathx_add(20, 22));
    return 0;
}
