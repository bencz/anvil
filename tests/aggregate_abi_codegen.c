/* Generates functions that pass and return structs and unions by value
 * (BYVAL / SRET parameters) for one target; tests/aggregate_abi_shim.c,
 * compiled by the platform C compiler, calls them and is called by them.
 * Integers are pointer-sized so the same harness serves every target. */
#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *message, anvil_ctx_t *ctx)
{
    fprintf(stderr, "aggregate ABI generator: %s%s%s\n", message,
            ctx ? ": " : "", ctx ? anvil_ctx_get_error(ctx) : "");
    return 1;
}

typedef struct {
    anvil_ctx_t *ctx;
    anvil_module_t *mod;
    anvil_type_t *word;     /* pointer-sized integer (C long) */
    anvil_type_t *f32, *f64;
} gen_t;

static anvil_value_t *word_const(gen_t *g, long value)
{
    return anvil_type_size(g->word) == 4 ? anvil_const_i32(g->ctx, (int32_t)value)
                                         : anvil_const_i64(g->ctx, value);
}

static anvil_type_t *record(gen_t *g, const char *name, anvil_type_t **fields,
                            size_t count, bool is_union)
{
    anvil_type_t *type = anvil_type_named_struct(g->ctx, name);
    bool ok = type && (is_union ? anvil_type_union_set_body(type, fields, count)
                                : anvil_type_struct_set_body(type, fields, count, false));
    return ok ? type : NULL;
}

static anvil_func_t *function(gen_t *g, const char *name, anvil_type_t *ret,
                              anvil_type_t **params, const anvil_param_attr_t *attrs,
                              size_t count, bool define)
{
    anvil_type_t *type = anvil_type_func_attrs(g->ctx, ret, params, attrs, count,
                                               false, ANVIL_CC_DEFAULT);
    anvil_func_t *func = !type ? NULL : define
        ? anvil_func_create(g->mod, name, type, ANVIL_LINK_EXTERNAL)
        : anvil_func_declare(g->mod, name, type);
    if (func && define)
        anvil_set_insert_point(g->ctx, anvil_func_get_entry(func));
    return func;
}

static anvil_value_t *field(gen_t *g, anvil_type_t *rec, anvil_value_t *ptr,
                            unsigned index, anvil_type_t *type)
{
    anvil_value_t *address = anvil_build_struct_gep(g->ctx, rec, ptr, index, "field");
    return address ? anvil_build_load(g->ctx, type, address, "value") : NULL;
}

static bool set_field(gen_t *g, anvil_type_t *rec, anvil_value_t *ptr,
                      unsigned index, anvil_value_t *value)
{
    anvil_value_t *address = anvil_build_struct_gep(g->ctx, rec, ptr, index, "field");
    return address && anvil_build_store(g->ctx, value, address);
}

