/* Test: Matrix operations */

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

/* 2x2 matrix multiplication */
void mat2x2_mul(int a[2][2], int b[2][2], int c[2][2]) {
    int i, j, k;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            c[i][j] = 0;
            for (k = 0; k < 2; k++) {
                c[i][j] = c[i][j] + a[i][k] * b[k][j];
            }
        }
    }
}

/* Matrix trace (sum of diagonal) */
int mat2x2_trace(int m[2][2]) {
    return m[0][0] + m[1][1];
}

/* Matrix determinant */
int mat2x2_det(int m[2][2]) {
    return m[0][0] * m[1][1] - m[0][1] * m[1][0];
}

int main(void) {
    int a[2][2];
    int b[2][2];
    int c[2][2];
    
    /* Initialize matrix A = [[1,2],[3,4]] */
    a[0][0] = 1; a[0][1] = 2;
    a[1][0] = 3; a[1][1] = 4;
    
    /* Initialize matrix B = [[5,6],[7,8]] */
    b[0][0] = 5; b[0][1] = 6;
    b[1][0] = 7; b[1][1] = 8;
    
    /* C = A * B = [[19,22],[43,50]] */
    mat2x2_mul(a, b, c);
    
    /* Print result matrix */
    print_num(c[0][0]); putchar(' ');
    print_num(c[0][1]); putchar('\n');
    print_num(c[1][0]); putchar(' ');
    print_num(c[1][1]); putchar('\n');
    
    /* Print trace of A (1+4=5) */
    print_num(mat2x2_trace(a));
    putchar('\n');
    
    /* Print determinant of A (1*4-2*3=-2) */
    print_num(mat2x2_det(a));
    putchar('\n');
    
    return 0;
}
