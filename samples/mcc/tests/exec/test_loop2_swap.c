int putchar(int c);

void print_num(int n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void loop2_swap(int m[3][3]) {
    int i, j, tmp;
    for (i = 0; i < 2; i++) {
        for (j = i + 1; j < 3; j++) {
            tmp = m[i][j];
            m[i][j] = m[j][i];
            m[j][i] = tmp;
        }
    }
}

int main(void) {
    int m[3][3];
    
    m[0][0] = 1; m[0][1] = 2; m[0][2] = 3;
    m[1][0] = 4; m[1][1] = 5; m[1][2] = 6;
    m[2][0] = 7; m[2][1] = 8; m[2][2] = 9;
    
    /* Before: diagonal 1 5 9 */
    print_num(m[0][0]); putchar(' ');
    print_num(m[1][1]); putchar(' ');
    print_num(m[2][2]); putchar('\n');
    
    loop2_swap(m);
    
    /* After transpose: diagonal should still be 1 5 9 */
    print_num(m[0][0]); putchar(' ');
    print_num(m[1][1]); putchar(' ');
    print_num(m[2][2]); putchar('\n');
    
    return 0;
}
