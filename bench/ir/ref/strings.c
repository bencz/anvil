#include <stdint.h>

int32_t kstrlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (int32_t)(p - s);
}

int32_t kstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

void kstrcpy(char *d, const char *s)
{
    while ((*d++ = *s++) != 0) {
    }
}

void kreverse(char *s, int32_t n)
{
    for (int32_t i = 0, j = n - 1; i < j; i++, j--) {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
}
