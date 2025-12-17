/* Test: C89/ANSI C features */

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

/* C89: Function declarations with types */
int add(int a, int b);
int multiply(int a, int b);

int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

/* C89: Struct definition */
struct Point {
    int x;
    int y;
};

int main(void) {
    /* C89: Variables must be declared at start of block */
    int a;
    int b;
    int result;
    struct Point p;
    
    a = 10;
    b = 5;
    
    /* Test function calls */
    result = add(a, b);
    print_num(result);  /* 15 */
    putchar('\n');
    
    result = multiply(a, b);
    print_num(result);  /* 50 */
    putchar('\n');
    
    /* Test struct */
    p.x = 3;
    p.y = 4;
    print_num(p.x + p.y);  /* 7 */
    putchar('\n');
    
    return 0;
}
