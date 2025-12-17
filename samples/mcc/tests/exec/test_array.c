int printf(const char *fmt, ...);
typedef unsigned short uint16_t;

static const uint16_t lookup[4] = { 10, 20, 30, 40 };

int main(void) {
    uint16_t idx = 2;
    uint16_t val = lookup[idx];
    printf("lookup[%d] = %d\n", idx, val);
    return (val == 30) ? 0 : 1;
}
