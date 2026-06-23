/*
 * Regression tests for the x86-64 -> MachineIR lowering path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_x86_64_mir.h>
#include <anvil/anvil_machine.h>

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

static anvil_ctx_t *new_x86_64_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64) == ANVIL_OK,
          "x86-64 target should be available");
    return ctx;
}

static void check_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) != NULL, msg);
}

static void test_x86_64_lowers_integer_add_with_entry_abi_copies(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_iadd");
    CHECK(mod != NULL, "module should be created");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64, i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "iadd", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "integer add function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "integer add should lower to MachineIR");
            if (mir) {
                anvil_mir_instr_info_t copy0_info;
                anvil_mir_instr_info_t copy1_info;
                CHECK(anvil_mir_get_instr_info(mir, 0, &copy0_info),
                      "first ABI arg copy should be inspectable");
                CHECK(anvil_mir_get_instr_info(mir, 1, &copy1_info),
                      "second ABI arg copy should be inspectable");
                anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
                anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);

                const anvil_mir_vreg_info_t *incoming0_info =
                    anvil_mir_get_vreg_info(mir, incoming0);
                const anvil_mir_vreg_info_t *incoming1_info =
                    anvil_mir_get_vreg_info(mir, incoming1);
                CHECK(incoming0_info && incoming0_info->reg_class == ANVIL_MIR_REG_GPR &&
                      incoming0_info->has_fixed_reg && incoming0_info->fixed_phys_reg == 7,
                      "first incoming integer argument should be fixed to RDI");
                CHECK(incoming1_info && incoming1_info->reg_class == ANVIL_MIR_REG_GPR &&
                      incoming1_info->has_fixed_reg && incoming1_info->fixed_phys_reg == 6,
                      "second incoming integer argument should be fixed to RSI");

                bool saw_add = false;
                bool saw_ret = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "iadd MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_ADD) saw_add = true;
                    if (info.op == ANVIL_MIR_OP_RET) saw_ret = true;
                }
                CHECK(saw_add, "integer add should lower to MIR ADD");
                CHECK(saw_ret, "function should lower a RET");

                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "x86-64 MachineIR regalloc should succeed for integer add");

                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "integer add MIR should emit x86-64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tpushq %rbp\n",
                                   "function should emit a prologue");
                    check_contains(asm_text, "\tret\n",
                                   "function should emit a return");
                    free(asm_text);
                }

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_x86_64_lowers_fp_add_uses_xmm_args(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_fadd");
    CHECK(mod != NULL, "module should be created for FP lowering");
    if (mod) {
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *params[] = { f64, f64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, f64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "fadd", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "FP add function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sum = anvil_build_fadd(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "FP add should lower to MachineIR");
            if (mir) {
                anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
                anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);
                const anvil_mir_vreg_info_t *incoming0_info =
                    anvil_mir_get_vreg_info(mir, incoming0);
                const anvil_mir_vreg_info_t *incoming1_info =
                    anvil_mir_get_vreg_info(mir, incoming1);
                CHECK(incoming0_info && incoming0_info->reg_class == ANVIL_MIR_REG_FPR &&
                      incoming0_info->has_fixed_reg && incoming0_info->fixed_phys_reg == 0,
                      "first incoming FP argument should be fixed to XMM0");
                CHECK(incoming1_info && incoming1_info->reg_class == ANVIL_MIR_REG_FPR &&
                      incoming1_info->has_fixed_reg && incoming1_info->fixed_phys_reg == 1,
                      "second incoming FP argument should be fixed to XMM1");

                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "x86-64 MachineIR regalloc should succeed for FP add");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "FP add MIR should emit x86-64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "addsd",
                                   "FP add assembly should contain addsd");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_x86_64_lowers_div_mod_and_cmp_predicates(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_predicates");
    CHECK(mod != NULL, "module should be created for predicate lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64, i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "predicates",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "predicate lowering function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sd = anvil_build_sdiv(ctx, a, b, "sd");
            anvil_value_t *ud = anvil_build_udiv(ctx, a, b, "ud");
            anvil_value_t *sm = anvil_build_smod(ctx, a, b, "sm");
            anvil_value_t *um = anvil_build_umod(ctx, a, b, "um");
            anvil_value_t *lt = anvil_build_cmp_lt(ctx, a, b, "lt");
            anvil_value_t *ult = anvil_build_cmp_ult(ctx, a, b, "ult");
            anvil_value_t *acc0 = anvil_build_add(ctx, sd, ud, "acc0");
            anvil_value_t *acc1 = anvil_build_add(ctx, sm, um, "acc1");
            anvil_value_t *acc2 = anvil_build_add(ctx, acc0, acc1, "acc2");
            anvil_value_t *acc3 = anvil_build_add(ctx, acc2, lt, "acc3");
            anvil_value_t *acc4 = anvil_build_add(ctx, acc3, ult, "acc4");
            anvil_build_ret(ctx, acc4);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "predicate function should lower to MachineIR");
            if (mir) {
                bool saw_sdiv = false;
                bool saw_udiv = false;
                bool saw_smod = false;
                bool saw_umod = false;
                bool saw_cmp_lt = false;
                bool saw_cmp_ult = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "predicate MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_SDIV) saw_sdiv = true;
                    if (info.op == ANVIL_MIR_OP_UDIV) saw_udiv = true;
                    if (info.op == ANVIL_MIR_OP_SMOD) saw_smod = true;
                    if (info.op == ANVIL_MIR_OP_UMOD) saw_umod = true;
                    if (info.op == ANVIL_MIR_OP_CMP_LT) saw_cmp_lt = true;
                    if (info.op == ANVIL_MIR_OP_CMP_ULT) saw_cmp_ult = true;
                }
                CHECK(saw_sdiv, "signed division should lower to SDIV MIR opcode");
                CHECK(saw_udiv, "unsigned division should lower to UDIV MIR opcode");
                CHECK(saw_smod, "signed modulo should lower to SMOD MIR opcode");
                CHECK(saw_umod, "unsigned modulo should lower to UMOD MIR opcode");
                CHECK(saw_cmp_lt, "signed less-than should lower to CMP_LT MIR opcode");
                CHECK(saw_cmp_ult, "unsigned less-than should lower to CMP_ULT MIR opcode");

                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "predicate MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "predicate MIR should emit x86-64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "idiv",
                                   "predicate assembly should contain signed division");
                    check_contains(asm_text, "\tdiv",
                                   "predicate assembly should contain unsigned division");
                    check_contains(asm_text, "setl ",
                                   "predicate assembly should use signed setl");
                    check_contains(asm_text, "setb ",
                                   "predicate assembly should use unsigned setb");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_x86_64_lowers_select_to_branch_diamond(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_select");
    CHECK(mod != NULL, "module should be created for select lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64, i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "pick", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "select function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, a, b, "gt");
            anvil_value_t *picked = anvil_build_select(ctx, cond, a, b, "picked");
            anvil_build_ret(ctx, picked);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "select should lower to MachineIR");
            if (mir) {
                bool saw_select = false;
                bool saw_cond_branch = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "select MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_SELECT) saw_select = true;
                    if (info.op == ANVIL_MIR_OP_BR_COND) saw_cond_branch = true;
                }
                CHECK(!saw_select,
                      "select must be lowered to a branch, not a MIR SELECT");
                CHECK(saw_cond_branch,
                      "select lowering should emit a conditional branch");
                CHECK(anvil_mir_num_blocks(mir) >= 4,
                      "select lowering should introduce then/else/join blocks");

                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "select-as-branch MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "select MIR should emit x86-64 assembly");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_x86_64_lowers_stack_call_args(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_stack_call");
    CHECK(mod != NULL, "module should be created for stack call lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *callee_params[10];
        for (size_t i = 0; i < 10; i++) callee_params[i] = i64;
        anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 10, false);
        anvil_func_t *callee = anvil_func_declare(mod, "callee10", callee_type);
        CHECK(callee != NULL, "callee10 declaration should be created");

        anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
        anvil_func_t *caller = anvil_func_create(mod, "stack_call",
                                                 caller_type, ANVIL_LINK_EXTERNAL);
        CHECK(caller != NULL, "stack call function should be created");
        if (callee && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *args[10];
            for (size_t i = 0; i < 10; i++) {
                args[i] = anvil_const_i64(ctx, (int64_t)i + 1);
            }
            anvil_value_t *call = anvil_build_call(ctx, i64,
                                                   anvil_func_get_value(callee),
                                                   args, 10, "call");
            anvil_build_ret(ctx, call);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(caller);
            CHECK(mir != NULL,
                  "call with arguments beyond r9 should lower to MachineIR");
            if (mir) {
                bool saw_stack_arg0 = false;
                bool saw_stack_arg8 = false;
                bool saw_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "stack call MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) {
                        if (info.imm == 0) saw_stack_arg0 = true;
                        if (info.imm == 8) saw_stack_arg8 = true;
                    } else if (info.op == ANVIL_MIR_OP_CALL) {
                        saw_call = true;
                        CHECK(info.num_uses == 6,
                              "SysV stack call should only keep six register args");
                    }
                }
                CHECK(saw_stack_arg0,
                      "seventh integer call arg should lower to CALL_STACK_ARG offset 0");
                CHECK(saw_stack_arg8,
                      "eighth integer call arg should lower to CALL_STACK_ARG offset 8");
                CHECK(saw_call,
                      "stack call lowering should still emit direct CALL instruction");
                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "stack call MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "stack call MIR should emit x86-64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tcall callee10\n",
                                   "stack call MIR should emit direct call");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_x86_64_regalloc_handles_spill_forcing_function(void)
{
    anvil_ctx_t *ctx = new_x86_64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x64_mir_spill");
    CHECK(mod != NULL, "module should be created for spill lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[8];
        for (size_t i = 0; i < 8; i++) params[i] = i64;
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 8, false);
        anvil_func_t *fn = anvil_func_create(mod, "spiller", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "spill-forcing function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *vals[8];
            for (size_t i = 0; i < 8; i++) vals[i] = anvil_func_get_param(fn, i);
            anvil_value_t *acc = anvil_build_add(ctx, vals[0], vals[1], "a0");
            for (size_t i = 2; i < 8; i++) {
                acc = anvil_build_add(ctx, acc, vals[i], "a");
            }
            for (size_t i = 0; i < 8; i++) {
                acc = anvil_build_mul(ctx, acc, vals[i], "m");
            }
            anvil_build_ret(ctx, acc);

            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "spill-forcing function should lower to MachineIR");
            if (mir) {
                CHECK(anvil_x86_64_regalloc_mir(mir),
                      "regalloc must succeed even when operands spill");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_64_emit_mir(mir, &asm_text, &asm_len),
                      "spill-forcing MIR should emit x86-64 assembly");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_x86_64_lowers_integer_add_with_entry_abi_copies();
    test_x86_64_lowers_fp_add_uses_xmm_args();
    test_x86_64_lowers_div_mod_and_cmp_predicates();
    test_x86_64_lowers_select_to_branch_diamond();
    test_x86_64_lowers_stack_call_args();
    test_x86_64_regalloc_handles_spill_forcing_function();

    if (failures) {
        fprintf(stderr, "%d x86-64 MIR lowering test(s) failed\n", failures);
        return 1;
    }

    printf("x86-64 MIR lowering tests passed\n");
    return 0;
}
