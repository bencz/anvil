/* Test: arrays */
int putchar(int c);

int main(void) {
    int arr[5];
    int i;
    
    /* Initialize array */
    for (i = 0; i < 5; i = i + 1) {
        arr[i] = i * 2;
    }
    
    /* Print array: 0, 2, 4, 6, 8 */
    for (i = 0; i < 5; i = i + 1) {
        putchar('0' + arr[i]);
    }
    putchar('\n');
    
    return 0;
}
