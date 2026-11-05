/* The no-wrap flag on integer add/sub/mul: API and verifier, preservation
 * and intersection, the extension distribution it enables, and the loop
 * address split that exposes base[i] streams; on sdiv/udiv (exact): a
 * shift and a multiply by the divisor's odd part's inverse. */
#include "anvil/anvil_internal.h"
#include "anvil/anvil_debug.h"
#include "anvil/anvil_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

static anvil_func_t *new_func(anvil_ctx_t *ctx, anvil_module_t **mod, const char *name, anvil_type_t *ret, anvil_type_t **params,
                              size_t count)
{
    *mod = anvil_module_create(ctx, name);
    anvil_type_t *type = anvil_type_func(ctx, ret, params, count, false);
    return *mod && type ? anvil_func_create(*mod, name, type, ANVIL_LINK_EXTERNAL) : NULL;
}

static bool valid(anvil_func_t *func)
{
    char error[256] = {0};
    bool ok = anvil_func_verify(func, error, sizeof(error));
    if (!ok)
        fprintf(stderr, "verifier: %s\n", error);
    return ok;
}

static void test_api_verifier_and_dump(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_type_t *i32 = anvil_type_i32(ctx), *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = {i32, i32, f64};
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_func(ctx, &mod, "api", i32, params, 3);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *a = anvil_func_get_param(fn, 0), *b = anvil_func_get_param(fn, 1), *x = anvil_func_get_param(fn, 2);
    anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
    anvil_value_t *both = anvil_build_and(ctx, sum, b, "both");
    anvil_value_t *real = anvil_build_fadd(ctx, x, x, "real");
    anvil_build_ret(ctx, both);
    (void)real;

    CHECK(!anvil_is_no_wrap(sum) && anvil_set_no_wrap(sum, true) && anvil_is_no_wrap(sum), "an integer add takes the no-wrap flag");
    CHECK(!anvil_set_no_wrap(both, true) && !anvil_set_no_wrap(real, true) && !anvil_set_no_wrap(a, true),
          "and, floating add and parameters refuse the no-wrap flag");
    CHECK(valid(fn), "a flagged add verifies");

    char *text = NULL;
    size_t length = 0;
    FILE *out = open_memstream(&text, &length);
    anvil_dump_module(out, mod);
    fclose(out);
    CHECK(text && strstr(text, "add nowrap"), "the dump shows the flag");
    free(text);

    anvil_instr_t copy = {0};
    CHECK(anvil_instr_copy_attributes(&copy, sum->data.instr) && copy.no_wrap, "copying attributes keeps the flag");

    both->data.instr->no_wrap = true;
    CHECK(!valid(fn), "the verifier rejects the flag on an and");
    both->data.instr->no_wrap = false;
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_cse_intersects(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = {i32, i32};
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_func(ctx, &mod, "cse", i32, params, 2);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *a = anvil_func_get_param(fn, 0), *b = anvil_func_get_param(fn, 1);
    anvil_value_t *first = anvil_build_add(ctx, a, b, "first");
    anvil_value_t *second = anvil_build_add(ctx, a, b, "second");
    anvil_set_no_wrap(first, true);
    anvil_build_ret(ctx, anvil_build_xor(ctx, first, second, "mix"));
    CHECK(anvil_pass_cse(fn) == ANVIL_PASS_RUN_CHANGED && !anvil_is_no_wrap(first) && valid(fn),
          "CSE keeps a no-wrap guarantee only when both merged operations carry it");
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

/* ret sext(a op b) (op flagged as given) + optionally another reader. */
static void distribution_case(anvil_op_t op, bool flagged, bool second_reader, bool signed_type, bool expect)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_type_t *narrow = signed_type ? anvil_type_i32(ctx) : anvil_type_u32(ctx);
    anvil_type_t *wide = signed_type ? anvil_type_i64(ctx) : anvil_type_u64(ctx);
    anvil_type_t *params[] = {narrow, narrow};
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_func(ctx, &mod, "distribute", wide, params, 2);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *a = anvil_func_get_param(fn, 0), *b = anvil_func_get_param(fn, 1);
    anvil_value_t *inner = op == ANVIL_OP_ADD ? anvil_build_add(ctx, a, b, "inner")
                         : op == ANVIL_OP_SUB ? anvil_build_sub(ctx, a, b, "inner")
                                              : anvil_build_mul(ctx, a, anvil_const_i32(ctx, 3), "inner");
    if (op == ANVIL_OP_MUL && !signed_type)
        inner->data.instr->operands[1] = anvil_const_u32(ctx, 3);
    anvil_set_no_wrap(inner, flagged);
    /* The extension: sign for a signed type, zero for an unsigned one. */
    anvil_value_t *extended = signed_type ? anvil_build_sext(ctx, inner, wide, "ext") : anvil_build_zext(ctx, inner, wide, "ext");
    anvil_value_t *result = extended;
    if (second_reader)
        result = anvil_build_add(ctx, extended, signed_type ? anvil_build_sext(ctx, anvil_build_xor(ctx, inner, a, "other"), wide, "o")
                                                           : anvil_build_zext(ctx, anvil_build_xor(ctx, inner, a, "other"), wide, "o"),
                                 "both");
    anvil_build_ret(ctx, result);

    anvil_pass_strength_reduce(fn);
    anvil_instr_t *rewritten = extended->data.instr;
    bool distributed = rewritten->op == op && rewritten->no_wrap && rewritten->num_operands == 2 &&
                       rewritten->operands[0]->kind == ANVIL_VAL_INSTR &&
                       rewritten->operands[0]->data.instr->op == (signed_type ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT) &&
                       rewritten->operands[0]->data.instr->operands[0] == a;
    if (op == ANVIL_OP_MUL)
        distributed = distributed && rewritten->operands[1]->kind == ANVIL_VAL_CONST_INT && rewritten->operands[1]->data.i == 3 &&
                      anvil_types_equal(rewritten->operands[1]->type, wide);
    char message[160];
    snprintf(message, sizeof(message), "extension over %s %s%s%s %s distribute", op == ANVIL_OP_ADD ? "add" : op == ANVIL_OP_SUB ? "sub" : "mul",
             flagged ? "no-wrap" : "wrapping", second_reader ? " (second reader)" : "", signed_type ? "" : " unsigned",
             expect ? "must" : "must not");
    CHECK(distributed == expect && valid(fn), message);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

/* for (j = 0; j < n; j++) out[k + j] = 0 with k invariant: the address
 * splits into an invariant base and j. */
static void test_licm_splits_invariant_index(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_type_t *i64 = anvil_type_i64(ctx), *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = {anvil_type_ptr(ctx, i32), i64, i64};
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_func(ctx, &mod, "split", anvil_type_void(ctx), params, 3);
    anvil_block_t *entry = anvil_func_get_entry(fn);
    anvil_block_t *header = anvil_block_create(fn, "header"), *body = anvil_block_create(fn, "body"), *exit = anvil_block_create(fn, "exit");
    anvil_value_t *out = anvil_func_get_param(fn, 0), *k = anvil_func_get_param(fn, 1), *n = anvil_func_get_param(fn, 2);
    anvil_set_insert_point(ctx, entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *j = anvil_build_phi(ctx, i64, "j");
    anvil_build_br_cond(ctx, anvil_build_cmp_lt(ctx, j, n, "more"), body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *index = anvil_build_add(ctx, k, j, "index");
    anvil_value_t *address = anvil_build_gep(ctx, i32, out, &index, 1, "address");
    anvil_build_store(ctx, anvil_const_i32(ctx, 0), address);
    anvil_value_t *next = anvil_build_add(ctx, j, anvil_const_i64(ctx, 1), "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(j, anvil_const_i64(ctx, 0), entry);
    anvil_phi_add_incoming(j, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret_void(ctx);

    CHECK(anvil_pass_licm(fn) == ANVIL_PASS_RUN_CHANGED, "LICM splits an invariant part off a loop address");
    anvil_instr_t *gep = address->data.instr;
    anvil_value_t *base = gep->operands[0];
    CHECK(gep->parent == body && gep->num_operands == 2 && gep->operands[1] == j && base->kind == ANVIL_VAL_INSTR &&
              base->data.instr->op == ANVIL_OP_GEP && base->data.instr->parent != body && base->data.instr->parent != header &&
              base->data.instr->operands[0] == out && base->data.instr->operands[1] == k,
          "the access indexes an invariant base, computed before the loop, by j alone");
    CHECK(valid(fn), "the split keeps valid IR");
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

/* (p - q) / 12 known exact: sar 2, then multiply by 3's inverse (no
 * rounding fix, no high multiply); x / 8 exact: one arithmetic shift. */
static void test_exact_division(void)
{
    static const int64_t divisors[] = {12, 8};
    for (size_t d = 0; d < 2; d++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = {i64};
        anvil_module_t *mod = NULL;
        anvil_func_t *fn = new_func(ctx, &mod, "exact", i64, params, 1);
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *q = anvil_build_sdiv(ctx, anvil_func_get_param(fn, 0), anvil_const_i64(ctx, divisors[d]), "q");
        anvil_build_ret(ctx, q);
        CHECK(anvil_set_no_wrap(q, true) && anvil_is_no_wrap(q), "an sdiv takes the exact (no-wrap) flag");
        CHECK(valid(fn), "an exact sdiv verifies");
        anvil_pass_strength_reduce(fn);
        size_t sar = 0, mul = 0, other = 0;
        int64_t factor = 0;
        for (anvil_instr_t *instr = anvil_func_get_entry(fn)->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_SAR)
                sar++;
            else if (instr->op == ANVIL_OP_MUL) {
                mul++;
                factor = instr->operands[1]->kind == ANVIL_VAL_CONST_INT ? instr->operands[1]->data.i : 0;
            } else if (instr->op != ANVIL_OP_RET)
                other++;
        }
        char message[128];
        snprintf(message, sizeof(message), "exact x / %lld: a shift%s only", (long long)divisors[d], d == 0 ? " and a multiply by 3^-1" : "");
        CHECK(valid(fn) && sar == 1 && other == 0 && (d == 0 ? mul == 1 && (uint64_t)factor * 3u == 1u : mul == 0), message);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

int main(void)
{
    test_api_verifier_and_dump();
    test_exact_division();
    test_cse_intersects();
    distribution_case(ANVIL_OP_ADD, true, false, true, true);
    distribution_case(ANVIL_OP_SUB, true, false, true, true);
    distribution_case(ANVIL_OP_MUL, true, false, true, true);
    distribution_case(ANVIL_OP_ADD, true, false, false, true);
    distribution_case(ANVIL_OP_ADD, false, false, true, false);
    distribution_case(ANVIL_OP_ADD, true, true, true, false);
    test_licm_splits_invariant_index();
    if (failures) {
        fprintf(stderr, "no_wrap_regression: %d failure(s)\n", failures);
        return 1;
    }
    printf("no_wrap_regression: all checks passed\n");
    return 0;
}