int main(int argc, char **argv)
{
    if (argc != 3) return fail("usage: aggregate_abi_codegen <target> <out.s>", NULL);
    static const struct { const char *name; anvil_arch_t arch; } targets[] = {
        { "x86_64", ANVIL_ARCH_X86_64 }, { "i386", ANVIL_ARCH_X86 },
        { "aarch64", ANVIL_ARCH_ARM64 }, { "ppc32", ANVIL_ARCH_PPC32 },
        { "ppc64", ANVIL_ARCH_PPC64 }, { "ppc64le", ANVIL_ARCH_PPC64LE }
    };
    anvil_arch_t arch = ANVIL_ARCH_COUNT;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (strcmp(argv[1], targets[i].name) == 0) arch = targets[i].arch;
    if (arch == ANVIL_ARCH_COUNT) return fail("unknown target", NULL);

    gen_t g = { anvil_ctx_create_for_target(arch), NULL, NULL, NULL, NULL };
    if (!g.ctx) return fail("cannot create context", NULL);
    if (!anvil_ctx_supports_param_attr(g.ctx, ANVIL_PARAM_BYVAL) ||
        !anvil_ctx_supports_param_attr(g.ctx, ANVIL_PARAM_SRET)) {
        printf("aggregate ABI: target %s does not lower BYVAL/SRET yet\n", argv[1]);
        return 2;
    }
    g.mod = anvil_module_create(g.ctx, "aggregate_abi");
    const anvil_arch_info_t *info = anvil_ctx_get_arch_info(g.ctx);
    g.word = info && info->ptr_size == 4 ? anvil_type_i32(g.ctx) : anvil_type_i64(g.ctx);
    g.f32 = anvil_type_f32(g.ctx);
    g.f64 = anvil_type_f64(g.ctx);
    anvil_type_t *i8 = anvil_type_i8(g.ctx);

    /* struct pair { long a; double b; }  struct small { char a, b, c; }
     * struct big { long a, b, c; }        struct hfa { float x, y, z; }
     * (small holds signed chars: plain char is unsigned on some targets)
     * union num { float f[2]; double d; } */
    anvil_type_t *pair_f[] = { g.word, g.f64 };
    anvil_type_t *small_f[] = { i8, i8, i8 };
    anvil_type_t *big_f[] = { g.word, g.word, g.word };
    anvil_type_t *hfa_f[] = { g.f32, g.f32, g.f32 };
    anvil_type_t *num_f[] = { anvil_type_array(g.ctx, g.f32, 2), g.f64 };
    anvil_type_t *pair = record(&g, "pair", pair_f, 2, false);
    anvil_type_t *small = record(&g, "small", small_f, 3, false);
    anvil_type_t *big = record(&g, "big", big_f, 3, false);
    anvil_type_t *hfa = record(&g, "hfa", hfa_f, 3, false);
    anvil_type_t *num = record(&g, "num", num_f, 2, true);
    if (!pair || !small || !big || !hfa || !num) return fail("cannot build records", g.ctx);
    anvil_type_t *pair_p = anvil_type_ptr(g.ctx, pair), *small_p = anvil_type_ptr(g.ctx, small);
    anvil_type_t *big_p = anvil_type_ptr(g.ctx, big), *hfa_p = anvil_type_ptr(g.ctx, hfa);
    anvil_type_t *num_p = anvil_type_ptr(g.ctx, num);
    anvil_type_t *void_t = anvil_type_void(g.ctx);
    const anvil_param_attr_t BV = ANVIL_PARAM_BYVAL, SR = ANVIL_PARAM_SRET, PL = ANVIL_PARAM_PLAIN;

    /* double anvil_sum_pair(struct pair p) { return p.a + p.b; } */
    {
        anvil_type_t *ps[] = { pair_p }; anvil_param_attr_t as[] = { BV };
        anvil_func_t *f = function(&g, "anvil_sum_pair", g.f64, ps, as, 1, true);
        anvil_value_t *p = f ? anvil_func_get_param(f, 0) : NULL;
        anvil_value_t *a = p ? field(&g, pair, p, 0, g.word) : NULL;
        anvil_value_t *b = p ? field(&g, pair, p, 1, g.f64) : NULL;
        if (!a || !b || !anvil_build_ret(g.ctx, anvil_build_fadd(g.ctx,
                anvil_build_sitofp(g.ctx, a, g.f64, "af"), b, "sum")))
            return fail("anvil_sum_pair", g.ctx);
    }
    /* struct pair anvil_make_pair(long seed) { return (struct pair){seed, seed * 0.5}; } */
    {
        anvil_type_t *ps[] = { pair_p, g.word }; anvil_param_attr_t as[] = { SR, PL };
        anvil_func_t *f = function(&g, "anvil_make_pair", void_t, ps, as, 2, true);
        anvil_value_t *r = f ? anvil_func_get_param(f, 0) : NULL;
        anvil_value_t *seed = f ? anvil_func_get_param(f, 1) : NULL;
        anvil_value_t *half = seed ? anvil_build_fmul(g.ctx, anvil_build_sitofp(g.ctx, seed, g.f64, "sf"),
                                                      anvil_const_f64(g.ctx, 0.5), "half") : NULL;
        if (!half || !set_field(&g, pair, r, 0, seed) || !set_field(&g, pair, r, 1, half) ||
            !anvil_build_ret_void(g.ctx))
            return fail("anvil_make_pair", g.ctx);
    }
    /* long anvil_press_small(long, long, long, long, long, struct small s, long t)
     * { return x1 + 2*x5 + s.a*100 + s.b*1000 + s.c*10000 + 7*t; } */
    {
        anvil_type_t *ps[] = { g.word, g.word, g.word, g.word, g.word, small_p, g.word };
        anvil_param_attr_t as[] = { PL, PL, PL, PL, PL, BV, PL };
        anvil_func_t *f = function(&g, "anvil_press_small", g.word, ps, as, 7, true);
        if (!f) return fail("anvil_press_small", g.ctx);
        anvil_value_t *s = anvil_func_get_param(f, 5);
        anvil_value_t *acc = anvil_build_add(g.ctx, anvil_func_get_param(f, 0),
            anvil_build_mul(g.ctx, anvil_func_get_param(f, 4), word_const(&g, 2), "x5"), "acc");
        static const long weights[] = { 100, 1000, 10000 };
        for (unsigned i = 0; i < 3; i++) {
            anvil_value_t *byte = field(&g, small, s, i, i8);
            anvil_value_t *wide = anvil_build_sext(g.ctx, byte, g.word, "wide");
            acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx, wide,
                    word_const(&g, weights[i]), "w"), "acc");
        }
        acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx, anvil_func_get_param(f, 6),
                word_const(&g, 7), "t"), "acc");
        if (!anvil_build_ret(g.ctx, acc)) return fail("anvil_press_small", g.ctx);
    }
    /* long c_sum_big(struct big b);  struct big c_make_big(long seed);
     * long anvil_big_roundtrip(struct big b, long seed)
     * { return c_sum_big(b) * 3 + c_sum_big(c_make_big(seed)); } */
    {
        anvil_type_t *sum_ps[] = { big_p }; anvil_param_attr_t sum_as[] = { BV };
        anvil_func_t *c_sum = function(&g, "c_sum_big", g.word, sum_ps, sum_as, 1, false);
        anvil_type_t *make_ps[] = { big_p, g.word }; anvil_param_attr_t make_as[] = { SR, PL };
        anvil_func_t *c_make = function(&g, "c_make_big", void_t, make_ps, make_as, 2, false);
        anvil_type_t *ps[] = { big_p, g.word }; anvil_param_attr_t as[] = { BV, PL };
        anvil_func_t *f = function(&g, "anvil_big_roundtrip", g.word, ps, as, 2, true);
        if (!c_sum || !c_make || !f) return fail("anvil_big_roundtrip", g.ctx);
        anvil_value_t *made = anvil_build_alloca(g.ctx, big, "made");
        anvil_value_t *copy = anvil_build_alloca(g.ctx, big, "copy");
        anvil_value_t *b = anvil_func_get_param(f, 0);
        for (unsigned i = 0; i < 3; i++)
            if (!set_field(&g, big, copy, i, field(&g, big, b, i, g.word)))
                return fail("anvil_big_roundtrip copy", g.ctx);
        anvil_value_t *make_args[] = { made, anvil_func_get_param(f, 1) };
        anvil_value_t *first = NULL, *second = NULL, *ignored = NULL;
        anvil_value_t *sum_copy[] = { copy }, *sum_made[] = { made };
        if (!anvil_build_call_checked(g.ctx, anvil_func_get_value(c_make), make_args, 2, "", &ignored) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_sum), sum_copy, 1, "a", &first) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_sum), sum_made, 1, "b", &second) ||
            !anvil_build_ret(g.ctx, anvil_build_add(g.ctx, anvil_build_mul(g.ctx, first,
                word_const(&g, 3), "x3"), second, "sum")))
            return fail("anvil_big_roundtrip", g.ctx);
    }
    /* struct hfa anvil_make_hfa(float v) { return (struct hfa){v, v*2, v*3}; } */
    {
        anvil_type_t *ps[] = { hfa_p, g.f32 }; anvil_param_attr_t as[] = { SR, PL };
        anvil_func_t *f = function(&g, "anvil_make_hfa", void_t, ps, as, 2, true);
        if (!f) return fail("anvil_make_hfa", g.ctx);
        anvil_value_t *r = anvil_func_get_param(f, 0), *v = anvil_func_get_param(f, 1);
        for (unsigned i = 0; i < 3; i++)
            if (!set_field(&g, hfa, r, i, anvil_build_fmul(g.ctx, v, anvil_const_f32(g.ctx, (float)(i + 1)), "m")))
                return fail("anvil_make_hfa", g.ctx);
        if (!anvil_build_ret_void(g.ctx)) return fail("anvil_make_hfa", g.ctx);
    }
    /* double anvil_hfa_dot(struct hfa a, struct hfa b) */
    {
        anvil_type_t *ps[] = { hfa_p, hfa_p }; anvil_param_attr_t as[] = { BV, BV };
        anvil_func_t *f = function(&g, "anvil_hfa_dot", g.f64, ps, as, 2, true);
        if (!f) return fail("anvil_hfa_dot", g.ctx);
        anvil_value_t *acc = anvil_const_f32(g.ctx, 0.0f);
        for (unsigned i = 0; i < 3; i++)
            acc = anvil_build_fadd(g.ctx, acc, anvil_build_fmul(g.ctx,
                field(&g, hfa, anvil_func_get_param(f, 0), i, g.f32),
                field(&g, hfa, anvil_func_get_param(f, 1), i, g.f32), "p"), "acc");
        if (!anvil_build_ret(g.ctx, anvil_build_fpext(g.ctx, acc, g.f64, "wide")))
            return fail("anvil_hfa_dot", g.ctx);
    }
    /* double anvil_union_double(union num u) { return u.d; }
     * union num anvil_make_union(float a, float b) { union num u; u.f[0]=a; u.f[1]=b; return u; } */
    {
        anvil_type_t *ps[] = { num_p }; anvil_param_attr_t as[] = { BV };
        anvil_func_t *f = function(&g, "anvil_union_double", g.f64, ps, as, 1, true);
        if (!f || !anvil_build_ret(g.ctx, field(&g, num, anvil_func_get_param(f, 0), 1, g.f64)))
            return fail("anvil_union_double", g.ctx);
        anvil_type_t *mps[] = { num_p, g.f32, g.f32 }; anvil_param_attr_t mas[] = { SR, PL, PL };
        anvil_func_t *m = function(&g, "anvil_make_union", void_t, mps, mas, 3, true);
        if (!m) return fail("anvil_make_union", g.ctx);
        anvil_value_t *floats = anvil_build_struct_gep(g.ctx, num, anvil_func_get_param(m, 0), 0, "f");
        for (unsigned i = 0; i < 2; i++) {
            anvil_value_t *index[] = { anvil_const_i64(g.ctx, 0), anvil_const_i64(g.ctx, i) };
            anvil_value_t *slot = anvil_build_gep(g.ctx, num_f[0], floats, index, 2, "slot");
            if (!slot || !anvil_build_store(g.ctx, anvil_func_get_param(m, 1 + i), slot))
                return fail("anvil_make_union", g.ctx);
        }
        if (!anvil_build_ret_void(g.ctx)) return fail("anvil_make_union", g.ctx);
    }

    /* long anvil_mixed(double d, long a, float f, long b)
     * { return a + 100*b + 10000*(long)d + 1000000*(long)f; }
     * Floating arguments take parameter slots of their own on some ABIs. */
    anvil_type_t *mixed_ps[] = { g.f64, g.word, g.f32, g.word };
    {
        anvil_func_t *f = function(&g, "anvil_mixed", g.word, mixed_ps, NULL, 4, true);
        if (!f) return fail("anvil_mixed", g.ctx);
        anvil_value_t *acc = anvil_build_add(g.ctx, anvil_func_get_param(f, 1),
            anvil_build_mul(g.ctx, anvil_func_get_param(f, 3), word_const(&g, 100), "b"), "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx,
            anvil_build_fptosi(g.ctx, anvil_func_get_param(f, 0), g.word, "d"), word_const(&g, 10000), "dm"), "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx,
            anvil_build_fptosi(g.ctx, anvil_func_get_param(f, 2), g.word, "f"), word_const(&g, 1000000), "fm"), "acc");
        if (!anvil_build_ret(g.ctx, acc)) return fail("anvil_mixed", g.ctx);
    }
    /* long anvil_spill(long a1, ..., long a9, double d, struct small s)
     * { return a1 + 10*a8 + 100*a9 + 1000*(long)d + s.a + 7*s.b + 13*s.c; }
     * a9, and on most ABIs s, arrive on the stack. */
    anvil_type_t *spill_ps[11];
    anvil_param_attr_t spill_as[11] = { 0 };
    for (unsigned i = 0; i < 9; i++) spill_ps[i] = g.word;
    spill_ps[9] = g.f64;
    spill_ps[10] = small_p;
    spill_as[10] = BV;
    {
        anvil_func_t *f = function(&g, "anvil_spill", g.word, spill_ps, spill_as, 11, true);
        if (!f) return fail("anvil_spill", g.ctx);
        anvil_value_t *acc = anvil_build_add(g.ctx, anvil_func_get_param(f, 0),
            anvil_build_mul(g.ctx, anvil_func_get_param(f, 7), word_const(&g, 10), "a8"), "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx, anvil_func_get_param(f, 8), word_const(&g, 100), "a9"), "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx,
            anvil_build_fptosi(g.ctx, anvil_func_get_param(f, 9), g.word, "d"), word_const(&g, 1000), "dm"), "acc");
        static const long weights[] = { 1, 7, 13 };
        for (unsigned i = 0; i < 3; i++)
            acc = anvil_build_add(g.ctx, acc, anvil_build_mul(g.ctx,
                anvil_build_sext(g.ctx, field(&g, small, anvil_func_get_param(f, 10), i, i8), g.word, "w"),
                word_const(&g, weights[i]), "m"), "acc");
        if (!anvil_build_ret(g.ctx, acc)) return fail("anvil_spill", g.ctx);
    }
    /* long anvil_call_c(void): the same shapes called from ANVIL code.
     * { struct small s = {1, -2, 3}; struct hfa h = c_make_hfa(2.0f);
     *   struct pair p = c_make_pair(5);
     *   return c_mixed(2.0, 3, 5.0f, 7) + c_spill(1, ..., 9, 4.0, s)
     *        + (long)c_hfa_sum(h) + p.a + (long)(p.b * 2.0); } */
    {
        anvil_func_t *c_mixed = function(&g, "c_mixed", g.word, mixed_ps, NULL, 4, false);
        anvil_func_t *c_spill = function(&g, "c_spill", g.word, spill_ps, spill_as, 11, false);
        anvil_type_t *mh_ps[] = { hfa_p, g.f32 }; anvil_param_attr_t mh_as[] = { SR, PL };
        anvil_func_t *c_make_hfa = function(&g, "c_make_hfa", void_t, mh_ps, mh_as, 2, false);
        anvil_type_t *hs_ps[] = { hfa_p }; anvil_param_attr_t hs_as[] = { BV };
        anvil_func_t *c_hfa_sum = function(&g, "c_hfa_sum", g.f64, hs_ps, hs_as, 1, false);
        anvil_type_t *mp_ps[] = { pair_p, g.word }; anvil_param_attr_t mp_as[] = { SR, PL };
        anvil_func_t *c_make_pair = function(&g, "c_make_pair", void_t, mp_ps, mp_as, 2, false);
        anvil_func_t *f = function(&g, "anvil_call_c", g.word, NULL, NULL, 0, true);
        if (!c_mixed || !c_spill || !c_make_hfa || !c_hfa_sum || !c_make_pair || !f)
            return fail("anvil_call_c", g.ctx);
        anvil_value_t *s = anvil_build_alloca(g.ctx, small, "s");
        anvil_value_t *h = anvil_build_alloca(g.ctx, hfa, "h");
        anvil_value_t *p = anvil_build_alloca(g.ctx, pair, "p");
        static const int8_t bytes[] = { 1, -2, 3 };
        for (unsigned i = 0; i < 3; i++)
            if (!set_field(&g, small, s, i, anvil_const_i8(g.ctx, bytes[i])))
                return fail("anvil_call_c small", g.ctx);
        anvil_value_t *mixed_args[] = { anvil_const_f64(g.ctx, 2.0), word_const(&g, 3),
                                        anvil_const_f32(g.ctx, 5.0f), word_const(&g, 7) };
        anvil_value_t *spill_args[11];
        for (unsigned i = 0; i < 9; i++) spill_args[i] = word_const(&g, i + 1);
        spill_args[9] = anvil_const_f64(g.ctx, 4.0);
        spill_args[10] = s;
        anvil_value_t *mh_args[] = { h, anvil_const_f32(g.ctx, 2.0f) };
        anvil_value_t *hs_args[] = { h };
        anvil_value_t *mp_args[] = { p, word_const(&g, 5) };
        anvil_value_t *r_mixed = NULL, *r_spill = NULL, *r_hsum = NULL, *ignored = NULL;
        if (!anvil_build_call_checked(g.ctx, anvil_func_get_value(c_mixed), mixed_args, 4, "m", &r_mixed) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_spill), spill_args, 11, "s", &r_spill) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_make_hfa), mh_args, 2, "", &ignored) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_hfa_sum), hs_args, 1, "h", &r_hsum) ||
            !anvil_build_call_checked(g.ctx, anvil_func_get_value(c_make_pair), mp_args, 2, "", &ignored))
            return fail("anvil_call_c calls", g.ctx);
        anvil_value_t *acc = anvil_build_add(g.ctx, r_mixed, r_spill, "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_fptosi(g.ctx, r_hsum, g.word, "hs"), "acc");
        acc = anvil_build_add(g.ctx, acc, field(&g, pair, p, 0, g.word), "acc");
        acc = anvil_build_add(g.ctx, acc, anvil_build_fptosi(g.ctx, anvil_build_fmul(g.ctx,
            field(&g, pair, p, 1, g.f64), anvil_const_f64(g.ctx, 2.0), "b2"), g.word, "pb"), "acc");
        if (!anvil_build_ret(g.ctx, acc)) return fail("anvil_call_c", g.ctx);
    }

    char *output = NULL;
    size_t length = 0;
    if (anvil_module_codegen(g.mod, &output, &length) != ANVIL_OK)
        return fail("codegen", g.ctx);
    FILE *out = fopen(argv[2], "w");
    if (!out || fwrite(output, 1, length, out) != length || fclose(out) != 0)
        return fail("cannot write output", NULL);
    free(output);
    anvil_module_destroy(g.mod);
    anvil_ctx_destroy(g.ctx);
    return 0;
}
