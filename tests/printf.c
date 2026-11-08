#include <stdio.h>

int main(void) {
    printf("[%d][%+d][%5d][%-5d][%05d]\n", -12, 12, 42, 42, 42);
    printf("[%u][%o][%x][%X][%#x]\n", 42u, 42u, 0xBEEFu, 0xBEEFu, 0xBEEFu);
    printf("[%s][%5s][%-5s][%.3s]\n", "abc", "ab", "ab", "abcdef");
    printf("[%c][%%][%p]\n", 'Z', (void *)0);
    printf("[%ld][%lld][%zu]\n", 123456L, 1234567890123LL, (size_t)99);
    return 0;
}
