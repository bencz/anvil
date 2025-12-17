/* Test: Bitwise operations */

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

/* Print binary representation (8 bits) */
void print_binary(int n) {
    int i;
    for (i = 7; i >= 0; i--) {
        if ((n >> i) & 1) {
            putchar('1');
        } else {
            putchar('0');
        }
    }
}

int main(void) {
    int a = 0x0F;  /* 00001111 */
    int b = 0x55;  /* 01010101 */
    
    /* AND: 0x0F & 0x55 = 0x05 (00000101) */
    print_num(a & b);
    putchar('\n');
    
    /* OR: 0x0F | 0x55 = 0x5F (01011111) */
    print_num(a | b);
    putchar('\n');
    
    /* XOR: 0x0F ^ 0x55 = 0x5A (01011010) */
    print_num(a ^ b);
    putchar('\n');
    
    /* NOT: ~0x0F (lower 8 bits) = 0xF0 = 240 */
    print_num((~a) & 0xFF);
    putchar('\n');
    
    /* Left shift: 1 << 4 = 16 */
    print_num(1 << 4);
    putchar('\n');
    
    /* Right shift: 128 >> 3 = 16 */
    print_num(128 >> 3);
    putchar('\n');
    
    /* Combined: (a << 4) | (b >> 4) = 0xF5 = 245 */
    print_num((a << 4) | (b >> 4));
    putchar('\n');
    
    return 0;
}
