/* uint32_t khex(const uint8_t *s, size_t n, uint8_t *out):
 *     h = 0; for (i = 0; i < n; i++) { int b = s[i]; int hi = b / 16, lo = b % 16;
 *     out[2i] = hi + (hi > 9 ? 'a' - 10 : '0'); out[2i + 1] = lo + (lo > 9 ? 'a' - 10 : '0');
 *     h = h * 31 + (hi ^ (b >> 2)); }  return h;
 * Hex encoding as C writes it: a byte promoted to int, divided, reduced and
 * shifted as a signed value whose sign bit is known zero. */
#include "../kit.h"

static anvil_value_t *digit(bench_t *b, anvil_value_t *value, const char *name)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_value_t *letter = anvil_build_cmp_gt(ctx, value, anvil_const_i32(ctx, 9), "letter");
    anvil_value_t *base = anvil_build_select(ctx, letter, anvil_const_i32(ctx, 'a' - 10), anvil_const_i32(ctx, '0'), "base");
    return anvil_build_trunc(ctx, anvil_build_add(ctx, value, base, "char"), b->u8, name);
}

bool bench_build_hex(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *pbyte = anvil_type_ptr(ctx, b->u8);
    anvil_type_t *params[] = { pbyte, b->uindex, pbyte };
    anvil_func_t *fn = bench_func(b, "khex", b->u32, params, 3);
    if (!fn)
        return false;
    anvil_value_t *s = bench_param(fn, 0), *n = bench_param(fn, 1), *out = bench_param(fn, 2);

    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "bytes");
    anvil_value_t *i = bench_loop_carry(&loop, b->uindex, bench_index_const(b, b->uindex, 0));
    anvil_value_t *h = bench_loop_carry(&loop, b->u32, anvil_const_u32(ctx, 0));
    bench_loop_test(&loop, anvil_build_cmp_ult(ctx, i, n, "more"));
    anvil_value_t *byte = anvil_build_zext(ctx, anvil_build_load(ctx, b->u8, bench_elem(b, b->u8, s, i), "byte"), b->i32, "b");
    anvil_value_t *high = anvil_build_sdiv(ctx, byte, anvil_const_i32(ctx, 16), "hi");
    anvil_value_t *low = anvil_build_smod(ctx, byte, anvil_const_i32(ctx, 16), "lo");
    anvil_value_t *at = bench_nw(anvil_build_add(ctx, i, i, "at"));
    anvil_build_store(ctx, digit(b, high, "dh"), bench_elem(b, b->u8, out, at));
    anvil_value_t *next_at = bench_nw(anvil_build_add(ctx, at, bench_index_const(b, b->uindex, 1), "at1"));
    anvil_build_store(ctx, digit(b, low, "dl"), bench_elem(b, b->u8, out, next_at));
    anvil_value_t *quarter = anvil_build_sar(ctx, byte, anvil_const_i32(ctx, 2), "quarter");
    anvil_value_t *mix = anvil_build_bitcast(ctx, anvil_build_xor(ctx, high, quarter, "x"), b->u32, "mix");
    anvil_value_t *mixed = anvil_build_add(ctx, anvil_build_mul(ctx, h, anvil_const_u32(ctx, 31), "h31"), mix, "h1");
    anvil_value_t *next[] = { anvil_build_add(ctx, i, bench_index_const(b, b->uindex, 1), "i1"), mixed };
    bench_loop_close(&loop, next);
    return anvil_build_ret(ctx, h);
}
