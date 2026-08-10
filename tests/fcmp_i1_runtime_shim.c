#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DECL(N) extern _Bool anvil_fcmp_##N(double, double)
DECL(0); DECL(1); DECL(2); DECL(3); DECL(4); DECL(5); DECL(6); DECL(7);
DECL(8); DECL(9); DECL(10); DECL(11); DECL(12); DECL(13); DECL(14); DECL(15);
extern _Bool anvil_load_i1(const unsigned char *);
extern _Bool anvil_trunc_i1(unsigned char);
extern void anvil_store_i1(_Bool, unsigned char *);
extern _Bool anvil_fptoui_i1(double);

typedef _Bool (*cmp_fn)(double, double);
static cmp_fn funcs[] = {
    anvil_fcmp_0, anvil_fcmp_1, anvil_fcmp_2, anvil_fcmp_3,
    anvil_fcmp_4, anvil_fcmp_5, anvil_fcmp_6, anvil_fcmp_7,
    anvil_fcmp_8, anvil_fcmp_9, anvil_fcmp_10, anvil_fcmp_11,
    anvil_fcmp_12, anvil_fcmp_13, anvil_fcmp_14, anvil_fcmp_15
};

static _Bool oracle(int pred, double a, double b)
{
    _Bool u = isnan(a) || isnan(b);
    _Bool l = !u && a < b, g = !u && a > b, e = !u && a == b;
    static const unsigned masks[] = {
        0, 4, 2, 6, 1, 5, 3, 7, 12, 10, 14, 9, 13, 11, 8, 15
    };
    unsigned category = u ? 8u : l ? 1u : g ? 2u : e ? 4u : 0u;
    return (masks[pred] & category) != 0;
}

int main(void)
{
    const double inf = INFINITY, nan = NAN;
    const double cases[][2] = {
        {-1.0, 2.0}, {2.0, -1.0}, {3.0, 3.0}, {0.0, -0.0},
        {nan, 1.0}, {1.0, nan}, {nan, nan}, {inf, -inf}, {-inf, inf}
    };
    for (int p = 0; p < 16; p++) for (size_t i = 0; i < 9; i++) {
        _Bool got = funcs[p](cases[i][0], cases[i][1]);
        _Bool want = oracle(p, cases[i][0], cases[i][1]);
        if (got != want) {
            fprintf(stderr, "FCMP predicate %d case %zu: got %d want %d\n",
                    p, i, got, want);
            return 1;
        }
    }
    unsigned char two = 2, three = 3, out = 0xff;
    if (anvil_load_i1(&two) != 0 || anvil_load_i1(&three) != 1 ||
        anvil_trunc_i1(2) != 0 || anvil_trunc_i1(3) != 1 ||
        anvil_fptoui_i1(0.0) != 0 || anvil_fptoui_i1(1.0) != 1)
        return 2;
    anvil_store_i1(1, &out);
    if (out != 1) return 3;
    anvil_store_i1(0, &out);
    if (out != 0) return 4;
    puts("native FCMP/i1 execution conformance passed");
    return 0;
}
