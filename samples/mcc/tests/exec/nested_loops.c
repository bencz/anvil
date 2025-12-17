/* Test: Nested loops and complex control flow */

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
    int i, j, k;
    int sum;
    
    /* Triple nested loop: sum of i*j*k for i,j,k in [1,3] */
    sum = 0;
    for (i = 1; i <= 3; i++) {
        for (j = 1; j <= 3; j++) {
            for (k = 1; k <= 3; k++) {
                sum = sum + i * j * k;
            }
        }
    }
    print_num(sum);  /* (1+2+3)^3 = 216 */
    putchar('\n');
    
    /* Print multiplication table 3x3 */
    for (i = 1; i <= 3; i++) {
        for (j = 1; j <= 3; j++) {
            print_num(i * j);
            putchar(' ');
        }
        putchar('\n');
    }
    
    /* While inside for */
    sum = 0;
    for (i = 0; i < 5; i++) {
        j = i;
        while (j > 0) {
            sum = sum + j;
            j = j - 1;
        }
    }
    print_num(sum);  /* 0 + 1 + (2+1) + (3+2+1) + (4+3+2+1) = 20 */
    putchar('\n');
    
    return 0;
}
