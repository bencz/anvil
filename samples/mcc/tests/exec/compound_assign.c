/* Test: Compound assignment operators */

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
    int a = 10;
    
    /* += */
    a += 5;
    print_num(a);  /* 15 */
    putchar('\n');
    
    /* -= */
    a -= 3;
    print_num(a);  /* 12 */
    putchar('\n');
    
    /* *= */
    a *= 2;
    print_num(a);  /* 24 */
    putchar('\n');
    
    /* /= */
    a /= 4;
    print_num(a);  /* 6 */
    putchar('\n');
    
    /* %= */
    a %= 4;
    print_num(a);  /* 2 */
    putchar('\n');
    
    /* Bitwise compound */
    a = 0xFF;
    a &= 0x0F;
    print_num(a);  /* 15 */
    putchar('\n');
    
    a |= 0x30;
    print_num(a);  /* 63 */
    putchar('\n');
    
    a ^= 0x0F;
    print_num(a);  /* 48 */
    putchar('\n');
    
    /* Shift compound */
    a = 4;
    a <<= 2;
    print_num(a);  /* 16 */
    putchar('\n');
    
    a >>= 1;
    print_num(a);  /* 8 */
    putchar('\n');
    
    return 0;
}
