int putchar(int c);

void print_num(int n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void swap_step(int m[3][3]) {
    int i, j, tmp;
    i = 0;
    j = 1;
    
    /* Step 1: tmp = m[0][1] */
    tmp = m[i][j];
    putchar('1'); putchar(':'); print_num(tmp); putchar('\n');
    
    /* Step 2: m[0][1] = m[1][0] */
    m[i][j] = m[j][i];
    putchar('2'); putchar(':'); print_num(m[i][j]); putchar('\n');
    
    /* Step 3: m[1][0] = tmp */
    m[j][i] = tmp;
    putchar('3'); putchar(':'); print_num(m[j][i]); putchar('\n');
    
    /* Now check m[0][2] - should still be 3 */
    putchar('m'); putchar('['); putchar('0'); putchar(']');
    putchar('['); putchar('2'); putchar(']'); putchar('=');
    print_num(m[0][2]); putchar('\n');
}

int main(void) {
    int m[3][3];
    
    m[0][0] = 1; m[0][1] = 2; m[0][2] = 3;
    m[1][0] = 4; m[1][1] = 5; m[1][2] = 6;
    m[2][0] = 7; m[2][1] = 8; m[2][2] = 9;
    
    swap_step(m);
    
    return 0;
}
