/* Test: Recursive functions */

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

/* Factorial: n! */
int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

/* Fibonacci: fib(n) */
int fibonacci(int n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}

/* GCD using Euclidean algorithm */
int gcd(int a, int b) {
    if (b == 0) {
        return a;
    }
    return gcd(b, a % b);
}

int main(void) {
    /* Factorial: 5! = 120 */
    print_num(factorial(5));
    putchar('\n');
    
    /* Fibonacci: fib(10) = 55 */
    print_num(fibonacci(10));
    putchar('\n');
    
    /* GCD: gcd(48, 18) = 6 */
    print_num(gcd(48, 18));
    putchar('\n');
    
    /* GCD: gcd(100, 35) = 5 */
    print_num(gcd(100, 35));
    putchar('\n');
    
    return 0;
}
