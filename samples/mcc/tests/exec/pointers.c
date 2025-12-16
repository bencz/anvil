/* Test: pointers */
int putchar(int c);

void swap(int *a, int *b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

int main(void) {
    int x = 3;
    int y = 7;
    
    /* Before swap: 3, 7 */
    putchar('0' + x);
    putchar('0' + y);
    putchar('\n');
    
    swap(&x, &y);
    
    /* After swap: 7, 3 */
    putchar('0' + x);
    putchar('0' + y);
    putchar('\n');
    
    return 0;
}
