/* Test: Logical operators */

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
    int a = 5;
    int b = 0;
    int c = 10;
    
    /* Logical AND */
    print_num(a && c);  /* 1 (both non-zero) */
    putchar('\n');
    
    print_num(a && b);  /* 0 (b is zero) */
    putchar('\n');
    
    /* Logical OR */
    print_num(a || b);  /* 1 (a is non-zero) */
    putchar('\n');
    
    print_num(b || b);  /* 0 (both zero) */
    putchar('\n');
    
    /* Logical NOT */
    print_num(!a);  /* 0 */
    putchar('\n');
    
    print_num(!b);  /* 1 */
    putchar('\n');
    
    /* Combined */
    print_num((a > 3) && (c < 20));  /* 1 */
    putchar('\n');
    
    print_num((a > 10) || (c > 5));  /* 1 */
    putchar('\n');
    
    print_num(!(a == b));  /* 1 */
    putchar('\n');
    
    /* Short-circuit evaluation test */
    int x = 0;
    /* b is 0, so second part should not execute */
    if (b && (x = 1)) {
        x = 2;
    }
    print_num(x);  /* 0 (x was never set) */
    putchar('\n');
    
    return 0;
}
