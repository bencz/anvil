/* Test: basic arithmetic */
int putchar(int c);

void print_digit(int n) {
    putchar('0' + n);
}

int main(void) {
    int a = 5;
    int b = 3;
    
    /* Addition: 5 + 3 = 8 */
    print_digit(a + b);
    putchar('\n');
    
    /* Subtraction: 5 - 3 = 2 */
    print_digit(a - b);
    putchar('\n');
    
    /* Multiplication: 5 * 3 = 15, print 1 and 5 */
    int mul = a * b;
    print_digit(mul / 10);
    print_digit(mul % 10);
    putchar('\n');
    
    return 0;
}
