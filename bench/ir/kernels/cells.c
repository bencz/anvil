/* void kcells(int32_t *t, int32_t n):
 *     for (k = 1; k < n; k++) while (t[k]) { t[k - 1] += 3; t[k]--; }
 * A tape machine's inner loop: the loop tests one cell and moves it into
 * its neighbour, through an index the loop does not change. */
#include "../kit.h"

bool bench_build_cells(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->i32), b->i32 };
    anvil_func_t *fn = bench_func(b, "kcells", NULL, params, 2);
    if (!fn)
        return false;
    anvil_value_t *t = bench_param(fn, 0), *n = bench_param(fn, 1);
    anvil_value_t *zero = anvil_const_i32(ctx, 0), *one = anvil_const_i32(ctx, 1);

    bench_loop_t cells, drain;
    bench_loop_open(b, &cells, fn, "cells");
    anvil_value_t *k = bench_loop_carry(&cells, b->i32, one);
    bench_loop_test(&cells, anvil_build_cmp_lt(ctx, k, n, "more"));
    anvil_value_t *here = bench_elem(b, b->i32, t, bench_to_index(b, k, b->index, true));
    anvil_value_t *left = bench_elem(b, b->i32, t, bench_to_index(b, bench_nw(anvil_build_sub(ctx, k, one, "left")), b->index, true));
    bench_loop_open(b, &drain, fn, "drain");
    anvil_value_t *value = anvil_build_load(ctx, b->i32, here, "value");
    bench_loop_test(&drain, anvil_build_cmp_ne(ctx, value, zero, "full"));
    anvil_build_store(ctx, bench_nw(anvil_build_add(ctx, anvil_build_load(ctx, b->i32, left, "neighbour"), anvil_const_i32(ctx, 3), "moved")), left);
    anvil_build_store(ctx, bench_nw(anvil_build_sub(ctx, value, one, "less")), here);
    bench_loop_close(&drain, NULL);
    anvil_value_t *next[] = { bench_nw(anvil_build_add(ctx, k, one, "k1")) };
    bench_loop_close(&cells, next);
    return anvil_build_ret_void(ctx);
}
