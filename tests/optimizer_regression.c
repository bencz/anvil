/*
 * Regression tests for target-independent optimizer passes.
 */

#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>

#include <math.h>
#include <stdint.h>
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

static anvil_ctx_t *new_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    CHECK(ctx != NULL, "context should be created");
    return ctx;
}

static anvil_func_t *new_i32_func(anvil_ctx_t *ctx, anvil_module_t **out_mod,
                                  const char *module_name, const char *func_name,
                                  anvil_type_t **params, size_t num_params)
{
    anvil_module_t *mod = anvil_module_create(ctx, module_name);
    CHECK(mod != NULL, "module should be created");
    if (!mod) return NULL;

    anvil_type_t *fn_type = anvil_type_func(ctx, anvil_type_i32(ctx), params,
                                            num_params, false);
    anvil_func_t *fn = anvil_func_create(mod, func_name, fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "function should be created");
    if (!fn) {
        anvil_module_destroy(mod);
        return NULL;
    }

    if (out_mod) *out_mod = mod;
    return fn;
}

static anvil_func_t *new_typed_func(anvil_ctx_t *ctx, anvil_module_t **out_mod,
                                    const char *module_name, const char *func_name,
                                    anvil_type_t *ret_type,
                                    anvil_type_t **params, size_t num_params)
{
    anvil_module_t *mod = anvil_module_create(ctx, module_name);
    CHECK(mod != NULL, "typed module should be created");
    if (!mod) return NULL;

    anvil_type_t *fn_type = anvil_type_func(ctx, ret_type, params,
                                            num_params, false);
    anvil_func_t *fn = anvil_func_create(mod, func_name, fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "typed function should be created");
    if (!fn) {
        anvil_module_destroy(mod);
        return NULL;
    }

    if (out_mod) *out_mod = mod;
    return fn;
}

static bool func_has_block(anvil_func_t *fn, anvil_block_t *target)
{
    if (!fn || !target) return false;

    for (anvil_block_t *block = fn->blocks; block; block = block->next) {
        if (block == target) return true;
    }
    return false;
}

static bool block_has_instr(anvil_block_t *block, anvil_instr_t *target)
{
    if (!block || !target) return false;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (instr == target) return true;
    }
    return false;
}

static void check_module_valid(anvil_module_t *mod, const char *message)
{
    char error[256] = { 0 };
    bool ok = anvil_module_verify(mod, error, sizeof(error));
    if (!ok) fprintf(stderr, "verify error: %s\n", error);
    CHECK(ok, message);
}

static int custom_pass_calls;

static anvil_pass_result_t custom_count_pass(anvil_func_t *func)
{
    (void)func;
    custom_pass_calls++;
    return ANVIL_PASS_RUN_UNCHANGED;
}

static anvil_pass_result_t custom_error_pass(anvil_func_t *func)
{
    (void)func;
    return ANVIL_PASS_RUN_ERROR;
}

static anvil_pass_result_t custom_always_changes(anvil_func_t *func)
{
    (void)func;
    custom_pass_calls++;
    return ANVIL_PASS_RUN_CHANGED;
}

static anvil_pass_result_t custom_corrupts_ir(anvil_func_t *func)
{
    anvil_instr_t *instr = func && func->blocks ? func->blocks->first : NULL;
    if (!instr) return ANVIL_PASS_RUN_ERROR;
    instr->op = ANVIL_OP_NOP; /* deliberately non-canonical fault injection */
    return ANVIL_PASS_RUN_CHANGED;
}

