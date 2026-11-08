#include <stdio.h>

enum E {
    A,
    B = 5,
    C,
    D = A + B + C
};

enum E ge = B;
int arr[C];

int main(void) {
    printf("%d %d %d %d %d\n", A, B, C, D, (int)sizeof(enum E));
    printf("%d %d %d\n", ge, (int)(sizeof(arr) / sizeof(arr[0])), arr[0]);

    enum Local {
        X = 10,
        Y = X + 5
    };
    printf("%d %d\n", X, Y);

    int A = 99;
    printf("%d\n", A);
    {
        enum { A = 7 };
        printf("%d\n", A);
    }
    printf("%d\n", A);

    enum { Z = A + 1 };
    printf("%d\n", Z);
    return 0;
}
