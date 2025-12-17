/* Test: Conditional compilation (#ifdef, #ifndef, #else, #endif) */

#define FEATURE_A
#define VALUE_X 10

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
#ifdef FEATURE_A
    print_num(1);  /* Should print 1 */
#else
    print_num(0);
#endif
    putchar('\n');

#ifndef FEATURE_B
    print_num(2);  /* Should print 2 */
#else
    print_num(0);
#endif
    putchar('\n');

#ifdef FEATURE_B
    print_num(0);
#else
    print_num(3);  /* Should print 3 */
#endif
    putchar('\n');

#if defined(FEATURE_A) && defined(VALUE_X)
    print_num(4);  /* Should print 4 */
#else
    print_num(0);
#endif
    putchar('\n');

    return 0;
}
