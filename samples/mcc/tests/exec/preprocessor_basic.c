/* Test: Basic preprocessor functionality */

#define VALUE 42
#define DOUBLE(x) ((x) * 2)
#define ADD(a, b) ((a) + (b))

int putchar(int c);

void print_digit(int n) {
    putchar('0' + n);
}

void print_num(int n) {
    if (n >= 10) {
        print_num(n / 10);
    }
    print_digit(n % 10);
}

int main(void) {
    /* Test simple macro */
    print_num(VALUE);
    putchar('\n');
    
    /* Test function-like macro */
    print_num(DOUBLE(21));
    putchar('\n');
    
    /* Test macro with multiple args */
    print_num(ADD(20, 22));
    putchar('\n');
    
    return 0;
}
