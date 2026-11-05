/* Switches with many cases lower to sorted runs and a balanced tree of
 * compares: dense and sparse values, negative ones, runs of consecutive
 * values with one destination, unsigned and 64-bit selectors, values at
 * the ends of the type, fall-through and default. */
#include <stdio.h>

static int dense(int x)
{
    switch (x) {
    case 0: return 10; case 1: return 11; case 2: return 12; case 3: return 13;
    case 4: return 14; case 5: return 15; case 6: return 16; case 7: return 17;
    case 8: return 18; case 9: return 19; case 10: return 20; case 11: return 21;
    default: return -1;
    }
}

static int sparse(int x)
{
    switch (x) {
    case -1000: return 1; case -7: return 2; case 3: return 3; case 64: return 4;
    case 65: return 5; case 1000: return 6; case 1001: return 7; case 99999: return 8;
    case 2147483647: return 9; case -2147483647 - 1: return 10;
    default: return 0;
    }
}

static int ranges(int c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        return 1;
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
        return 2;
    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        return 3;
    case ' ': case '\t': case '\n':
        return 4;
    case '_':
        return 5;
    default:
        return 0;
    }
}

static unsigned unsigned_cases(unsigned x)
{
    switch (x) {
    case 0u: return 1; case 1u: return 2; case 0x7fffffffu: return 3; case 0x80000000u: return 4;
    case 0xfffffffeu: return 5; case 0xffffffffu: return 6; case 100u: return 7; case 101u: return 8;
    default: return 0;
    }
}

static int wide(long long x)
{
    switch (x) {
    case -5000000000LL: return 1; case -1: return 2; case 0: return 3; case 1: return 4;
    case 4294967296LL: return 5; case 4294967297LL: return 6; case 9223372036854775807LL: return 7;
    default: return 0;
    }
}

static int fallthrough(int x)
{
    int r = 0;
    switch (x) {
    case 1: r += 1;
    case 2: r += 2;
    case 3: r += 4; break;
    case 7: r += 8;
    case 8: r += 16; break;
    case 20: case 21: case 22: r += 32;
    default: r += 64;
    }
    return r;
}

int main(void)
{
    unsigned long check = 0;
    for (int x = -40; x < 1100; x++) {
        check = check * 31 + (unsigned long)(dense(x) + 100);
        check = check * 31 + (unsigned long)sparse(x);
        check = check * 31 + (unsigned long)ranges(x);
        check = check * 31 + (unsigned long)fallthrough(x);
    }
    int edges[] = {-1000, -7, 3, 64, 65, 1000, 1001, 99999, 2147483647, -2147483647 - 1, 0, 2, 66};
    for (int i = 0; i < (int)(sizeof(edges) / sizeof(edges[0])); i++)
        check = check * 31 + (unsigned long)sparse(edges[i]);
    unsigned uedges[] = {0u, 1u, 2u, 99u, 100u, 101u, 102u, 0x7ffffffeu, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu};
    for (int i = 0; i < (int)(sizeof(uedges) / sizeof(uedges[0])); i++)
        check = check * 31 + unsigned_cases(uedges[i]);
    long long wedges[] = {-5000000000LL, -4999999999LL, -2, -1, 0, 1, 2, 4294967295LL, 4294967296LL, 4294967297LL, 4294967298LL,
                          9223372036854775807LL, -9223372036854775807LL - 1};
    for (int i = 0; i < (int)(sizeof(wedges) / sizeof(wedges[0])); i++)
        check = check * 31 + (unsigned long)wide(wedges[i]);
    printf("switch tree %lu\n", check);
    return 0;
}
