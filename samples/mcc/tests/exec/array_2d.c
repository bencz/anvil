/* Test: 2D array operations */

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
    int arr[3][4];
    int i, j;
    int sum;
    
    /* Initialize 2D array with row*10 + col */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            arr[i][j] = i * 10 + j;
        }
    }
    
    /* Print array */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            print_num(arr[i][j]);
            putchar(' ');
        }
        putchar('\n');
    }
    
    /* Sum all elements */
    sum = 0;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            sum = sum + arr[i][j];
        }
    }
    print_num(sum);  /* (0+1+2+3) + (10+11+12+13) + (20+21+22+23) = 138 */
    putchar('\n');
    
    /* Sum diagonal (min of rows, cols) */
    sum = 0;
    for (i = 0; i < 3; i++) {
        sum = sum + arr[i][i];
    }
    print_num(sum);  /* 0 + 11 + 22 = 33 */
    putchar('\n');
    
    return 0;
}