static void test_const_fold_preserves_integer_division_by_zero(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_div_zero", "div_zero", NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *div = anvil_build_sdiv(ctx, anvil_const_i32(ctx, 42),
                                              anvil_const_i32(ctx, 0), "divz");
        anvil_build_ret(ctx, div);

        bool changed = anvil_pass_const_fold(fn);
        CHECK(!changed, "constant folding must not fold integer division by zero");
        CHECK(div->kind == ANVIL_VAL_INSTR &&
              div->data.instr->op == ANVIL_OP_SDIV,
              "division by zero instruction should remain in IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_const_fold_preserves_zero_divided_by_unknown(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i32(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_zero_div", "zero_div",
                                    params, 1);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *div = anvil_build_sdiv(ctx, anvil_const_i32(ctx, 0),
                                              anvil_func_get_param(fn, 0),
                                              "maybe_divz");
        anvil_build_ret(ctx, div);

        bool changed = anvil_pass_const_fold(fn);
        CHECK(!changed, "0 / x must not fold unless x is known non-zero");
        CHECK(div->kind == ANVIL_VAL_INSTR &&
              div->data.instr->op == ANVIL_OP_SDIV,
              "0 / unknown should remain in IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_const_fold_rejects_invalid_shift_amount(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_bad_shift", "bad_shift",
                                    NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *shift = anvil_build_shl(ctx, anvil_const_i32(ctx, 1),
                                               anvil_const_i32(ctx, 40), "sh");
        anvil_build_ret(ctx, shift);

        bool changed = anvil_pass_const_fold(fn);
        CHECK(!changed, "i32 shift by 40 must not be folded");
        CHECK(shift->kind == ANVIL_VAL_INSTR &&
              shift->data.instr->op == ANVIL_OP_SHL,
              "invalid shift should remain in IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_const_fold_keeps_valid_shift_fold(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_good_shift", "good_shift",
                                    NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *shift = anvil_build_shl(ctx, anvil_const_i32(ctx, 1),
                                               anvil_const_i32(ctx, 3), "sh");
        anvil_build_ret(ctx, shift);
        anvil_instr_t *shift_instr = shift->data.instr;
        anvil_instr_t *ret = anvil_func_get_entry(fn)->last;

        bool changed = anvil_pass_const_fold(fn);
        CHECK(changed, "valid constant shift should still fold");
        CHECK(!block_has_instr(anvil_func_get_entry(fn), shift_instr),
              "folded shift instruction should be unlinked");
        CHECK(ret && ret->op == ANVIL_OP_RET &&
              ret->operands[0]->kind == ANVIL_VAL_CONST_INT &&
              ret->operands[0]->data.i == 8,
              "1 << 3 should fold to 8");
        check_module_valid(mod,
                           "const-fold must leave valid IR without a DCE cleanup");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_copy_prop_all_ones_is_width_aware(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *i64_params[] = { i64 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_copy_i64_mask",
                                      "copy_i64_mask", i64, i64_params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *masked = anvil_build_and(ctx, x,
                                                anvil_const_i64(ctx,
                                                                INT64_C(0xFFFFFFFF)),
                                                "masked");
        anvil_build_ret(ctx, masked);

        bool changed = anvil_pass_copy_prop(fn);
        CHECK(!changed,
              "i64 x & 0xFFFFFFFF must not be treated as an identity copy");
        CHECK(entry->last->operands[0] == masked,
              "i64 low-32-bit mask must remain observable");
    }
    anvil_module_destroy(mod);

    anvil_type_t *u32 = anvil_type_u32(ctx);
    anvil_type_t *u32_params[] = { u32 };
    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_copy_u32_mask", "copy_u32_mask",
                        u32, u32_params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *masked = anvil_build_and(ctx, x,
                                                anvil_const_u32(ctx, UINT32_MAX),
                                                "masked");
        anvil_build_ret(ctx, masked);

        bool changed = anvil_pass_copy_prop(fn);
        CHECK(changed, "u32 x & UINT32_MAX should be recognized as a copy");
        CHECK(entry->last->operands[0] == x,
              "u32 all-ones mask should propagate the original value");
    }
    anvil_module_destroy(mod);

    anvil_ctx_destroy(ctx);
}

static void test_const_fold_neg_int64_min_is_modular(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_neg_int64_min",
                                      "neg_int64_min", anvil_type_i64(ctx),
                                      NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *neg = anvil_build_neg(ctx,
                                             anvil_const_i64(ctx, INT64_MIN),
                                             "neg");
        anvil_build_ret(ctx, neg);

        bool changed = anvil_pass_const_fold(fn);
        anvil_value_t *result = entry->last->operands[0];
        CHECK(changed, "neg INT64_MIN should fold with fixed-width semantics");
        CHECK(result && result->kind == ANVIL_VAL_CONST_INT &&
              result->data.i == INT64_MIN,
              "neg INT64_MIN should wrap to the INT64_MIN bit pattern");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_const_fold_strict_fp_special_values(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_fabs_negzero",
                                      "fabs_negzero", f64, NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *abs = anvil_build_fabs(ctx,
                                              anvil_const_f64(ctx, -0.0),
                                              "abs");
        anvil_build_ret(ctx, abs);
        CHECK(anvil_pass_const_fold(fn), "fabs(-0) should fold");
        anvil_value_t *result = entry->last->operands[0];
        CHECK(result && result->kind == ANVIL_VAL_CONST_FLOAT &&
              result->data.f == 0.0 && !signbit(result->data.f),
              "fabs(-0) must produce positive zero");
    }
    anvil_module_destroy(mod);

    anvil_type_t *params[] = { f64 };
    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_fsub_negzero", "fsub_negzero",
                        f64, params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *sub = anvil_build_fsub(ctx, anvil_func_get_param(fn, 0),
                                              anvil_const_f64(ctx, -0.0),
                                              "sub");
        anvil_value_t *mul = anvil_build_fmul(ctx, anvil_func_get_param(fn, 0),
                                              anvil_const_f64(ctx, 1.0),
                                              "mul_one");
        anvil_value_t *div = anvil_build_fdiv(ctx, anvil_func_get_param(fn, 0),
                                              anvil_const_f64(ctx, 1.0),
                                              "div_one");
        anvil_build_ret(ctx, sub);
        CHECK(!anvil_pass_const_fold(fn),
              "strict FP must not apply identities without formal fast-math flags");
        CHECK(entry->last->operands[0] == sub &&
              block_has_instr(entry, mul->data.instr) &&
              block_has_instr(entry, div->data.instr),
              "x-(-0), x*1 and x/1 must remain represented in strict IR");
    }
    anvil_module_destroy(mod);

    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_fp_nan", "fp_nan", f64, NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *sum = anvil_build_fadd(ctx, anvil_const_f64(ctx, NAN),
                                              anvil_const_f64(ctx, 1.0), "sum");
        anvil_build_ret(ctx, sum);
        CHECK(!anvil_pass_const_fold(fn),
              "strict NaN arithmetic must remain to preserve quieting/exceptions");
        CHECK(entry->last->operands[0] == sum,
              "NaN arithmetic must remain until strict target evaluation exists");
    }
    anvil_module_destroy(mod);

    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_fp_inf", "fp_inf", f64, NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *neg = anvil_build_fneg(ctx,
                                              anvil_const_f64(ctx, INFINITY),
                                              "neg");
        anvil_build_ret(ctx, neg);
        CHECK(anvil_pass_const_fold(fn), "infinity negation should fold");
        anvil_value_t *result = entry->last->operands[0];
        CHECK(result && result->kind == ANVIL_VAL_CONST_FLOAT &&
              isinf(result->data.f) && signbit(result->data.f),
              "constant folding must preserve negative infinity");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);

    ctx = anvil_ctx_create_for_target(ANVIL_ARCH_S370);
    CHECK(ctx != NULL, "HFP context should be created");
    if (!ctx) return;
    f64 = anvil_type_f64(ctx);
    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_hfp_no_host_fold", "hfp_no_host_fold",
                        f64, NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *neg = anvil_build_fneg(ctx, anvil_const_f64(ctx, 1.0),
                                              "hfp_neg");
        anvil_build_ret(ctx, neg);
        CHECK(!anvil_pass_const_fold(fn),
              "host IEEE evaluator must not fold IBM HFP operations");
        CHECK(entry->last->operands[0] == neg,
              "HFP operation must remain for target lowering");
    }
    anvil_module_destroy(mod);

    mod = NULL;
    fn = new_typed_func(ctx, &mod, "opt_hfp_fcmp", "hfp_fcmp",
                        anvil_type_i1(ctx), NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *cmp = anvil_build_fcmp(
            ctx, ANVIL_FCMP_OEQ, anvil_const_f64(ctx, 1.0),
            anvil_const_f64(ctx, 1.0), "hfp_cmp");
        anvil_build_ret(ctx, cmp);
        CHECK(!anvil_pass_const_fold(fn),
              "host IEEE FCMP evaluator must not fold IBM HFP constants");
        CHECK(entry->last->operands[0] == cmp,
              "HFP FCMP must remain for target-specific evaluation");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_const_fold_all_fcmp_predicates(void)
{
    static const bool nan_expected[] = {
        false, false, false, false, false, false, false, false,
        true, true, true, true, true, true, true, true
    };

    for (int pred = ANVIL_FCMP_FALSE; pred <= ANVIL_FCMP_TRUE; pred++) {
        anvil_ctx_t *ctx = new_ctx();
        if (!ctx) return;
        anvil_module_t *mod = NULL;
        anvil_func_t *fn = new_typed_func(
            ctx, &mod, "opt_fcmp_fold", "fcmp_fold", anvil_type_i1(ctx),
            NULL, 0);
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_set_insert_point(ctx, entry);
            anvil_value_t *cmp = anvil_build_fcmp(
                ctx, (anvil_fcmp_pred_t)pred, anvil_const_f64(ctx, NAN),
                anvil_const_f64(ctx, 1.0), "cmp");
            anvil_build_ret(ctx, cmp);
            CHECK(anvil_pass_const_fold(fn) == ANVIL_PASS_RUN_CHANGED,
                  "constant FCMP predicate should fold to i1");
            anvil_value_t *result = entry->last->operands[0];
            CHECK(result && result->kind == ANVIL_VAL_CONST_INT &&
                  result->type == anvil_type_i1(ctx) &&
                  (result->data.i != 0) == nan_expected[pred],
                  "FCMP NaN predicate truth table must be exact");
            check_module_valid(mod, "folded FCMP must preserve valid i1 IR");
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }

    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(
        ctx, &mod, "opt_fcmp_zero", "fcmp_zero", anvil_type_i1(ctx), NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *cmp = anvil_build_fcmp(
            ctx, ANVIL_FCMP_OEQ, anvil_const_f64(ctx, -0.0),
            anvil_const_f64(ctx, +0.0), "zero_eq");
        anvil_build_ret(ctx, cmp);
        CHECK(anvil_pass_const_fold(fn) == ANVIL_PASS_RUN_CHANGED,
              "ordered equality of signed zeros should fold");
        CHECK(entry->last->operands[0]->kind == ANVIL_VAL_CONST_INT &&
              entry->last->operands[0]->data.i == 1,
              "-0 and +0 must compare equal");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_cse_keys_fcmp_predicate(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64, f64 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_fcmp_cse", "fcmp_cse",
                                      anvil_type_i1(ctx), params, 2);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *lhs = anvil_func_get_param(fn, 0);
        anvil_value_t *rhs = anvil_func_get_param(fn, 1);
        anvil_value_t *eq = anvil_build_fcmp(ctx, ANVIL_FCMP_OEQ,
                                             lhs, rhs, "eq");
        anvil_value_t *ne = anvil_build_fcmp(ctx, ANVIL_FCMP_UNE,
                                             lhs, rhs, "ne");
        anvil_value_t *duplicate = anvil_build_fcmp(ctx, ANVIL_FCMP_OEQ,
                                                    lhs, rhs, "eq2");
        anvil_value_t *selected = anvil_build_select(ctx, duplicate, ne, eq,
                                                     "selected");
        anvil_build_ret(ctx, selected);

        CHECK(anvil_pass_cse(fn) == ANVIL_PASS_RUN_CHANGED,
              "same FCMP predicate should CSE");
        CHECK(!block_has_instr(entry, duplicate->data.instr) &&
              block_has_instr(entry, eq->data.instr) &&
              block_has_instr(entry, ne->data.instr),
              "CSE must merge identical FCMP but preserve different predicates");
        check_module_valid(mod, "FCMP predicate-aware CSE must preserve valid IR");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_strength_reduce_u64_high_bit_and_typed_constants(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *params[] = { u64 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_strength_u64_high",
                                      "strength_u64_high", u64, params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *high = anvil_const_u64(ctx, UINT64_C(1) << 63);
        anvil_value_t *mul = anvil_build_mul(ctx, x, high, "mul");
        anvil_value_t *div = anvil_build_udiv(ctx, x, high, "div");
        anvil_value_t *modulo = anvil_build_umod(ctx, x, high, "modulo");
        anvil_build_ret(ctx, modulo);

        CHECK(anvil_pass_strength_reduce(fn),
              "u64 high-bit powers of two should be strength-reduced");
        CHECK(mul->data.instr->op == ANVIL_OP_SHL &&
              mul->data.instr->operands[1]->type == u64 &&
              mul->data.instr->operands[1]->data.u == 63,
              "u64 multiply should become a typed shift by 63");
        CHECK(div->data.instr->op == ANVIL_OP_SHR &&
              div->data.instr->operands[1]->type == u64 &&
              div->data.instr->operands[1]->data.u == 63,
              "u64 division should become a typed logical shift by 63");
        CHECK(modulo->data.instr->op == ANVIL_OP_AND &&
              modulo->data.instr->operands[1]->type == u64 &&
              modulo->data.instr->operands[1]->data.u == INT64_MAX,
              "u64 modulo should become AND with a typed 0x7fff... mask");
        check_module_valid(mod, "u64 strength reduction should preserve valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_strength_reduce_is_transactional_on_oom(void)
{
    struct {
        anvil_op_t op;
        bool constant_on_left;
        const char *message;
    } cases[] = {
        { ANVIL_OP_MUL, false, "rhs multiply" },
        { ANVIL_OP_MUL, true,  "lhs multiply" },
        { ANVIL_OP_UDIV, false, "unsigned division" },
        { ANVIL_OP_UMOD, false, "unsigned modulo" }
    };

    for (size_t case_index = 0;
         case_index < sizeof(cases) / sizeof(cases[0]); case_index++) {
        anvil_ctx_t *ctx = new_ctx();
        if (!ctx) return;
        anvil_type_t *u32 = anvil_type_u32(ctx);
        anvil_type_t *params[] = { u32 };
        anvil_module_t *mod = NULL;
        anvil_func_t *fn = new_typed_func(ctx, &mod, "opt_strength_oom",
                                          "strength_oom", u32, params, 1);
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *x = anvil_func_get_param(fn, 0);
            anvil_value_t *power = anvil_const_u32(ctx, 4);
            anvil_value_t *lhs = cases[case_index].constant_on_left ? power : x;
            anvil_value_t *rhs = cases[case_index].constant_on_left ? x : power;
            anvil_value_t *value = NULL;
            if (cases[case_index].op == ANVIL_OP_MUL)
                value = anvil_build_mul(ctx, lhs, rhs, "candidate");
            else if (cases[case_index].op == ANVIL_OP_UDIV)
                value = anvil_build_udiv(ctx, lhs, rhs, "candidate");
            else
                value = anvil_build_umod(ctx, lhs, rhs, "candidate");
            anvil_build_ret(ctx, value);

            anvil_instr_t *instr = value->data.instr;
            anvil_test_fail_alloc_after(ctx, 0);
            CHECK(anvil_pass_strength_reduce(fn) == ANVIL_PASS_RUN_ERROR,
                  "strength-reduction constant allocation OOM must propagate");
            CHECK(instr->op == cases[case_index].op &&
                  instr->operands[0] == lhs && instr->operands[1] == rhs,
                  cases[case_index].message);
            anvil_test_disable_alloc_fail(ctx);
            check_module_valid(mod,
                               "failed strength reduction must leave original IR valid");
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_dead_store_keeps_store_before_may_alias_load(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32, ptr_i32 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_dse_alias",
                                    "dse_alias", params, 2);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *p = anvil_func_get_param(fn, 0);
        anvil_value_t *q = anvil_func_get_param(fn, 1);
        anvil_build_store(ctx, anvil_const_i32(ctx, 1), p);
        anvil_instr_t *first_store = entry->last;
        anvil_value_t *observed = anvil_build_load(ctx, i32, q, "observed");
        anvil_build_store(ctx, anvil_const_i32(ctx, 2), p);
        anvil_build_ret(ctx, observed);

        bool changed = anvil_pass_dead_store(fn);
        CHECK(!changed,
              "DSE must not remove a store across a possibly-aliasing load");
        CHECK(first_store && first_store->op == ANVIL_OP_STORE,
              "store before may-alias load must remain a store");
        check_module_valid(mod, "DSE alias barrier should preserve valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_cse_rewrites_cross_block_uses_before_erase(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_cse_cross_block",
                                    "cse_cross_block", params, 2);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *exit = anvil_block_create(fn, "exit");
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *y = anvil_func_get_param(fn, 1);
        anvil_value_t *first = anvil_build_add(ctx, x, y, "first");
        anvil_value_t *duplicate = anvil_build_add(ctx, x, y, "duplicate");
        anvil_instr_t *duplicate_instr = duplicate->data.instr;
        anvil_value_t *local_use = anvil_build_sub(ctx, duplicate, x, "local");
        anvil_build_br(ctx, exit);

        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, duplicate);
        anvil_instr_t *ret = exit->last;

        bool changed = anvil_pass_cse(fn);
        CHECK(changed, "CSE should eliminate the duplicate expression");
        CHECK(!block_has_instr(entry, duplicate_instr),
              "duplicate expression should be unlinked");
        CHECK(local_use->data.instr->operands[0] == first,
              "CSE should rewrite a later same-block use");
        CHECK(ret && ret->operands[0] == first,
              "CSE should rewrite a successor-block use before deleting the def");

        check_module_valid(mod,
                           "CSE must preserve valid IR without a DCE cleanup");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_store_load_prop_rewrites_cross_block_uses_before_erase(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32, i32 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_store_load_cross_block",
                                    "store_load_cross_block", params, 2);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *exit = anvil_block_create(fn, "exit");
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *p = anvil_func_get_param(fn, 0);
        anvil_value_t *stored = anvil_func_get_param(fn, 1);
        anvil_build_store(ctx, stored, p);
        anvil_value_t *loaded = anvil_build_load(ctx, i32, p, "loaded");
        anvil_instr_t *load_instr = loaded->data.instr;
        anvil_value_t *local_use = anvil_build_add(ctx, loaded,
                                                   anvil_const_i32(ctx, 1),
                                                   "local");
        anvil_build_br(ctx, exit);

        anvil_set_insert_point(ctx, exit);
        anvil_build_ret(ctx, loaded);
        anvil_instr_t *ret = exit->last;

        bool changed = anvil_pass_store_load_prop(fn);
        CHECK(changed, "store-load propagation should eliminate the load");
        CHECK(!block_has_instr(entry, load_instr),
              "forwarded load should be unlinked");
        CHECK(local_use->data.instr->operands[0] == stored,
              "store-load propagation should rewrite a local use");
        CHECK(ret && ret->operands[0] == stored,
              "store-load propagation should rewrite a successor-block use");

        check_module_valid(mod,
                           "store-load propagation must preserve valid IR without DCE");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_load_elim_erases_canonically(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_load_erase",
                                    "load_erase", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *p = anvil_func_get_param(fn, 0);
        anvil_value_t *first = anvil_build_load(ctx, i32, p, "first");
        anvil_value_t *second = anvil_build_load(ctx, i32, p, "second");
        anvil_instr_t *second_instr = second->data.instr;
        anvil_value_t *sum = anvil_build_add(ctx, second,
                                             anvil_const_i32(ctx, 1), "sum");
        anvil_build_ret(ctx, sum);

        CHECK(anvil_pass_load_elim(fn),
              "redundant load elimination should transform the second load");
        CHECK(!block_has_instr(entry, second_instr),
              "redundant load should be unlinked");
        CHECK(!anvil_pass_load_elim(fn),
              "rerunning load elimination must not erase an instruction twice");
        CHECK(sum->data.instr->operands[0] == first,
              "redundant load use should be rewritten to the available load");
        check_module_valid(mod,
                           "load elimination must preserve valid IR without DCE");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_dead_store_erases_canonically(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_store_erase",
                                    "store_erase", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *p = anvil_func_get_param(fn, 0);
        anvil_build_store(ctx, anvil_const_i32(ctx, 1), p);
        anvil_instr_t *first_store = entry->last;
        anvil_build_store(ctx, anvil_const_i32(ctx, 2), p);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        CHECK(anvil_pass_dead_store(fn),
              "overwritten must-alias store should be eliminated");
        CHECK(!block_has_instr(entry, first_store),
              "dead store should be unlinked");
        check_module_valid(mod,
                           "dead-store elimination must preserve valid IR immediately");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_erase_repairs_builder_cursor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_erase_cursor",
                                    "erase_cursor", NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                        anvil_const_i32(ctx, 2), "unused");

        CHECK(anvil_pass_dce(fn),
              "DCE should erase the last unused instruction");
        CHECK(entry->first == NULL && entry->last == NULL,
              "erasing the only instruction should empty the block");
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
        CHECK(entry->last && entry->last->op == ANVIL_OP_RET,
              "builder should append after its former cursor is erased");
        check_module_valid(mod,
                           "cursor repair after erase should preserve valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_dce_uses_dense_function_worklist(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i32(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_dense_dce", "dense_dce",
                                    params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *value = anvil_func_get_param(fn, 0);
        for (size_t i = 0; i < 4096; i++) {
            value = anvil_build_add(ctx, value, anvil_const_i32(ctx, 1),
                                    "dead_chain");
        }
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        /* IDs are context-global and may be sparse or exhausted because of
         * unrelated modules. DCE must size itself from this function only. */
        ctx->next_value_id = UINT32_MAX;
        CHECK(anvil_pass_dce(fn) == ANVIL_PASS_RUN_CHANGED,
              "dense worklist DCE should remove a long dead dependency chain");
        CHECK(entry->first == entry->last && entry->last &&
              entry->last->op == ANVIL_OP_RET,
              "worklist DCE should reach the chain root in one linear run");
        check_module_valid(mod,
                           "dense DCE must tolerate an exhausted global ID space");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_dce_preserves_may_trap_and_memory_operations(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_dce_effects",
                                    "dce_effects", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *div = anvil_build_sdiv(ctx, anvil_const_i32(ctx, 1),
                                              anvil_const_i32(ctx, 0),
                                              "may_trap");
        anvil_value_t *slot = anvil_build_alloca_dyn(
            ctx, i32, anvil_func_get_param(fn, 0), "dynamic_stack");
        anvil_value_t *load = anvil_build_load(ctx, i32, slot, "may_fault");
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

        CHECK(anvil_pass_dce(fn) == ANVIL_PASS_RUN_UNCHANGED,
              "DCE must preserve operations with trap, stack or memory effects");
        CHECK(block_has_instr(entry, div->data.instr) &&
              block_has_instr(entry, slot->data.instr) &&
              block_has_instr(entry, load->data.instr),
              "unused division, dynamic alloca and load must remain without formal traits");
        check_module_valid(mod,
                           "conservative DCE effect handling must preserve valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_rewrites_phi_from_empty_block_to_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_cfg", "phi_cfg",
                                    NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *empty = anvil_block_create(fn, "empty");
        anvil_block_t *merge = anvil_block_create(fn, "merge");
        CHECK(entry != NULL && empty != NULL && merge != NULL,
              "CFG test blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, empty);

        anvil_set_insert_point(ctx, empty);
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 7), empty);
        anvil_build_ret(ctx, phi);

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(changed, "CFG simplification should remove the empty block");
        CHECK(entry->last && entry->last->op == ANVIL_OP_BR &&
              entry->last->true_block == merge,
              "entry should branch directly to merge");
        CHECK(phi->data.instr->num_phi_incoming == 1,
              "PHI should still have one incoming edge");
        CHECK(phi->data.instr->phi_blocks[0] == entry,
              "PHI incoming edge should be rewritten from empty block to entry");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_removes_phi_incoming_from_dead_const_branch_edge(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_const_branch",
                                    "phi_const_branch", NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *then_block = anvil_block_create(fn, "then");
        anvil_block_t *else_block = anvil_block_create(fn, "else");
        anvil_block_t *merge = anvil_block_create(fn, "merge");
        CHECK(entry != NULL && then_block != NULL && else_block != NULL && merge != NULL,
              "constant branch CFG blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), then_block, else_block);

        anvil_set_insert_point(ctx, then_block);
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, else_block);
        anvil_build_br(ctx, merge);

        anvil_set_insert_point(ctx, merge);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 11), then_block);
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 22), else_block);
        anvil_build_ret(ctx, phi);

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(changed, "constant branch CFG should simplify");
        CHECK(phi->data.instr->num_phi_incoming == 1,
              "PHI should lose incoming value from the dead branch edge");
        CHECK(phi->data.instr->phi_blocks[0] != else_block,
              "PHI should not reference the unreachable else block");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_preserves_switch_successors_as_reachable(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i32(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_switch_cfg",
                                    "switch_cfg", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *case_block = anvil_block_create(fn, "case0");
        anvil_block_t *default_block = anvil_block_create(fn, "default");
        CHECK(entry != NULL && case_block != NULL && default_block != NULL,
              "switch CFG blocks should be created");

        anvil_set_insert_point(ctx, entry);
        anvil_instr_t *sw =
            anvil_build_switch(ctx, anvil_func_get_param(fn, 0), default_block);
        CHECK(sw != NULL, "switch terminator should be created");
        CHECK(anvil_switch_add_case(sw, anvil_const_i32(ctx, 0), case_block),
              "switch case should be added");

        anvil_set_insert_point(ctx, case_block);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 7));

        anvil_set_insert_point(ctx, default_block);
        anvil_build_ret(ctx, anvil_const_i32(ctx, 99));

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(!changed, "switch successors should not be removed as unreachable");
        CHECK(func_has_block(fn, case_block),
              "switch case block should remain in function");
        CHECK(func_has_block(fn, default_block),
              "switch default block should remain in function");

        char error[256] = { 0 };
        bool verify_ok = anvil_module_verify(mod, error, sizeof(error));
        if (!verify_ok) {
            fprintf(stderr, "verify error after switch simplify_cfg: %s\n", error);
        }
        CHECK(verify_ok,
              "module should remain valid after simplify_cfg handles switch successors");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_merge_rewrites_downstream_phi_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i32(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_merge",
                                    "phi_merge", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *middle = anvil_block_create(fn, "middle");
        anvil_block_t *join = anvil_block_create(fn, "join");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, middle);

        anvil_set_insert_point(ctx, middle);
        anvil_value_t *value = anvil_build_add(ctx,
                                               anvil_func_get_param(fn, 0),
                                               anvil_const_i32(ctx, 1),
                                               "value");
        anvil_build_br(ctx, join);

        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, value, middle);
        anvil_build_ret(ctx, phi);

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(changed, "CFG simplification should merge the single-predecessor block");
        CHECK(!func_has_block(fn, middle), "merged block should leave the function");
        CHECK(phi->data.instr->num_phi_incoming == 1 &&
              phi->data.instr->phi_blocks[0] == entry,
              "downstream PHI predecessor must follow the merged block");
        check_module_valid(mod, "block merge should preserve downstream PHIs");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_unreachable_removal_updates_live_phi(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_unreachable",
                                    "phi_unreachable", NULL, 0);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *dead = anvil_block_create(fn, "dead");
        anvil_block_t *join = anvil_block_create(fn, "join");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, join);

        anvil_set_insert_point(ctx, dead);
        anvil_value_t *dead_value = anvil_build_add(ctx,
                                                    anvil_const_i32(ctx, 20),
                                                    anvil_const_i32(ctx, 2),
                                                    "dead_value");
        anvil_build_br(ctx, join);

        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 7), entry);
        anvil_phi_add_incoming(phi, dead_value, dead);
        anvil_build_ret(ctx, phi);

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(changed, "CFG simplification should remove an unreachable block");
        CHECK(!func_has_block(fn, dead), "unreachable block should be removed");
        CHECK(phi->data.instr->num_phi_incoming == 1 &&
              phi->data.instr->phi_blocks[0] == entry,
              "live PHI must lose the incoming edge from an unreachable block");
        check_module_valid(mod, "unreachable removal should preserve live PHIs");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_avoids_phi_edge_collision_on_empty_bypass(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i1(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_edge_collision",
                                    "phi_edge_collision", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *empty = anvil_block_create(fn, "empty");
        anvil_block_t *join = anvil_block_create(fn, "join");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_func_get_param(fn, 0), empty, join);

        anvil_set_insert_point(ctx, empty);
        anvil_build_br(ctx, join);

        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 11), entry);
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 22), empty);
        anvil_build_ret(ctx, phi);

        bool changed = anvil_pass_simplify_cfg(fn);
        CHECK(!changed,
              "empty-block bypass must stop when it would collapse PHI edges");
        CHECK(func_has_block(fn, empty),
              "edge-collision empty block must remain as an edge discriminator");
        CHECK(phi->data.instr->num_phi_incoming == 2,
              "edge-collision PHI must retain both semantic inputs");
        check_module_valid(mod, "refused empty bypass should leave valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_treats_duplicate_edges_as_one_predecessor(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i1(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_duplicate_edges",
                                    "phi_duplicate_edges", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *empty = anvil_block_create(fn, "empty");
        anvil_block_t *join = anvil_block_create(fn, "join");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_func_get_param(fn, 0), empty, empty);
        anvil_set_insert_point(ctx, empty);
        anvil_build_br(ctx, join);
        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 9), empty);
        anvil_build_ret(ctx, phi);

        CHECK(anvil_pass_simplify_cfg(fn) == ANVIL_PASS_RUN_CHANGED,
              "duplicate CFG edges must not duplicate PHI predecessors");
        CHECK(!func_has_block(fn, empty),
              "empty block with one unique predecessor should be bypassed");
        CHECK(phi->data.instr->num_phi_incoming == 1 &&
              phi->data.instr->phi_blocks[0] == entry,
              "two edges from one block must produce one PHI incoming block");
        check_module_valid(mod,
                           "duplicate-edge empty bypass must preserve valid IR");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_simplify_cfg_phi_expansion_is_transactional_on_oom(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_type_t *params[] = { anvil_type_i1(ctx) };
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_phi_transaction",
                                    "phi_transaction", params, 1);
    if (fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *empty = anvil_block_create(fn, "empty");
        anvil_block_t *join = anvil_block_create(fn, "join");

        anvil_set_insert_point(ctx, entry);
        anvil_build_br_cond(ctx, anvil_func_get_param(fn, 0), left, right);
        anvil_set_insert_point(ctx, left);
        anvil_build_br(ctx, empty);
        anvil_set_insert_point(ctx, right);
        anvil_build_br(ctx, empty);
        anvil_set_insert_point(ctx, empty);
        anvil_build_br(ctx, join);
        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, anvil_type_i32(ctx), "p");
        anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 17), empty);
        anvil_build_ret(ctx, phi);

        /* The first 12 allocations build the dense index, predecessor cache,
         * predecessor snapshot and PHI plan.  Fail the first expanded operand
         * array, after preflight has begun but before its atomic commit. */
        anvil_test_fail_alloc_after(ctx, 12);
        CHECK(anvil_pass_simplify_cfg(fn) == ANVIL_PASS_RUN_ERROR,
              "OOM during multi-predecessor PHI expansion must be reported");
        CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
              "PHI expansion OOM must preserve the allocator diagnostic");
        CHECK(func_has_block(fn, empty) &&
              left->last->true_block == empty && right->last->true_block == empty,
              "failed PHI preflight must not redirect CFG edges");
        CHECK(phi->data.instr->num_phi_incoming == 1 &&
              phi->data.instr->phi_blocks[0] == empty,
              "failed PHI preflight must not partially rewrite any PHI");
        anvil_test_disable_alloc_fail(ctx);
        check_module_valid(mod,
                           "IR must remain valid after transactional PHI OOM");

        CHECK(anvil_pass_simplify_cfg(fn) == ANVIL_PASS_RUN_CHANGED,
              "CFG simplification should be retryable after PHI allocation OOM");
        CHECK(!func_has_block(fn, empty) &&
              phi->data.instr->num_phi_incoming == 2,
              "successful retry must expand PHI to both unique predecessors");
        check_module_valid(mod,
                           "retry after transactional PHI OOM must remain valid");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_pass_manager_rejects_invalid_ids_and_levels(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
    CHECK(pm != NULL, "pass manager should be created");
    if (pm) {
        CHECK(anvil_pass_manager_enable(pm, ANVIL_PASS_CONST_FOLD) == ANVIL_OK,
              "valid pass ID should be enabled");
        CHECK(anvil_pass_manager_enable(pm, (anvil_pass_id_t)-1) ==
                  ANVIL_ERR_INVALID_ARG,
              "negative pass ID enable must return an error");
        CHECK(anvil_pass_manager_disable(pm, (anvil_pass_id_t)-1) ==
                  ANVIL_ERR_INVALID_ARG,
              "negative pass ID disable must return an error");
        CHECK(!anvil_pass_manager_is_enabled(pm, (anvil_pass_id_t)-1),
              "negative pass ID must be rejected");
        CHECK(anvil_pass_manager_is_enabled(pm, ANVIL_PASS_CONST_FOLD),
              "invalid pass ID must not corrupt neighboring enable flags");

        CHECK(anvil_pass_manager_set_level(pm, ANVIL_OPT_BASIC) == ANVIL_OK,
              "valid direct optimization level should succeed");
        CHECK(anvil_pass_manager_set_level(pm, (anvil_opt_level_t)-1) ==
                  ANVIL_ERR_INVALID_ARG,
              "negative direct optimization level must return an error");
        CHECK(anvil_pass_manager_get_level(pm) == ANVIL_OPT_BASIC,
              "direct invalid optimization level must leave the level unchanged");

        CHECK(anvil_pass_manager_set_level(pm, ANVIL_OPT_AGGRESSIVE) == ANVIL_OK,
              "O3 direct optimization level should succeed");
        CHECK(anvil_pass_manager_is_enabled(pm, ANVIL_PASS_COMMON_SUBEXPR),
              "O3 should retain the complete implemented O2 pass set");
    }

    CHECK(anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD) == ANVIL_OK,
          "valid optimization level should be accepted");
    CHECK(anvil_ctx_set_opt_level(ctx, (anvil_opt_level_t)-1) ==
              ANVIL_ERR_INVALID_ARG,
          "negative context optimization level must be rejected");
    CHECK(anvil_ctx_get_opt_level(ctx) == ANVIL_OPT_STANDARD,
          "rejected context level must leave the previous level unchanged");
    CHECK(anvil_ctx_set_opt_level(ctx,
                                  (anvil_opt_level_t)(ANVIL_OPT_AGGRESSIVE + 1)) ==
              ANVIL_ERR_INVALID_ARG,
          "out-of-range context optimization level must be rejected");

    anvil_ctx_destroy(ctx);
}

