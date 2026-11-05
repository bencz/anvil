/* uint32_t klex(const uint8_t *s, size_t n):
 *     h = 0; for (i = 0; i < n; i++) { switch (s[i]) { digits: k = 1; lower: k = 2; upper: k = 3;
 *     ' ', '\t', '\n': k = 4; '+', '-', '*', '/', '=': k = 5; '(', ')', '{', '}', ';': k = 6; default: k = 7; }
 *     h = h * 31 + k; }  return h;
 * A lexer's byte classification: one switch of 74 cases in seven classes. */
#include "../kit.h"

#include <stdio.h>

bool bench_build_lexer(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->u8), b->uindex };
    anvil_func_t *fn = bench_func(b, "klex", b->u32, params, 2);
    if (!fn)
        return false;
    anvil_value_t *s = bench_param(fn, 0), *n = bench_param(fn, 1);

    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "bytes");
    anvil_value_t *i = bench_loop_carry(&loop, b->uindex, bench_index_const(b, b->uindex, 0));
    anvil_value_t *h = bench_loop_carry(&loop, b->u32, anvil_const_u32(ctx, 0));
    bench_loop_test(&loop, anvil_build_cmp_ult(ctx, i, n, "more"));
    anvil_value_t *c = anvil_build_zext(ctx, anvil_build_load(ctx, b->u8, bench_elem(b, b->u8, s, i), "byte"), b->u32, "c");

    enum { CLASSES = 7 };
    anvil_block_t *classes[CLASSES], *merge = anvil_block_create(fn, "classified");
    for (int k = 0; k < CLASSES; k++) {
        char name[16];
        snprintf(name, sizeof(name), "class%d", k + 1);
        classes[k] = anvil_block_create(fn, name);
    }
    anvil_instr_t *dispatch = anvil_build_switch(ctx, c, classes[CLASSES - 1]);
    if (!dispatch)
        return false;
    static const char *const sets[] = { "0123456789", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", " \t\n", "+-*/=", "(){};" };
    for (int k = 0; k < CLASSES - 1; k++)
        for (const char *ch = sets[k]; *ch; ch++)
            if (!anvil_switch_add_case(dispatch, anvil_const_u32(ctx, (uint32_t)(unsigned char)*ch), classes[k]))
                return false;
    for (int k = 0; k < CLASSES; k++) {
        anvil_set_insert_point(ctx, classes[k]);
        anvil_build_br(ctx, merge);
    }
    anvil_set_insert_point(ctx, merge);
    anvil_value_t *class = anvil_build_phi(ctx, b->u32, "k");
    for (int k = 0; k < CLASSES; k++)
        anvil_phi_add_incoming(class, anvil_const_u32(ctx, (uint32_t)(k + 1)), classes[k]);
    anvil_value_t *mixed = anvil_build_add(ctx, anvil_build_mul(ctx, h, anvil_const_u32(ctx, 31), "h31"), class, "h1");
    anvil_value_t *next[] = { anvil_build_add(ctx, i, bench_index_const(b, b->uindex, 1), "i1"), mixed };
    bench_loop_close(&loop, next);
    return anvil_build_ret(ctx, h);
}
