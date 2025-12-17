int putchar(int c);

void print_num(int n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void debug_loop(int m[3][3]) {
    int i, j, tmp;
    for (i = 0; i < 2; i++) {
        putchar('i'); putchar('='); print_num(i); putchar('\n');
        for (j = i + 1; j < 3; j++) {
            putchar('j'); putchar('='); print_num(j); putchar('\n');
            putchar('m'); putchar('['); print_num(i); putchar(']');
            putchar('['); print_num(j); putchar(']'); putchar('=');
            print_num(m[i][j]); putchar('\n');
            
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
    
    debug_loop(m);
    
    putchar('\n');
    print_num(m[0][0]); putchar(' ');
    print_num(m[1][1]); putchar(' ');
    print_num(m[2][2]); putchar('\n');
    
    return 0;
}
