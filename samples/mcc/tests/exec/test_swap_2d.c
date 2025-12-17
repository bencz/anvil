int putchar(int c);

void print_num(int n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void swap_elements(int m[3][3], int i1, int j1, int i2, int j2) {
    int tmp;
    tmp = m[i1][j1];
    m[i1][j1] = m[i2][j2];
    m[i2][j2] = tmp;
}

int main(void) {
    int m[3][3];
    
    m[0][0] = 1; m[0][1] = 2; m[0][2] = 3;
    m[1][0] = 4; m[1][1] = 5; m[1][2] = 6;
    m[2][0] = 7; m[2][1] = 8; m[2][2] = 9;
    
    /* Before swap: m[0][1]=2, m[1][0]=4 */
    print_num(m[0][1]); putchar(' ');
    print_num(m[1][0]); putchar('\n');
    
    swap_elements(m, 0, 1, 1, 0);
    
    /* After swap: m[0][1]=4, m[1][0]=2 */
    print_num(m[0][1]); putchar(' ');
    print_num(m[1][0]); putchar('\n');
    
    return 0;
}
