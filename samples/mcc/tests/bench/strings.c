/* Byte loops: hand-written strlen/compare/copy/reverse on many strings. */
#include <stdio.h>

static char text[1 << 16];
static char copy[1 << 16];

static int my_strlen(const char *s) { const char *p = s; while (*p) p++; return (int)(p - s); }
static int my_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static void my_strcpy(char *d, const char *s) { while ((*d++ = *s++) != 0) {} }
static void reverse(char *s, int n) { for (int i = 0, j = n - 1; i < j; i++, j--) { char t = s[i]; s[i] = s[j]; s[j] = t; } }

int main(void)
{
    for (int i = 0; i < (int)sizeof(text) - 1; i++)
        text[i] = (char)('a' + (i * 7 + i / 13) % 26);
    text[sizeof(text) - 1] = 0;
    long check = 0;
    for (int rep = 0; rep < 600; rep++) {
        my_strcpy(copy, text);
        int n = my_strlen(copy);
        reverse(copy, n);
        check += my_strcmp(copy, text) + n + copy[rep % n];
    }
    printf("strings %ld\n", check);
    return 0;
}
