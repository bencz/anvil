#include "bench.h"

int32_t kstrlen(const char *s);
int32_t kstrcmp(const char *a, const char *b);
void kstrcpy(char *d, const char *s);
void kreverse(char *s, int32_t n);

static char text[BENCH_PICK(1 << 16, 1 << 12)];
static char copy[BENCH_PICK(1 << 16, 1 << 12)];

int main(void)
{
    for (int i = 0; i < (int)sizeof(text) - 1; i++)
        text[i] = (char)('a' + (i * 7 + i / 13) % 26);
    long check = 0;
    for (int rep = 0; rep < BENCH_PICK(1500, 6); rep++) {
        kstrcpy(copy, text);
        int n = kstrlen(copy);
        kreverse(copy, n);
        check += kstrcmp(copy, text) + n + copy[rep % n];
    }
    printf("strings %ld\n", check);
    return 0;
}
