#include <stdio.h>

extern int sum(int a, int b);

int main(void) {
    int result = sum(10, 32);
    printf("sum(10, 32) = %d\n", result);
    return 0;
}
