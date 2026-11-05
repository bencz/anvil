/* void kqsort(int32_t *a, int32_t lo, int32_t hi):
 *     while (lo < hi) {
 *         pivot = a[lo + ((hi - lo) >> 1)];  i = lo;  j = hi;
 *         while (i <= j) {
 *             while (a[i] < pivot) i++;
 *             while (a[j] > pivot) j--;
 *             if (i <= j) { swap a[i], a[j]; i++; j--; }
 *         }
 *         kqsort(a, lo, j);  lo = i;
 *     } */
#include "../kit.h"

static anvil_value_t *slot(bench_t *b, anvil_value_t *a, anvil_value_t *i)
{
    return bench_elem(b, b->i32, a, bench_to_index(b, i, b->index, true));
}

bool bench_build_qsort(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->i32), b->i32, b->i32 };
    anvil_func_t *fn = bench_func(b, "kqsort", NULL, params, 3);
    if (!fn)
        return false;
    anvil_value_t *a = bench_param(fn, 0), *hi = bench_param(fn, 2), *one = anvil_const_i32(ctx, 1);

    bench_loop_t parts, partition, up, down;
    bench_loop_open(b, &parts, fn, "parts");
    anvil_value_t *lo = bench_loop_carry(&parts, b->i32, bench_param(fn, 1));
    bench_loop_test(&parts, anvil_build_cmp_lt(ctx, lo, hi, "more"));
    anvil_value_t *mid = bench_nw(anvil_build_add(ctx, lo, anvil_build_sar(ctx, bench_nw(anvil_build_sub(ctx, hi, lo, "span")), one, "half"), "mid"));
    anvil_value_t *pivot = anvil_build_load(ctx, b->i32, slot(b, a, mid), "pivot");

    bench_loop_open(b, &partition, fn, "partition");
    anvil_value_t *i = bench_loop_carry(&partition, b->i32, lo);
    anvil_value_t *j = bench_loop_carry(&partition, b->i32, hi);
    bench_loop_test(&partition, anvil_build_cmp_le(ctx, i, j, "open"));

    bench_loop_open(b, &up, fn, "up");
    anvil_value_t *ii = bench_loop_carry(&up, b->i32, i);
    bench_loop_test(&up, anvil_build_cmp_lt(ctx, anvil_build_load(ctx, b->i32, slot(b, a, ii), "x"), pivot, "below"));
    anvil_value_t *ii1[] = { bench_nw(anvil_build_add(ctx, ii, one, "ii1")) };
    bench_loop_close(&up, ii1);

    bench_loop_open(b, &down, fn, "down");
    anvil_value_t *jj = bench_loop_carry(&down, b->i32, j);
    bench_loop_test(&down, anvil_build_cmp_gt(ctx, anvil_build_load(ctx, b->i32, slot(b, a, jj), "y"), pivot, "above"));
    anvil_value_t *jj1[] = { bench_nw(anvil_build_sub(ctx, jj, one, "jj1")) };
    bench_loop_close(&down, jj1);

    anvil_block_t *scanned = anvil_get_insert_block(ctx);
    anvil_block_t *swap = anvil_block_create(fn, "swap"), *join = anvil_block_create(fn, "join");
    anvil_build_br_cond(ctx, anvil_build_cmp_le(ctx, ii, jj, "crossed"), swap, join);
    anvil_set_insert_point(ctx, swap);
    anvil_value_t *pi = slot(b, a, ii), *pj = slot(b, a, jj);
    anvil_value_t *x = anvil_build_load(ctx, b->i32, pi, "x"), *y = anvil_build_load(ctx, b->i32, pj, "y");
    anvil_build_store(ctx, y, pi);
    anvil_build_store(ctx, x, pj);
    anvil_value_t *i2 = bench_nw(anvil_build_add(ctx, ii, one, "i2")), *j2 = bench_nw(anvil_build_sub(ctx, jj, one, "j2"));
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, join);
    anvil_value_t *inext = anvil_build_phi(ctx, b->i32, "inext"), *jnext = anvil_build_phi(ctx, b->i32, "jnext");
    anvil_phi_add_incoming(inext, ii, scanned);
    anvil_phi_add_incoming(inext, i2, swap);
    anvil_phi_add_incoming(jnext, jj, scanned);
    anvil_phi_add_incoming(jnext, j2, swap);
    anvil_value_t *pnext[] = { inext, jnext };
    bench_loop_close(&partition, pnext);

    anvil_value_t *args[] = { a, lo, j };
    bench_call(b, fn, args, 3);
    anvil_value_t *lnext[] = { i };
    bench_loop_close(&parts, lnext);
    return anvil_build_ret_void(ctx);
}
