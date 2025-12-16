/* Test: function calls */
int putchar(int c);

int add(int a, int b) {
    return a + b;
}

int multiply(int a, int b) {
    return a * b;
}

int main(void) {
    int x = add(3, 4);      /* 7 */
    int y = multiply(2, 3); /* 6 */
    int z = add(x, y);      /* 13 */
    
    /* Print 13 */
    putchar('0' + z / 10);
    putchar('0' + z % 10);
    putchar('\n');
    
    return 0;
}
