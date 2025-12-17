/* Test: Do-while loops */

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

/* Count digits in a number */
int count_digits(int n) {
    int count = 0;
    if (n < 0) n = -n;
    do {
        count = count + 1;
        n = n / 10;
    } while (n > 0);
    return count;
}

int main(void) {
    int i;
    
    /* Basic do-while */
    i = 0;
    do {
        print_num(i);
        i = i + 1;
    } while (i < 5);
    putchar('\n');  /* 01234 */
    
    /* Do-while always executes at least once */
    i = 10;
    do {
        print_num(i);
    } while (i < 5);
    putchar('\n');  /* 10 */
    
    /* Count digits */
    print_num(count_digits(12345));  /* 5 */
    putchar('\n');
    
    print_num(count_digits(7));  /* 1 */
    putchar('\n');
    
    print_num(count_digits(0));  /* 1 */
    putchar('\n');
    
    return 0;
}
