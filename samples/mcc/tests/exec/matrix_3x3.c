/* Test: 3x3 matrix operations */

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

/* Sum all elements of a 3x3 matrix */
int mat3x3_sum(int m[3][3]) {
    int sum = 0;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            sum = sum + m[i][j];
        }
    }
    return sum;
}

/* Transpose a 3x3 matrix in place */
void mat3x3_transpose(int m[3][3]) {
    int i, j, tmp;
    for (i = 0; i < 3; i++) {
        for (j = i + 1; j < 3; j++) {
            tmp = m[i][j];
            m[i][j] = m[j][i];
            m[j][i] = tmp;
        }
    }
}

int main(void) {
    int m[3][3];
    int i, j;
    
    /* Initialize matrix with values 1-9 */
    /* [[1,2,3],[4,5,6],[7,8,9]] */
    m[0][0] = 1; m[0][1] = 2; m[0][2] = 3;
    m[1][0] = 4; m[1][1] = 5; m[1][2] = 6;
    m[2][0] = 7; m[2][1] = 8; m[2][2] = 9;
    
    /* Print sum (1+2+...+9 = 45) */
    print_num(mat3x3_sum(m));
    putchar('\n');
    
    /* Transpose and print first row (should be 1,4,7) */
    mat3x3_transpose(m);
    print_num(m[0][0]); putchar(' ');
    print_num(m[0][1]); putchar(' ');
    print_num(m[0][2]); putchar('\n');
    
    /* Print diagonal (1,5,9) */
    print_num(m[0][0]); putchar(' ');
    print_num(m[1][1]); putchar(' ');
    print_num(m[2][2]); putchar('\n');
    
    return 0;
}
