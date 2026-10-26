/* Platform-compiler side of the aggregate ABI conformance test: calls the
 * ANVIL-generated functions from tests/aggregate_abi_codegen.c and provides
 * the C functions they call.  Freestanding-friendly: only an exit status
 * reports the result, so it also runs without a C library. */
struct pair { long a; double b; };
struct small { signed char a, b, c; };
struct big { long a, b, c; };
struct hfa { float x, y, z; };
union num { float f[2]; double d; };

double anvil_sum_pair(struct pair p);
struct pair anvil_make_pair(long seed);
long anvil_press_small(long, long, long, long, long, struct small s, long t);
long anvil_big_roundtrip(struct big b, long seed);
struct hfa anvil_make_hfa(float v);
double anvil_hfa_dot(struct hfa a, struct hfa b);
double anvil_union_double(union num u);
union num anvil_make_union(float a, float b);
long anvil_mixed(double d, long a, float f, long b);
long anvil_spill(long, long, long, long, long, long, long, long, long, double, struct small);
long anvil_call_c(void);

long c_sum_big(struct big b) { return b.a + 10 * b.b + 100 * b.c; }
struct big c_make_big(long seed) { struct big b = { seed, seed + 1, seed + 2 }; return b; }
long c_mixed(double d, long a, float f, long b) { return a + 100 * b + 10000 * (long)d + 1000000 * (long)f; }
long c_spill(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9,
             double d, struct small s)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return a1 + 10 * a8 + 100 * a9 + 1000 * (long)d + s.a + 7 * s.b + 13 * s.c;
}
struct hfa c_make_hfa(float v) { struct hfa h = { v, v * 2, v * 3 }; return h; }
double c_hfa_sum(struct hfa h) { return h.x + h.y + h.z; }
struct pair c_make_pair(long seed) { struct pair p = { seed, seed * 0.5 }; return p; }

int aggregate_abi_check(void)
{
    struct pair p = { 7, 2.5 };
    if (anvil_sum_pair(p) != 9.5) return 1;
    struct pair made = anvil_make_pair(9);
    if (made.a != 9 || made.b != 4.5) return 2;
    struct small s = { 1, -2, 3 };
    if (anvil_press_small(1, 2, 3, 4, 5, s, 6) != 1 + 10 + 100 - 2000 + 30000 + 42) return 3;
    struct big b = { 1, 2, 3 };
    if (anvil_big_roundtrip(b, 4) != 321 * 3 + (4 + 50 + 600)) return 4;
    struct hfa h = anvil_make_hfa(1.5f);
    if (h.x != 1.5f || h.y != 3.0f || h.z != 4.5f) return 5;
    struct hfa u = { 1.0f, 2.0f, 3.0f };
    if (anvil_hfa_dot(h, u) != 1.5 + 6.0 + 13.5) return 6;
    union num n;
    n.d = -3.25;
    if (anvil_union_double(n) != -3.25) return 7;
    union num m = anvil_make_union(0.5f, -1.0f);
    if (m.f[0] != 0.5f || m.f[1] != -1.0f) return 8;
    if (anvil_mixed(2.0, 3, 5.0f, 7) != c_mixed(2.0, 3, 5.0f, 7)) return 9;
    if (anvil_spill(1, 2, 3, 4, 5, 6, 7, 8, 9, 4.0, s) != c_spill(1, 2, 3, 4, 5, 6, 7, 8, 9, 4.0, s)) return 10;
    if (anvil_call_c() != c_mixed(2.0, 3, 5.0f, 7) + c_spill(1, 2, 3, 4, 5, 6, 7, 8, 9, 4.0, s) + 12 + 5 + 5)
        return 11;
    return 0;
}

#ifdef AGGREGATE_ABI_FREESTANDING
int test_main(void) { return aggregate_abi_check(); }
#else
int main(void) { return aggregate_abi_check(); }
#endif
