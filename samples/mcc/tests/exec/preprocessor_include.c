/* Test: #include directive with standard headers */

#include <limits.h>
#include <stddef.h>

int putchar(int c);

void print_digit(int n) {
    putchar('0' + n);
}

void print_num(int n) {
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    if (n >= 10) {
        print_num(n / 10);
    }
    print_digit(n % 10);
}

int main(void) {
    /* Test CHAR_BIT from limits.h */
    print_num(CHAR_BIT);
    putchar('\n');
    
    /* Test INT_MAX sign (should be positive) */
    if (INT_MAX > 0) {
        print_num(1);
    } else {
        print_num(0);
    }
    putchar('\n');
    
    /* Test NULL from stddef.h */
    void *p = NULL;
    if (p == 0) {
        print_num(1);
    } else {
        print_num(0);
    }
    putchar('\n');
    
    return 0;
}
