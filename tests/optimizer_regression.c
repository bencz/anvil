/*
 * Regression tests for target-independent optimizer passes.
 */

#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

static anvil_ctx_t *new_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
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

static bool func_has_block(anvil_func_t *fn, anvil_block_t *target)
{
    if (!fn || !target) return false;

    for (anvil_block_t *block = fn->blocks; block; block = block->next) {
        if (block == target) return true;
    }
    return false;
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
        anvil_instr_t *ret = anvil_func_get_entry(fn)->last;

        bool changed = anvil_pass_const_fold(fn);
        CHECK(changed, "valid constant shift should still fold");
        CHECK(shift->data.instr->op == ANVIL_OP_NOP,
              "folded shift instruction should be marked dead");
        CHECK(ret && ret->op == ANVIL_OP_RET &&
              ret->operands[0]->kind == ANVIL_VAL_CONST_INT &&
              ret->operands[0]->data.i == 8,
              "1 << 3 should fold to 8");
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
        anvil_build_br_cond(ctx, anvil_const_i8(ctx, 1), then_block, else_block);

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

static void test_codegen_runs_enabled_optimizer_before_backend_prepare_ir(void)
{
    anvil_ctx_t *ctx = new_ctx();
    if (!ctx) return;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available for codegen optimizer test");
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
        anvil_instr_t *ret = anvil_func_get_entry(fn)->last;

        char *output = NULL;
        size_t len = 0;
        anvil_error_t err = anvil_module_codegen(mod, &output, &len);
        CHECK(err == ANVIL_OK && output != NULL && len > 0,
              "codegen should succeed for optimizer integration test");
        free(output);

        CHECK(sum->data.instr->op == ANVIL_OP_NOP,
              "enabled optimizer should run before backend codegen");
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
    test_simplify_cfg_rewrites_phi_from_empty_block_to_predecessor();
    test_simplify_cfg_removes_phi_incoming_from_dead_const_branch_edge();
    test_simplify_cfg_preserves_switch_successors_as_reachable();
    test_codegen_runs_enabled_optimizer_before_backend_prepare_ir();

    if (failures) {
        fprintf(stderr, "%d optimizer regression test(s) failed\n", failures);
        return 1;
    }

    printf("optimizer regression tests passed\n");
    return 0;
}
