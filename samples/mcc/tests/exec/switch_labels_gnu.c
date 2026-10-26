/* Test: switch labels as ordinary labels: Duff's device, labels inside
 * loops, declarations before the first case, nested switches, GNU case
 * ranges (signed and unsigned) and switches that never complete. */
#include <stdio.h>
static void duff(char *to, const char *from, int count)
{
    int n = (count + 7) / 8;
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
}
static int prefix_decl(int x) { switch (x) { int tmp; case 1: tmp = 10; return tmp + x; default: tmp = 20; return tmp; } }
static int nested(int a, int b) {
    int r = 0;
    switch (a) {
    case 1:
        switch (b) { case 1: r += 100; break; default: r += 200; }
        r += 1;
        /* fallthrough */
    case 2: r += 10; break;
    default: r = -1;
    }
    return r;
}
static int middle_default(int x) { int r = 0; switch (x) { case 1: r += 1; default: r += 2; case 3: r += 3; } return r; }
static int no_match(int x) { int r = 5; switch (x) { case 1: r = 1; } return r; }
static int all_return(int x) { switch (x) { case 1: return 10; default: return 20; } }
static int in_loop(int x) { int r = 0, i = 0; switch (x) { case 0: for (i = 0; i < 3; i++) { case 1: r += i + 1; } break; } return r; }
static int ranges(int x) { switch (x) { case -10 ... -1: return 1; case 0: return 2; case 1 ... 9: return 3; default: return 4; } }
static int uranges(unsigned x) { switch (x) { case 0 ... 0x7fffffffu: return 1; case 0x80000000u ... 0xffffffffu: return 2; } return 0; }
static int chars(char c) { switch (c) { case 'a': return 1; case 'z': return 26; case -1: return -1; } return 0; }
static long longs(long v) { switch (v) { case 4000000000L: return 1; case -4000000000L: return 2; default: return 3; } }
static int goto_in(int x) { int r = 0; if (x) goto inside; switch (x) { case 0: r = 1; break; inside: r = 7; } return r; }
static int scope(void) { int v = 5; switch (1) { case 1: { } int v = 9; (void)v; break; } return v; }
int main(void)
{
    char buf[32] = {0};
    int i;
    for (i = 0; i < 20; i++) { char out[32] = {0}; duff(out, "abcdefghijklmnopqrstuvwxyz", i); printf("%s|", out); }
    printf("\n%d %d %d %d %d %d\n", prefix_decl(1), prefix_decl(2), nested(1, 1), nested(1, 2), nested(2, 0), nested(9, 9));
    printf("%d %d %d %d %d\n", middle_default(1), middle_default(2), middle_default(3), no_match(1), no_match(2));
    printf("%d %d %d %d %d\n", all_return(1), all_return(3), in_loop(0), in_loop(1), in_loop(2));
    printf("%d %d %d %d %d\n", ranges(-5), ranges(0), ranges(5), ranges(50), ranges(-50));
    printf("%d %d %d\n", uranges(5), uranges(0x90000000u), uranges(0xffffffffu));
    printf("%d %d %d %d\n", chars('a'), chars('z'), chars(-1), chars('q'));
    printf("%ld %ld %ld\n", longs(4000000000L), longs(-4000000000L), longs(1));
    printf("%d %d %d %s\n", goto_in(0), goto_in(1), scope(), buf);
    return 0;
}
