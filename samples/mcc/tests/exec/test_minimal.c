int printf(const char *fmt, ...);

int test_func(int a, int b) {
    return a + b;
}

int main(void) {
    int a = 5, b = 3;
    int result = test_func(a, b);
    printf("test_func(%d, %d) = %d\n", a, b, result);
    return 0;
}
