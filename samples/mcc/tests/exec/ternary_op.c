/* Test: Ternary operator */

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

int max(int a, int b) {
    return a > b ? a : b;
}

int min(int a, int b) {
    return a < b ? a : b;
}

int abs_val(int x) {
    return x >= 0 ? x : -x;
}

int main(void) {
    /* Basic ternary */
    print_num(1 > 0 ? 10 : 20);  /* 10 */
    putchar('\n');
    
    print_num(1 < 0 ? 10 : 20);  /* 20 */
    putchar('\n');
    
    /* Ternary in function */
    print_num(max(15, 8));  /* 15 */
    putchar('\n');
    
    print_num(min(15, 8));  /* 8 */
    putchar('\n');
    
    /* Nested ternary */
    int a = 5, b = 10, c = 3;
    int largest = a > b ? (a > c ? a : c) : (b > c ? b : c);
    print_num(largest);  /* 10 */
    putchar('\n');
    
    /* Absolute value */
    print_num(abs_val(-42));  /* 42 */
    putchar('\n');
    
    print_num(abs_val(17));  /* 17 */
    putchar('\n');
    
    return 0;
}
