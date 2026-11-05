/* Run-time helpers (libgcc ABI) for freestanding qemu-user runs. */
typedef long long di; typedef unsigned long long du; typedef unsigned su;
du __umoddi3(du a, du b);
static du udivmod(du n, du d, du *rem)
{
    du q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
du __udivdi3(du a, du b) { return udivmod(a, b, 0); }
du __umoddi3(du a, du b) { du r; udivmod(a, b, &r); return r; }
di __divdi3(di a, di b) { int neg = (a < 0) != (b < 0); du q = udivmod(a < 0 ? 0 - (du)a : (du)a, b < 0 ? 0 - (du)b : (du)b, 0); return neg ? (di)(0 - q) : (di)q; }
di __moddi3(di a, di b) { du r; udivmod(a < 0 ? 0 - (du)a : (du)a, b < 0 ? 0 - (du)b : (du)b, &r); return a < 0 ? (di)(0 - r) : (di)r; }
di __muldi3(di a, di b)
{
    su al = (su)a, ah = (su)((du)a >> 32), bl = (su)b, bh = (su)((du)b >> 32);
    su a0 = al & 0xffff, a1 = al >> 16, b0 = bl & 0xffff, b1 = bl >> 16;
    su p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    su mid = (p00 >> 16) + (p01 & 0xffff) + (p10 & 0xffff);
    su lo = (p00 & 0xffff) | (mid << 16);
    su hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16) + al * bh + ah * bl;
    return (di)(((du)hi << 32) | lo);
}
di __ashldi3(di a, int s) { return (di)((du)a << s); }
du __lshrdi3(du a, int s) { return a >> s; }
di __ashrdi3(di a, int s) { return a >> s; }
static double u64_to_double(du v)
{
    /* Round to 53 bits with a sticky bit, then scale (exact). */
    int shift = 0;
    while ((v >> shift) >= (1ULL << 53)) shift++;
    if (shift) { du kept = v >> (shift - 1); int sticky = (v & ((1ULL << (shift - 1)) - 1)) != 0;
        du m = kept >> 1; int half = kept & 1;
        if (half && (sticky || (m & 1))) m++;
        double d = (double)(su)(m >> 32) * 4294967296.0 + (double)(su)m; while (shift--) d *= 2.0; return d; }
    return (double)(su)(v >> 32) * 4294967296.0 + (double)(su)v;
}
static float u64_to_float(du v)
{
    int shift = 0;
    while ((v >> shift) >= (1ULL << 24)) shift++;
    du m = v;
    if (shift) { du kept = v >> (shift - 1); int sticky = (v & ((1ULL << (shift - 1)) - 1)) != 0;
        m = kept >> 1; if ((kept & 1) && (sticky || (m & 1))) m++; }
    float f = (float)(su)m; while (shift-- > 0) f *= 2.0f; return f;
}
double __floatundidf(du v) { return u64_to_double(v); }
double __floatdidf(di v) { return v < 0 ? -u64_to_double(0 - (du)v) : u64_to_double((du)v); }
float __floatundisf(du v) { return u64_to_float(v); }
float __floatdisf(di v) { return v < 0 ? -u64_to_float(0 - (du)v) : u64_to_float((du)v); }
du __fixunsdfdi(double d)
{
    if (d < 1.0) return 0;
    du hi = (du)(su)(d / 4294967296.0);
    double rest = d - (double)(su)hi * 4294967296.0;
    if (rest < 0) { hi--; rest += 4294967296.0; }
    return (hi << 32) + (su)rest;
}
di __fixdfdi(double d) { return d < 0 ? (di)(0 - __fixunsdfdi(-d)) : (di)__fixunsdfdi(d); }
du __fixunssfdi(float f) { return __fixunsdfdi(f); }
di __fixsfdi(float f) { return __fixdfdi(f); }
su __fixunsdfsi(double d) { return d >= 2147483648.0 ? (su)(int)(d - 2147483648.0) ^ 0x80000000u : (su)(int)d; }
su __fixunssfsi(float f) { return __fixunsdfsi(f); }
/* Library square roots for the NaN fallback path of inline sqrt. */
#if defined(__powerpc__) && !defined(__powerpc64__)
double sqrt(double x) { double r; __asm__("fsqrt %0, %1" : "=f"(r) : "f"(x)); return r; }
float sqrtf(float x) { double r; __asm__("fsqrt %0, %1" : "=f"(r) : "f"((double)x)); return (float)r; }
#else
double sqrt(double x) { return __builtin_elementwise_sqrt(x); }
float sqrtf(float x) { return __builtin_elementwise_sqrt(x); }
#endif