static void test_pass_manager_contracts(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "pm_contracts",
                                    "pm_contracts", NULL, 0);
    if (!fn) {
        anvil_ctx_destroy(ctx);
        return;
    }
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                         anvil_const_i32(ctx, 3), "sum");
    anvil_instr_t *sum_instr = sum->data.instr;
    anvil_build_ret(ctx, sum);

    anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
    CHECK(pm != NULL, "contract test pass manager should be created");
    if (pm) {
        /* Manual selection is meaningful even at O0. */
        CHECK(anvil_pass_manager_enable(pm, ANVIL_PASS_CONST_FOLD) == ANVIL_OK,
              "manual O0 pass enable should succeed");
        CHECK(anvil_ctx_get_opt_level(ctx) == ANVIL_OPT_NONE,
              "manual-pass test should remain at O0");
        CHECK(anvil_module_optimize(mod) == ANVIL_OK,
              "manually enabled O0 pass should run successfully");
        CHECK(!block_has_instr(anvil_func_get_entry(fn), sum_instr),
              "manual const-fold must run through module optimization at O0");

        anvil_pass_info_t invalid = {
            .id = ANVIL_PASS_CONST_FOLD,
            .name = "invalid-id",
            .description = "invalid custom id",
            .run = custom_count_pass,
            .min_level = ANVIL_OPT_NONE
        };
        CHECK(anvil_pass_manager_register(pm, &invalid) == ANVIL_ERR_INVALID_ARG,
              "custom pass must use the custom ID sentinel");
        invalid.id = ANVIL_PASS_CUSTOM;
        invalid.run = NULL;
        CHECK(anvil_pass_manager_register(pm, &invalid) == ANVIL_ERR_INVALID_ARG,
              "custom pass must provide a run callback");
        invalid.run = custom_count_pass;
        invalid.min_level = (anvil_opt_level_t)-1;
        CHECK(anvil_pass_manager_register(pm, &invalid) == ANVIL_ERR_INVALID_ARG,
              "custom pass must provide a valid minimum level");

        anvil_pass_info_t gated = {
            .id = ANVIL_PASS_CUSTOM,
            .name = "gated",
            .description = "minimum-level gate",
            .run = custom_count_pass,
            .min_level = ANVIL_OPT_BASIC
        };
        CHECK(anvil_pass_manager_register(pm, &gated) == ANVIL_OK,
              "valid gated custom pass should register");
        custom_pass_calls = 0;
        CHECK(anvil_pass_manager_set_level(pm, ANVIL_OPT_DEBUG) == ANVIL_OK,
              "debug level should be accepted");
        CHECK(anvil_pass_manager_run_module(pm, mod) != ANVIL_PASS_RUN_ERROR,
              "pipeline below custom minimum should remain valid");
        CHECK(custom_pass_calls == 0,
              "custom min_level must be respected");
        CHECK(anvil_pass_manager_set_level(pm, ANVIL_OPT_BASIC) == ANVIL_OK,
              "basic level should be accepted");
        CHECK(anvil_pass_manager_run_module(pm, mod) != ANVIL_PASS_RUN_ERROR,
              "pipeline at custom minimum should remain valid");
        CHECK(custom_pass_calls == 1,
              "custom pass should run at its minimum level");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_pass_manager_errors_verification_and_ownership(void)
{
    /* Registration allocation failures must be transactional, and strings
     * must be owned by the manager rather than borrowed from the caller. */
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "pm_errors", "pm_errors",
                                    NULL, 0);
    if (!fn) {
        anvil_ctx_destroy(ctx);
        return;
    }
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
    anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);

    char name[32] = "owned-error-pass";
    char description[32] = "owned description";
    anvil_pass_info_t pass = {
        .id = ANVIL_PASS_CUSTOM,
        .name = name,
        .description = description,
        .run = custom_error_pass,
        .min_level = ANVIL_OPT_NONE
    };
    anvil_test_fail_alloc_after(ctx, 1); /* name succeeds, description fails */
    CHECK(anvil_pass_manager_register(pm, &pass) == ANVIL_ERR_NOMEM,
          "custom metadata allocation failure must propagate");
    anvil_test_disable_alloc_fail(ctx);
    custom_pass_calls = 0;
    CHECK(anvil_pass_manager_run_module(pm, mod) == ANVIL_PASS_RUN_UNCHANGED,
          "failed registration must not leave a partial pass");
    CHECK(custom_pass_calls == 0,
          "failed registration must be transactional");

    CHECK(anvil_pass_manager_register(pm, &pass) == ANVIL_OK,
          "custom error pass should register after allocation recovers");
    strcpy(name, "caller-mutated");
    strcpy(description, "caller-mutated");
    CHECK(anvil_pass_manager_run_module(pm, mod) == ANVIL_PASS_RUN_ERROR,
          "custom pass error status must propagate");
    CHECK(strstr(anvil_ctx_get_error(ctx), "owned-error-pass") != NULL &&
          strstr(anvil_ctx_get_error(ctx), "owned description") != NULL,
          "custom name and description must be deep-copied");

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);

    /* A built-in pass must not collapse allocation failure into the same
     * status as a legitimate no-change result. */
    const anvil_pass_id_t allocating_passes[] = {
        ANVIL_PASS_DCE,
        ANVIL_PASS_COMMON_SUBEXPR,
        ANVIL_PASS_SIMPLIFY_CFG
    };
    for (size_t pass_index = 0;
         pass_index < sizeof(allocating_passes) / sizeof(allocating_passes[0]);
         pass_index++) {
        ctx = new_ctx();
        if (!ctx) return;
        mod = NULL;
        fn = new_i32_func(ctx, &mod, "pm_builtin_oom", "pm_builtin_oom",
                          NULL, 0);
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            (void)anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                                 anvil_const_i32(ctx, 2), "unused");
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
            pm = anvil_ctx_get_pass_manager(ctx);
            CHECK(anvil_pass_manager_enable(
                      pm, allocating_passes[pass_index]) == ANVIL_OK,
                  "allocation-bearing built-in should enable");
            anvil_test_fail_alloc_after(ctx, 0);
            CHECK(anvil_pass_manager_run_func(pm, fn) == ANVIL_PASS_RUN_ERROR,
                  "built-in allocation failure must return pass error");
            CHECK(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM,
                  "built-in allocation failure must preserve the OOM diagnostic");
            anvil_test_disable_alloc_fail(ctx);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }

    /* Verification occurs immediately after the offending pass, even when no
     * later cleanup pass is enabled. */
    ctx = new_ctx();
    if (!ctx) return;
    fn = new_i32_func(ctx, &mod, "pm_verify", "pm_verify", NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 1),
                                             anvil_const_i32(ctx, 2), "sum");
        anvil_build_ret(ctx, sum);
        pm = anvil_ctx_get_pass_manager(ctx);
        anvil_pass_info_t corrupt = {
            .id = ANVIL_PASS_CUSTOM,
            .name = "corrupt",
            .description = "verifier fault injection",
            .run = custom_corrupts_ir,
            .min_level = ANVIL_OPT_NONE
        };
        CHECK(anvil_pass_manager_register(pm, &corrupt) == ANVIL_OK,
              "corrupting test pass should register");
        CHECK(anvil_pass_manager_run_module(pm, mod) == ANVIL_PASS_RUN_ERROR,
              "invalid IR must fail immediately after the producing pass");
        CHECK(strstr(anvil_ctx_get_error(ctx), "corrupt") != NULL,
              "verification error should identify the offending pass");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);

    /* A pipeline that keeps reporting changes is an error, not a silent
     * successful truncation at the iteration cap. */
    ctx = new_ctx();
    if (!ctx) return;
    fn = new_i32_func(ctx, &mod, "pm_fixpoint", "pm_fixpoint", NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_build_ret(ctx, anvil_const_i32(ctx, 0));
        pm = anvil_ctx_get_pass_manager(ctx);
        anvil_pass_info_t looping = {
            .id = ANVIL_PASS_CUSTOM,
            .name = "never-converges",
            .description = "fixpoint fault injection",
            .run = custom_always_changes,
            .min_level = ANVIL_OPT_NONE
        };
        CHECK(anvil_pass_manager_register(pm, &looping) == ANVIL_OK,
              "non-converging test pass should register");
        CHECK(anvil_pass_manager_set_iteration_limit(pm, 3) == ANVIL_OK,
              "custom fixpoint limit should be configurable");
        CHECK(anvil_pass_manager_get_iteration_limit(pm) == 3,
              "configured fixpoint limit should be observable");
        CHECK(anvil_pass_manager_set_iteration_limit(pm, 0) ==
                  ANVIL_ERR_INVALID_ARG,
              "zero fixpoint limit must be rejected");
        CHECK(anvil_pass_manager_get_iteration_limit(pm) == 3,
              "rejected fixpoint limit must preserve the previous value");
        custom_pass_calls = 0;
        CHECK(anvil_pass_manager_run_module(pm, mod) == ANVIL_PASS_RUN_ERROR,
              "non-converging pipeline must return an error");
        CHECK(custom_pass_calls == 3,
              "non-convergence should use the configured bound");
        CHECK(strstr(anvil_ctx_get_error(ctx), "did not converge") != NULL,
              "non-convergence error should be explicit");
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_codegen_runs_enabled_optimizer_before_backend_prepare_ir(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ARM64);
    CHECK(ctx != NULL, "ARM64 context should be created");
    if (!ctx) return;

    CHECK(anvil_ctx_set_opt_level(ctx, ANVIL_OPT_BASIC) == ANVIL_OK,
          "optimization level should be set");

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = new_i32_func(ctx, &mod, "opt_codegen", "fold_before_codegen",
                                    NULL, 0);
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *sum = anvil_build_add(ctx, anvil_const_i32(ctx, 2),
                                             anvil_const_i32(ctx, 3), "sum");
        anvil_build_ret(ctx, sum);
        anvil_instr_t *sum_instr = sum->data.instr;
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_instr_t *ret = anvil_func_get_entry(fn)->last;

        char *output = NULL;
        size_t len = 0;
        anvil_error_t err = anvil_module_codegen(mod, &output, &len);
        CHECK(err == ANVIL_OK && output != NULL && len > 0,
              "codegen should succeed for optimizer integration test");
        free(output);

        CHECK(!block_has_instr(entry, sum_instr),
              "enabled optimizer should unlink folded IR before backend codegen");
        CHECK(ret && ret->op == ANVIL_OP_RET &&
              ret->operands[0]->kind == ANVIL_VAL_CONST_INT &&
              ret->operands[0]->data.i == 5,
              "codegen-triggered optimizer should fold 2 + 3 before prepare_ir");
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_const_fold_preserves_integer_division_by_zero();
    test_const_fold_preserves_zero_divided_by_unknown();
    test_const_fold_rejects_invalid_shift_amount();
    test_const_fold_keeps_valid_shift_fold();
    test_copy_prop_all_ones_is_width_aware();
    test_const_fold_neg_int64_min_is_modular();
    test_const_fold_strict_fp_special_values();
    test_const_fold_all_fcmp_predicates();
    test_cse_keys_fcmp_predicate();
    test_strength_reduce_u64_high_bit_and_typed_constants();
    test_strength_reduce_is_transactional_on_oom();
    test_dead_store_keeps_store_before_may_alias_load();
    test_cse_rewrites_cross_block_uses_before_erase();
    test_store_load_prop_rewrites_cross_block_uses_before_erase();
    test_load_elim_erases_canonically();
    test_dead_store_erases_canonically();
    test_erase_repairs_builder_cursor();
    test_dce_uses_dense_function_worklist();
    test_dce_preserves_may_trap_and_memory_operations();
    test_simplify_cfg_rewrites_phi_from_empty_block_to_predecessor();
    test_simplify_cfg_removes_phi_incoming_from_dead_const_branch_edge();
    test_simplify_cfg_preserves_switch_successors_as_reachable();
    test_simplify_cfg_merge_rewrites_downstream_phi_predecessor();
    test_simplify_cfg_unreachable_removal_updates_live_phi();
    test_simplify_cfg_avoids_phi_edge_collision_on_empty_bypass();
    test_simplify_cfg_treats_duplicate_edges_as_one_predecessor();
    test_simplify_cfg_phi_expansion_is_transactional_on_oom();
    test_pass_manager_rejects_invalid_ids_and_levels();
    test_pass_manager_contracts();
    test_pass_manager_errors_verification_and_ownership();
    test_codegen_runs_enabled_optimizer_before_backend_prepare_ir();

    if (failures) {
        fprintf(stderr, "%d optimizer regression test(s) failed\n", failures);
        return 1;
    }

    printf("optimizer regression tests passed\n");
    return 0;
}
