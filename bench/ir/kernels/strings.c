/* int32_t kstrlen(const char *s);           while (s[i]) i++;  return i
 * int32_t kstrcmp(const char *a, const char *b);   while (a[i] && a[i] == b[i]) i++;  return (uint8_t)a[i] - (uint8_t)b[i]
 * void kstrcpy(char *d, const char *s);     do { c = s[i]; d[i] = c; i++; } while (c)
 * void kreverse(char *s, int32_t n);        for (i = 0, j = n - 1; i < j; i++, j--) swap s[i], s[j] */
#include "../kit.h"

static bool build_strlen(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->i8) };
    anvil_func_t *fn = bench_func(b, "kstrlen", b->i32, params, 1);
    if (!fn)
        return false;
    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "chars");
    anvil_value_t *i = bench_loop_carry(&loop, b->index, bench_index_const(b, b->index, 0));
    anvil_value_t *c = anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, bench_param(fn, 0), i), "c");
    bench_loop_test(&loop, anvil_build_cmp_ne(ctx, c, anvil_const_i8(ctx, 0), "more"));
    anvil_value_t *next[] = { bench_nw(anvil_build_add(ctx, i, bench_index_const(b, b->index, 1), "i1")) };
    bench_loop_close(&loop, next);
    return anvil_build_ret(ctx, b->index == b->i32 ? i : anvil_build_trunc(ctx, i, b->i32, "len"));
}

static bool build_strcmp(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *pchar = anvil_type_ptr(ctx, b->i8);
    anvil_type_t *params[] = { pchar, pchar };
    anvil_func_t *fn = bench_func(b, "kstrcmp", b->i32, params, 2);
    if (!fn)
        return false;
    anvil_value_t *s = bench_param(fn, 0), *t = bench_param(fn, 1);
    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "chars");
    anvil_value_t *i = bench_loop_carry(&loop, b->index, bench_index_const(b, b->index, 0));
    anvil_value_t *c = anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, s, i), "c");
    bench_loop_test(&loop, anvil_build_cmp_ne(ctx, c, anvil_const_i8(ctx, 0), "more"));
    anvil_block_t *same = anvil_block_create(fn, "same");
    anvil_value_t *d = anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, t, i), "d");
    anvil_build_br_cond(ctx, anvil_build_cmp_eq(ctx, c, d, "equal"), same, loop.exit);
    anvil_set_insert_point(ctx, same);
    anvil_value_t *next[] = { bench_nw(anvil_build_add(ctx, i, bench_index_const(b, b->index, 1), "i1")) };
    bench_loop_close(&loop, next);
    /* As unsigned char: zero-extended. */
    anvil_value_t *x = anvil_build_zext(ctx, anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, s, i), "x"), b->i32, "xw");
    anvil_value_t *y = anvil_build_zext(ctx, anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, t, i), "y"), b->i32, "yw");
    return anvil_build_ret(ctx, bench_nw(anvil_build_sub(ctx, x, y, "diff")));
}

static bool build_strcpy(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *pchar = anvil_type_ptr(ctx, b->i8);
    anvil_type_t *params[] = { pchar, pchar };
    anvil_func_t *fn = bench_func(b, "kstrcpy", NULL, params, 2);
    if (!fn)
        return false;
    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "chars");
    anvil_value_t *i = bench_loop_carry(&loop, b->index, bench_index_const(b, b->index, 0));
    anvil_value_t *c = anvil_build_load(ctx, b->i8, bench_elem(b, b->i8, bench_param(fn, 1), i), "c");
    anvil_build_store(ctx, c, bench_elem(b, b->i8, bench_param(fn, 0), i));
    bench_loop_test(&loop, anvil_build_cmp_ne(ctx, c, anvil_const_i8(ctx, 0), "more"));
    anvil_value_t *next[] = { bench_nw(anvil_build_add(ctx, i, bench_index_const(b, b->index, 1), "i1")) };
    bench_loop_close(&loop, next);
    return anvil_build_ret_void(ctx);
}

static bool build_reverse(bench_t *b)
{
    anvil_ctx_t *ctx = b->ctx;
    anvil_type_t *params[] = { anvil_type_ptr(ctx, b->i8), b->i32 };
    anvil_func_t *fn = bench_func(b, "kreverse", NULL, params, 2);
    if (!fn)
        return false;
    anvil_value_t *s = bench_param(fn, 0);
    anvil_value_t *last = bench_nw(anvil_build_sub(ctx, bench_param(fn, 1), anvil_const_i32(ctx, 1), "last"));
    bench_loop_t loop;
    bench_loop_open(b, &loop, fn, "pairs");
    anvil_value_t *i = bench_loop_carry(&loop, b->i32, anvil_const_i32(ctx, 0));
    anvil_value_t *j = bench_loop_carry(&loop, b->i32, last);
    bench_loop_test(&loop, anvil_build_cmp_lt(ctx, i, j, "more"));
    anvil_value_t *pi = bench_elem(b, b->i8, s, bench_to_index(b, i, b->index, true));
    anvil_value_t *pj = bench_elem(b, b->i8, s, bench_to_index(b, j, b->index, true));
    anvil_value_t *x = anvil_build_load(ctx, b->i8, pi, "x"), *y = anvil_build_load(ctx, b->i8, pj, "y");
    anvil_build_store(ctx, y, pi);
    anvil_build_store(ctx, x, pj);
    anvil_value_t *next[] = { bench_nw(anvil_build_add(ctx, i, anvil_const_i32(ctx, 1), "i1")), bench_nw(anvil_build_sub(ctx, j, anvil_const_i32(ctx, 1), "j1")) };
    bench_loop_close(&loop, next);
    return anvil_build_ret_void(ctx);
}

bool bench_build_strings(bench_t *b)
{
    return build_strlen(b) && build_strcmp(b) && build_strcpy(b) && build_reverse(b);
}
