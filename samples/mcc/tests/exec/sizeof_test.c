/* Test: sizeof operator */
int putchar(int c);

int main(void) {
    /* Print sizes as digits */
    putchar('0' + sizeof(char));    /* 1 */
    putchar('0' + sizeof(short));   /* 2 */
    putchar('0' + sizeof(int));     /* 4 */
    putchar('0' + sizeof(long));    /* 4 or 8 */
    putchar('\n');
    
    /* Pointer size */
    int *p;
    putchar('0' + sizeof(p));       /* 4 or 8 */
    putchar('\n');
    
    return 0;
}
