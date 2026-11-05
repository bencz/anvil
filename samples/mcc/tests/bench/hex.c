/* Hex encoding: a byte promoted to int, divided, reduced and shifted as a
 * signed value whose sign bit is known zero. */
#include <stdio.h>

static unsigned char data[1 << 20];
static unsigned char text[2 << 20];

static unsigned encode(const unsigned char *s, unsigned long n, unsigned char *out)
{
    unsigned h = 0;
    for (unsigned long i = 0; i < n; i++) {
        int b = s[i];
        int hi = b / 16, lo = b % 16;
        out[2 * i] = (unsigned char)(hi + (hi > 9 ? 'a' - 10 : '0'));
        out[2 * i + 1] = (unsigned char)(lo + (lo > 9 ? 'a' - 10 : '0'));
        h = h * 31 + (unsigned)(hi ^ (b >> 2));
    }
    return h;
}

int main(void)
{
    unsigned seed = 12345;
    for (unsigned long i = 0; i < sizeof(data); i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (unsigned char)(seed >> 16);
    }
    unsigned h = 0;
    for (int rep = 0; rep < 40; rep++)
        h ^= encode(data, sizeof(data), text) + (unsigned)rep;
    unsigned sum = 0;
    for (unsigned long i = 0; i < sizeof(text); i += 97)
        sum = sum * 33 + text[i];
    printf("hex %u %u\n", h, sum);
    return 0;
}
