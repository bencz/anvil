/* void kntt(int32_t *t, int32_t n, int32_t s, int32_t d, int32_t h):
 *     for (i = s; i < 1 << 25; i *= 2) d = d * d % M;
 *     for (k = 0; k < n; k += 2s) for (j = 0, c = 1; j < s; j++) { p = t + k + j;
 *         a = p[s] * (h ? c : 1) % M; p[s] = ((u32)M + *p - a) * (h ? 1 : c) % M;
 *         *p = ((u32)a + *p) % (u32)M; c = c * d % M; }
 * One pass of a number-theoretic transform modulo the prime M = 1811939329:
 * 64-bit products reduced by a constant, 32-bit unsigned sums. */
#include "../kit.h"

enum { NTT_PRIME = 1811939329 };

static anvil_value_t *reduce(bench_t *b, anvil_value_t *product, const char *name)
{
    return anvil_build_trunc(b->ctx, anvil_build_smod(b->ctx, product, anvil_const_i64(b->ctx, NTT_PRIME), "mod"), b->i32, name);
}

static anvil_value_t *wide(bench_t *b, anvil_value_t *value)
{
    return anvil_build_sext(b->ctx, value, b->i64, "wide");
}

bool bench_build_bigprime(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *pint = anvil_type_ptr(ctx, b->i32);
    anvil_type_t *params[] = { pint, b->i32, b->i32, b->i32, b->i32 };
    anvil_func_t *fn = bench_func(b, "kntt", NULL, params, 5);
    if (!fn)
        return false;
    anvil_value_t *t = bench_param(fn, 0), *n = bench_param(fn, 1), *s = bench_param(fn, 2), *h = bench_param(fn, 4);
    anvil_value_t *inverse = anvil_build_cmp_ne(ctx, h, anvil_const_i32(ctx, 0), "inverse");

    bench_loop_t root;
    bench_loop_open(b, &root, fn, "root");
    anvil_value_t *i = bench_loop_carry(&root, b->i32, s);
    anvil_value_t *d = bench_loop_carry(&root, b->i32, bench_param(fn, 3));
    bench_loop_test(&root, anvil_build_cmp_lt(ctx, i, anvil_const_i32(ctx, 1 << 25), "more"));
    anvil_value_t *root_next[] = { bench_nw(anvil_build_mul(ctx, i, anvil_const_i32(ctx, 2), "i2")),
                                   reduce(b, bench_nw(anvil_build_mul(ctx, wide(b, d), wide(b, d), "dd")), "d1") };
    bench_loop_close(&root, root_next);

    bench_loop_t blocks;
    bench_loop_open(b, &blocks, fn, "blocks");
    anvil_value_t *k = bench_loop_carry(&blocks, b->i32, anvil_const_i32(ctx, 0));
    bench_loop_test(&blocks, anvil_build_cmp_lt(ctx, k, n, "more"));
    bench_loop_t pairs;
    bench_loop_open(b, &pairs, fn, "pairs");
    anvil_value_t *j = bench_loop_carry(&pairs, b->i32, anvil_const_i32(ctx, 0));
    anvil_value_t *c = bench_loop_carry(&pairs, b->i32, anvil_const_i32(ctx, 1));
    bench_loop_test(&pairs, anvil_build_cmp_lt(ctx, j, s, "more"));
    anvil_value_t *at = bench_nw(anvil_build_add(ctx, k, j, "at"));
    anvil_value_t *low = bench_elem(b, b->i32, t, bench_to_index(b, at, b->index, true));
    anvil_value_t *high = bench_elem(b, b->i32, t, bench_to_index(b, bench_nw(anvil_build_add(ctx, at, s, "far")), b->index, true));
    anvil_value_t *one = anvil_const_i64(ctx, 1);
    anvil_value_t *x = anvil_build_load(ctx, b->i32, low, "x"), *y = anvil_build_load(ctx, b->i32, high, "y");
    anvil_value_t *a = reduce(b, bench_nw(anvil_build_mul(ctx, wide(b, y), anvil_build_select(ctx, inverse, wide(b, c), one, "cy"), "ya")), "a");
    anvil_value_t *ux = anvil_build_bitcast(ctx, x, b->u32, "ux"), *ua = anvil_build_bitcast(ctx, a, b->u32, "ua");
    anvil_value_t *difference = anvil_build_sub(ctx, anvil_build_add(ctx, anvil_const_u32(ctx, NTT_PRIME), ux, "mx"), ua, "diff");
    anvil_value_t *scaled = bench_nw(anvil_build_mul(ctx, anvil_build_zext(ctx, difference, b->i64, "dw"),
                                                     anvil_build_select(ctx, inverse, one, wide(b, c), "cd"), "scaled"));
    anvil_build_store(ctx, reduce(b, scaled, "y1"), high);
    anvil_value_t *sum = anvil_build_umod(ctx, anvil_build_add(ctx, ua, ux, "sum"), anvil_const_u32(ctx, NTT_PRIME), "x1");
    anvil_build_store(ctx, anvil_build_bitcast(ctx, sum, b->i32, "x1i"), low);
    anvil_value_t *pairs_next[] = { bench_nw(anvil_build_add(ctx, j, anvil_const_i32(ctx, 1), "j1")),
                                    reduce(b, bench_nw(anvil_build_mul(ctx, wide(b, c), wide(b, d), "cd")), "c1") };
    bench_loop_close(&pairs, pairs_next);
    anvil_value_t *blocks_next[] = { bench_nw(anvil_build_add(ctx, k, bench_nw(anvil_build_mul(ctx, s, anvil_const_i32(ctx, 2), "s2")), "k1")) };
    bench_loop_close(&blocks, blocks_next);
    return anvil_build_ret_void(ctx);
}
