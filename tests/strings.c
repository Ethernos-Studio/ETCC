#include <stdio.h>

int main(void) {
    puts("tab\there");
    puts("quote\"slash\\");
    putchar('X');
    putchar('\n');
    printf("%s %c\n", "concatenated" " strings", '\x41');
    printf("octal=%d\n", '\101');
    return 0;
}
