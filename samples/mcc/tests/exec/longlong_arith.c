/* Exercise 64-bit arithmetic with long long. */

int putchar(int c);

static void print_digit(int d) { putchar('0' + d); }

static void print_llong(long long v) {
    char buf[32];
    int n = 0;
    long long x = v < 0 ? -v : v;
    if (v < 0) putchar('-');
    if (x == 0) { print_digit(0); return; }
    while (x > 0) {
        buf[n++] = (char)(x % 10);
        x /= 10;
    }
    while (n > 0) {
        print_digit(buf[--n]);
    }
}

int main(void) {
    long long a = 1000000LL;
    long long b = 1000000LL;
    long long p = a * b;  /* 1,000,000,000,000 — doesn't fit in i32 */
    print_llong(p);
    putchar('\n');
    return 0;
}
