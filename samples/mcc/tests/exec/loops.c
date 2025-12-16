/* Test: loops */
int putchar(int c);

int main(void) {
    int i;
    
    /* while loop: print 0-4 */
    i = 0;
    while (i < 5) {
        putchar('0' + i);
        i = i + 1;
    }
    putchar('\n');
    
    /* for loop: print 5-9 */
    for (i = 5; i < 10; i = i + 1) {
        putchar('0' + i);
    }
    putchar('\n');
    
    return 0;
}
