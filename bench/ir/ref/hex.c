#include <stddef.h>
#include <stdint.h>

uint32_t khex(const uint8_t *s, size_t n, uint8_t *out)
{
    uint32_t h = 0;
    for (size_t i = 0; i < n; i++) {
        int b = s[i];
        int hi = b / 16, lo = b % 16;
        out[2 * i] = (uint8_t)(hi + (hi > 9 ? 'a' - 10 : '0'));
        out[2 * i + 1] = (uint8_t)(lo + (lo > 9 ? 'a' - 10 : '0'));
        h = h * 31 + (uint32_t)(hi ^ (b >> 2));
    }
    return h;
}
