/* void kmatmul(double *c, const double *a, const double *b, int32_t n):
 *     for (i = 0; i < n; i++)
 *         for (k = 0; k < n; k++) {
 *             double aik = a[i * n + k];
 *             for (j = 0; j < n; j++) c[i * n + j] += aik * b[k * n + j];
 *         } */
#include "../kit.h"

bool bench_build_matmul(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *pf64 = anvil_type_ptr(ctx, b->f64);
    anvil_type_t *params[] = { pf64, pf64, pf64, b->i32 };
    anvil_func_t *fn = bench_func(b, "kmatmul", NULL, params, 4);
    if (!fn)
        return false;
    /* C's defaults under the reference compiler: lane-wise FP is allowed
     * (no FP environment access) and a*b + c within one expression may
     * fuse (-ffp-contract=on). */
    anvil_func_set_fp_vectorization(fn, true);
    anvil_func_set_fp_contraction(fn, true);
    anvil_value_t *c = bench_param(fn, 0), *a = bench_param(fn, 1), *m = bench_param(fn, 2), *n = bench_param(fn, 3);
    anvil_value_t *zero = anvil_const_i32(ctx, 0), *one = anvil_const_i32(ctx, 1);

    bench_loop_t rows, inner, cols;
    bench_loop_open(b, &rows, fn, "rows");
    anvil_value_t *i = bench_loop_carry(&rows, b->i32, zero);
    bench_loop_test(&rows, anvil_build_cmp_lt(ctx, i, n, "more"));
    anvil_value_t *row = bench_nw(anvil_build_mul(ctx, i, n, "row"));

    bench_loop_open(b, &inner, fn, "inner");
    anvil_value_t *k = bench_loop_carry(&inner, b->i32, zero);
    bench_loop_test(&inner, anvil_build_cmp_lt(ctx, k, n, "more"));
    anvil_value_t *aik = anvil_build_load(ctx, b->f64, bench_elem(b, b->f64, a, bench_to_index(b, bench_nw(anvil_build_add(ctx, row, k, "ik")), b->index, true)), "aik");
    anvil_value_t *krow = bench_nw(anvil_build_mul(ctx, k, n, "krow"));

    bench_loop_open(b, &cols, fn, "cols");
    anvil_value_t *j = bench_loop_carry(&cols, b->i32, zero);
    bench_loop_test(&cols, anvil_build_cmp_lt(ctx, j, n, "more"));
    anvil_value_t *bkj = anvil_build_load(ctx, b->f64, bench_elem(b, b->f64, m, bench_to_index(b, bench_nw(anvil_build_add(ctx, krow, j, "kj")), b->index, true)), "bkj");
    anvil_value_t *cij = bench_elem(b, b->f64, c, bench_to_index(b, bench_nw(anvil_build_add(ctx, row, j, "ij")), b->index, true));
    anvil_value_t *sum = anvil_build_fadd(ctx, anvil_build_load(ctx, b->f64, cij, "acc"), anvil_build_fmul(ctx, aik, bkj, "prod"), "sum");
    anvil_build_store(ctx, sum, cij);
    anvil_value_t *jn[] = { bench_nw(anvil_build_add(ctx, j, one, "j1")) };
    bench_loop_close(&cols, jn);

    anvil_value_t *kn[] = { bench_nw(anvil_build_add(ctx, k, one, "k1")) };
    bench_loop_close(&inner, kn);
    anvil_value_t *in[] = { bench_nw(anvil_build_add(ctx, i, one, "i1")) };
    bench_loop_close(&rows, in);
    return anvil_build_ret_void(ctx);
}
