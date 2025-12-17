int printf(const char *fmt, ...);
typedef unsigned short uint16_t;

uint16_t add_u16(uint16_t a, uint16_t b) {
    return a + b;
}

int main(void) {
    uint16_t a = 5, b = 3;
    uint16_t result = add_u16(a, b);
    printf("add_u16(%d, %d) = %d\n", a, b, result);
    return (result == 8) ? 0 : 1;
}
