int printf(const char *fmt, ...);
typedef unsigned short uint16_t;

uint16_t test_func(uint16_t a, uint16_t b) {
    return a + b;
}

int main(void) {
    uint16_t a = 5, b = 3;
    uint16_t result = test_func(a, b);
    printf("test_func(%d, %d) = %d\n", a, b, result);
    return 0;
}
