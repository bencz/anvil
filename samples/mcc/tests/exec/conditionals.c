/* Test: if/else conditionals */
int putchar(int c);

int main(void) {
    int x = 10;
    
    if (x > 5) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    
    if (x < 5) {
        putchar('A');
    } else if (x == 10) {
        putchar('B');
    } else {
        putchar('C');
    }
    putchar('\n');
    
    return 0;
}
