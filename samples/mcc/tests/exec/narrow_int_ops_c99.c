/* char/short arithmetic whose result depends on the operand's extension:
 * division, remainder, right shifts, compares and conversions to floating
 * point, with values that carry garbage above their width in a register. */
#include <stdio.h>

static volatile signed char vsc[] = { -128, -100, -7, -1, 0, 1, 7, 100, 127 };
static volatile unsigned char vuc[] = { 0, 1, 7, 100, 127, 128, 200, 255 };
static volatile short vss[] = { -32768, -1000, -7, -1, 0, 1, 7, 1000, 32767 };
static volatile unsigned short vus[] = { 0, 1, 7, 1000, 32767, 32768, 65535 };

#define COUNT(a) (int)(sizeof(a) / sizeof(a[0]))

static unsigned mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u + (h >> 9); }

/* Values computed in a wider type and narrowed, so the register keeps high
 * bits the narrow type must ignore. */
static signed char narrow_sc(int x) { return (signed char)(x * 257 + 3); }
static unsigned char narrow_uc(int x) { return (unsigned char)(x * 257 + 3); }

int main(void)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < COUNT(vsc); i++)
        for (int j = 0; j < COUNT(vsc); j++) {
            signed char a = vsc[i], b = vsc[j];
            if (b != 0 && !(a == -128 && b == -1)) {
                h = mix(h, (unsigned)(signed char)(a / b));
                h = mix(h, (unsigned)(signed char)(a % b));
            }
            h = mix(h, (unsigned)((a < b) | (a <= b) << 1 | (a > b) << 2 | (a == b) << 3));
            h = mix(h, (unsigned)(signed char)(a >> 1));
        }
    for (int i = 0; i < COUNT(vuc); i++)
        for (int j = 0; j < COUNT(vuc); j++) {
            unsigned char a = vuc[i], b = vuc[j];
            if (b != 0) {
                h = mix(h, (unsigned char)(a / b));
                h = mix(h, (unsigned char)(a % b));
            }
            h = mix(h, (unsigned)((a < b) | (a >= b) << 1 | (a != b) << 2));
            h = mix(h, (unsigned char)(a >> 3));
        }
    for (int i = 0; i < COUNT(vss); i++)
        for (int j = 0; j < COUNT(vus); j++) {
            short a = vss[i];
            unsigned short b = vus[j];
            if (b != 0) {
                h = mix(h, (unsigned)(short)(a / (short)b));
                h = mix(h, (unsigned short)(b / 3));
            }
            h = mix(h, (unsigned)((a < (short)b) | ((unsigned short)a < b) << 1));
            h = mix(h, (unsigned)(short)(a >> 4));
            h = mix(h, (unsigned short)(b >> 4));
        }
    double dsum = 0.0;
    float fsum = 0.0f;
    for (int i = 0; i < COUNT(vsc); i++) dsum += (double)vsc[i] * 3.0;
    for (int i = 0; i < COUNT(vuc); i++) dsum += (double)vuc[i];
    for (int i = 0; i < COUNT(vss); i++) fsum += (float)vss[i];
    for (int i = 0; i < COUNT(vus); i++) dsum += (double)vus[i] / 2.0;
    for (int x = -300; x <= 300; x += 37) {
        signed char s = narrow_sc(x);
        unsigned char u = narrow_uc(x);
        dsum += (double)s + (double)u;
        h = mix(h, (unsigned)(signed char)(s / 3));
        h = mix(h, (unsigned char)(u / 3));
        h = mix(h, (unsigned)((s < 0) | (u > 127) << 1));
    }
    printf("narrow %08x %d %d\n", h, (int)dsum, (int)fsum);
    return 0;
}
