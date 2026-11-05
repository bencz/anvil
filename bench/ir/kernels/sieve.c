/* int32_t ksieve(uint8_t *composite, int32_t limit):
 *     count = 0;
 *     for (i = 2; i <= limit; i++) {
 *         if (composite[i]) continue;
 *         count++;
 *         if (i < 46341) for (j = i * i; j <= limit; j += i) composite[j] = 1;
 *     }
 *     return count; */
#include "../kit.h"

bool bench_build_sieve(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->u8), b->i32 };
    anvil_func_t *fn = bench_func(b, "ksieve", b->i32, params, 2);
    if (!fn)
        return false;
    anvil_value_t *composite = bench_param(fn, 0), *limit = bench_param(fn, 1);

    bench_loop_t outer, mark;
    bench_loop_open(b, &outer, fn, "numbers");
    anvil_value_t *i = bench_loop_carry(&outer, b->i32, anvil_const_i32(ctx, 2));
    anvil_value_t *count = bench_loop_carry(&outer, b->i32, anvil_const_i32(ctx, 0));
    bench_loop_test(&outer, anvil_build_cmp_le(ctx, i, limit, "more"));

    anvil_block_t *prime = anvil_block_create(fn, "prime"), *small = anvil_block_create(fn, "small"), *next = anvil_block_create(fn, "next");
    anvil_block_t *test = anvil_get_insert_block(ctx);
    anvil_value_t *flag = anvil_build_load(ctx, b->u8, bench_elem(b, b->u8, composite, bench_to_index(b, i, b->index, true)), "flag");
    anvil_build_br_cond(ctx, anvil_build_cmp_ne(ctx, flag, anvil_const_u8(ctx, 0), "known"), next, prime);

    anvil_set_insert_point(ctx, prime);
    anvil_value_t *counted = bench_nw(anvil_build_add(ctx, count, anvil_const_i32(ctx, 1), "counted"));
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, i, anvil_const_i32(ctx, 46341), "squarable"), small, next);

    anvil_set_insert_point(ctx, small);
    anvil_value_t *square = bench_nw(anvil_build_mul(ctx, i, i, "square"));
    bench_loop_open(b, &mark, fn, "multiples");
    anvil_value_t *j = bench_loop_carry(&mark, b->i32, square);
    bench_loop_test(&mark, anvil_build_cmp_le(ctx, j, limit, "inside"));
    anvil_build_store(ctx, anvil_const_u8(ctx, 1), bench_elem(b, b->u8, composite, bench_to_index(b, j, b->index, true)));
    anvil_value_t *step[] = { bench_nw(anvil_build_add(ctx, j, i, "j1")) };
    bench_loop_close(&mark, step);
    anvil_block_t *marked = anvil_get_insert_block(ctx);
    anvil_build_br(ctx, next);

    anvil_set_insert_point(ctx, next);
    anvil_value_t *total = anvil_build_phi(ctx, b->i32, "total");
    anvil_phi_add_incoming(total, count, test);
    anvil_phi_add_incoming(total, counted, prime);
    anvil_phi_add_incoming(total, counted, marked);
    anvil_value_t *advance[] = { bench_nw(anvil_build_add(ctx, i, anvil_const_i32(ctx, 1), "i1")), total };
    bench_loop_close(&outer, advance);
    return anvil_build_ret(ctx, count);
}
