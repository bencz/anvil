/* int32_t kfib(int32_t n) { return n < 2 ? n : kfib(n - 1) + kfib(n - 2); } */
#include "../kit.h"

bool bench_build_fib(bench_t *b)
{
    anvil_type_t *params[] = { b->i32 };
    anvil_func_t *fn = bench_func(b, "kfib", b->i32, params, 1);
    if (!fn)
        return false;
    anvil_value_t *n = bench_param(fn, 0);
    anvil_block_t *base = anvil_block_create(fn, "base"), *recurse = anvil_block_create(fn, "recurse");
    anvil_build_br_cond(b->ctx, anvil_build_cmp_lt(b->ctx, n, anvil_const_i32(b->ctx, 2), "small"), base, recurse);

    anvil_set_insert_point(b->ctx, base);
    anvil_build_ret(b->ctx, n);

    anvil_set_insert_point(b->ctx, recurse);
    anvil_value_t *a1[] = { bench_nw(anvil_build_sub(b->ctx, n, anvil_const_i32(b->ctx, 1), "n1")) };
    anvil_value_t *f1 = bench_call(b, fn, a1, 1);
    anvil_value_t *a2[] = { bench_nw(anvil_build_sub(b->ctx, n, anvil_const_i32(b->ctx, 2), "n2")) };
    anvil_value_t *f2 = bench_call(b, fn, a2, 1);
    return anvil_build_ret(b->ctx, bench_nw(anvil_build_add(b->ctx, f1, f2, "sum")));
}
