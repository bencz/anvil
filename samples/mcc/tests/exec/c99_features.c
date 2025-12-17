/* Test: C99 features */

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
    /* C99: Mixed declarations and code */
    int sum = 0;
    
    print_num(sum);
    putchar('\n');
    
    /* C99: for loop with declaration */
    for (int i = 0; i < 5; i++) {
        sum = sum + i;
    }
    print_num(sum);  /* 0+1+2+3+4 = 10 */
    putchar('\n');
    
    /* C99: Variable declaration in middle of block */
    int product = 1;
    for (int j = 1; j <= 4; j++) {
        product = product * j;
    }
    print_num(product);  /* 1*2*3*4 = 24 */
    putchar('\n');
    
    /* C99: Single-line comments work */
    // This is a C99 comment
    int x = 42;
    print_num(x);
    putchar('\n');
    
    return 0;
}
