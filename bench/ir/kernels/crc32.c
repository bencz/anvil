/* void kcrc_table(uint32_t *t):
 *     for (i = 0; i < 256; i++) { c = i; for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >> 1 : c >> 1; t[i] = c; }
 * uint32_t kcrc32(uint32_t crc, const uint8_t *buf, size_t len, const uint32_t *t):
 *     for (i = 0; i < len; i++) crc = t[(crc ^ buf[i]) & 255] ^ crc >> 8;  return crc; */
#include "../kit.h"

static bool build_table(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->u32) };
    anvil_func_t *fn = bench_func(b, "kcrc_table", NULL, params, 1);
    if (!fn)
        return false;
    anvil_value_t *table = bench_param(fn, 0);

    bench_loop_t outer, inner;
    bench_loop_open(b, &outer, fn, "entries");
    anvil_value_t *i = bench_loop_carry(&outer, b->u32, anvil_const_u32(ctx, 0));
    bench_loop_test(&outer, anvil_build_cmp_ult(ctx, i, anvil_const_u32(ctx, 256), "more"));

    bench_loop_open(b, &inner, fn, "bits");
    anvil_value_t *k = bench_loop_carry(&inner, b->i32, anvil_const_i32(ctx, 0));
    anvil_value_t *c = bench_loop_carry(&inner, b->u32, i);
    bench_loop_test(&inner, anvil_build_cmp_lt(ctx, k, anvil_const_i32(ctx, 8), "more"));
    anvil_value_t *odd = anvil_build_cmp_ne(ctx, anvil_build_and(ctx, c, anvil_const_u32(ctx, 1), "low"), anvil_const_u32(ctx, 0), "odd");
    anvil_value_t *half = anvil_build_shr(ctx, c, anvil_const_u32(ctx, 1), "half");
    anvil_value_t *mixed = anvil_build_xor(ctx, half, anvil_const_u32(ctx, 0xEDB88320u), "mixed");
    anvil_value_t *next_inner[] = { bench_nw(anvil_build_add(ctx, k, anvil_const_i32(ctx, 1), "k1")), anvil_build_select(ctx, odd, mixed, half, "c1") };
    bench_loop_close(&inner, next_inner);

    anvil_build_store(ctx, c, bench_elem(b, b->u32, table, bench_to_index(b, i, b->uindex, false)));
    anvil_value_t *next_outer[] = { anvil_build_add(ctx, i, anvil_const_u32(ctx, 1), "i1") };
    bench_loop_close(&outer, next_outer);
    return anvil_build_ret_void(ctx);
}

static bool build_crc(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { b->u32, anvil_type_ptr(ctx, b->u8), b->uindex, anvil_type_ptr(ctx, b->u32) };
    anvil_func_t *fn = bench_func(b, "kcrc32", b->u32, params, 4);
    if (!fn)
        return false;
    anvil_value_t *buf = bench_param(fn, 1), *len = bench_param(fn, 2), *table = bench_param(fn, 3);

    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "bytes");
    anvil_value_t *i = bench_loop_carry(&loop, b->uindex, bench_index_const(b, b->uindex, 0));
    anvil_value_t *crc = bench_loop_carry(&loop, b->u32, bench_param(fn, 0));
    bench_loop_test(&loop, anvil_build_cmp_ult(ctx, i, len, "more"));
    anvil_value_t *byte = anvil_build_zext(ctx, anvil_build_load(ctx, b->u8, bench_elem(b, b->u8, buf, i), "byte"), b->u32, "wide");
    anvil_value_t *slot = anvil_build_and(ctx, anvil_build_xor(ctx, crc, byte, "x"), anvil_const_u32(ctx, 255), "low");
    anvil_value_t *entry = anvil_build_load(ctx, b->u32, bench_elem(b, b->u32, table, bench_to_index(b, slot, b->uindex, false)), "entry");
    anvil_value_t *next[] = { anvil_build_add(ctx, i, bench_index_const(b, b->uindex, 1), "i1"),
                              anvil_build_xor(ctx, entry, anvil_build_shr(ctx, crc, anvil_const_u32(ctx, 8), "shifted"), "crc1") };
    bench_loop_close(&loop, next);
    return anvil_build_ret(ctx, crc);
}

bool bench_build_crc32(bench_t *b)
{
    return build_table(b) && build_crc(b);
}
