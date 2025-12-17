/* Test: Pointer arithmetic */

int putchar(int c);

void print_digit(int n) {
    putchar('0' + n);
}

void print_num(int n) {
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    if (n >= 10) {
        print_num(n / 10);
    }
    print_digit(n % 10);
}

/* Sum array using pointer arithmetic */
int sum_array(int *arr, int n) {
    int sum = 0;
    int *end = arr + n;
    while (arr < end) {
        sum = sum + *arr;
        arr = arr + 1;
    }
    return sum;
}

/* Reverse array in place */
void reverse_array(int *arr, int n) {
    int *left = arr;
    int *right = arr + n - 1;
    int tmp;
    while (left < right) {
        tmp = *left;
        *left = *right;
        *right = tmp;
        left = left + 1;
        right = right - 1;
    }
}

int main(void) {
    int arr[5];
    int i;
    int *p;
    
    /* Initialize array */
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4; arr[4] = 5;
    
    /* Test sum using pointer arithmetic */
    print_num(sum_array(arr, 5));  /* 15 */
    putchar('\n');
    
    /* Test pointer increment */
    p = arr;
    print_num(*p);  /* 1 */
    p = p + 2;
    print_num(*p);  /* 3 */
    putchar('\n');
    
    /* Test reverse */
    reverse_array(arr, 5);
    for (i = 0; i < 5; i++) {
        print_num(arr[i]);
    }
    putchar('\n');  /* 54321 */
    
    return 0;
}
