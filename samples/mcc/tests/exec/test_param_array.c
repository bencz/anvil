int putchar(int c);

void print_num(int n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n >= 10) print_num(n / 10);
    putchar('0' + n % 10);
}

void set_element(int arr[3], int idx, int val) {
    arr[idx] = val;
}

int main(void) {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    
    /* Before: 1 2 3 */
    print_num(arr[0]); putchar(' ');
    print_num(arr[1]); putchar(' ');
    print_num(arr[2]); putchar('\n');
    
    set_element(arr, 1, 99);
    
    /* After: 1 99 3 */
    print_num(arr[0]); putchar(' ');
    print_num(arr[1]); putchar(' ');
    print_num(arr[2]); putchar('\n');
    
    return 0;
}
