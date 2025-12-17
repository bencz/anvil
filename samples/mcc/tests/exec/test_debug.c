int printf(const char *fmt, ...);

int test_func(int a, int b) {
    printf("Inside test_func: a=%d, b=%d\n", a, b);
    return a + b;
}

int main(void) {
    int a = 5, b = 3;
    printf("Before call: a=%d, b=%d\n", a, b);
    int result = test_func(a, b);
    printf("After call: a=%d, b=%d, result=%d\n", a, b, result);
    return 0;
}
