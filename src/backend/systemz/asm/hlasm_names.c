#include "../systemz_internal.h"

void systemz_uppercase(char *dest, const char *src, size_t max_len)
{
    size_t i = 0;
    if (!dest || max_len == 0)
        return;
    if (!src)
        src = "ANON";
    for (; i + 1 < max_len && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            dest[i] = c;
        } else {
            dest[i] = '_';
        }
    }
    dest[i] = '\0';
}
