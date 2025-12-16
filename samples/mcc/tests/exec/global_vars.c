/* Test: global variables */
int putchar(int c);

int counter = 0;

void increment(void) {
    counter = counter + 1;
}

int main(void) {
    putchar('0' + counter);  /* 0 */
    increment();
    putchar('0' + counter);  /* 1 */
    increment();
    increment();
    putchar('0' + counter);  /* 3 */
    putchar('\n');
    
    return 0;
}
