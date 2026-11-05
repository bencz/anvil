/* Searches without a bound: the loop reads until a lane condition holds.
 * Vectorized, they read aligned vectors from the one holding the start
 * (lanes before it masked off) and resume the scalar loop at a vector with
 * a hit.  Every start alignment, every length, terminators right at the
 * end of a buffer, and element widths of 1, 2 and 4 bytes. */
#include <stdio.h>

typedef unsigned short u16;
typedef unsigned int u32;

static int length_by_pointer(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (int)(p - s);
}

static int length_by_index(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static long find_char(const unsigned char *s, unsigned char c)
{
    long i = 0;
    while (s[i] != c)
        i++;
    return i;
}

static int length16(const u16 *s)
{
    const u16 *p = s;
    while (*p)
        p++;
    return (int)(p - s);
}

static int length32(const u32 *s)
{
    int n = 0;
    while (s[n] != 0)
        n++;
    return n;
}

static int skip_spaces(const char *s, int from)
{
    while (s[from] == ' ')
        from++;
    return from;
}

static char text[160];
static u16 wide[96];
static u32 wider[96];

int main(void)
{
    unsigned long check = 0;
    for (int start = 0; start < 32; start++) {
        for (int len = 0; len + start < 150; len += (len < 40 ? 1 : 7)) {
            for (int k = 0; k < (int)sizeof(text); k++)
                text[k] = (char)('a' + (k * 7) % 26);
            /* A terminator before the start, inside the first vector: it
             * must not count. */
            if (start > 0)
                text[start - 1] = 0;
            text[start + len] = 0;
            int a = length_by_pointer(text + start), b = length_by_index(text + start);
            long c = find_char((const unsigned char *)text + start, 0);
            if (a != len || b != len || c != len)
                printf("mismatch start=%d len=%d: %d %d %ld\n", start, len, a, b, c);
            check = check * 31 + (unsigned long)(a + b + c);
        }
    }
    /* Terminators at the very end of the buffer. */
    for (int start = 140; start < 160; start++) {
        for (int k = 0; k < 159; k++)
            text[k] = 'x';
        text[159] = 0;
        check = check * 31 + (unsigned long)length_by_pointer(text + start);
    }
    for (int start = 0; start < 16; start++) {
        for (int len = 0; len + start < 90; len += 3) {
            for (int k = 0; k < 96; k++) {
                wide[k] = (u16)(k + 1);
                wider[k] = (u32)(k * 1000 + 1);
            }
            wide[start + len] = 0;
            wider[start + len] = 0;
            int a = length16(wide + start), b = length32(wider + start);
            if (a != len || b != len)
                printf("wide mismatch start=%d len=%d: %d %d\n", start, len, a, b);
            check = check * 31 + (unsigned long)(a * 3 + b);
        }
    }
    const char *spaced = "        x  y";
    for (int from = 0; from < 12; from++)
        check = check * 31 + (unsigned long)skip_spaces(spaced, from);
    printf("open search %lu\n", check);
    return 0;
}
