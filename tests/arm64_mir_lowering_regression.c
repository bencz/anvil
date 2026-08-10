/*
 * Regression tests for the experimental ARM64 -> MachineIR lowering path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_arm64_mir.h>
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

static anvil_ctx_t *new_arm64_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) == ANVIL_OK,
          "ARM64 target should be available");
    return ctx;
}

static anvil_ctx_t *new_arm64_darwin_ctx(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN) == ANVIL_OK,
          "ARM64 Darwin ABI should be selectable");
    return ctx;
}

static void check_instr(const anvil_mir_func_t *mir, size_t index,
                        anvil_mir_opcode_t op, anvil_mir_vreg_t def,
                        size_t num_uses, const char *msg)
{
    anvil_mir_instr_info_t info;
    CHECK(anvil_mir_get_instr_info(mir, index, &info), msg);
    CHECK(info.op == op, "MachineIR instruction opcode should match");
    CHECK(info.def == def, "MachineIR instruction def should match");
    CHECK(info.num_uses == num_uses, "MachineIR instruction use count should match");
}

static void check_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) != NULL, msg);
}

static void test_arm64_lowers_integer_add_with_entry_abi_copies(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_iadd");
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

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "integer add should lower to MachineIR");
            if (mir) {
                CHECK(anvil_mir_num_vregs(mir) == 6,
                      "integer add MIR should have incoming arg, local arg, temp, and return vregs");
                CHECK(anvil_mir_num_instrs(mir) == 5,
                      "integer add MIR should copy ABI args, add, copy return, ret");

                anvil_mir_instr_info_t copy0_info;
                CHECK(anvil_mir_get_instr_info(mir, 0, &copy0_info),
                      "first ABI arg copy should be inspectable");
                anvil_mir_instr_info_t copy1_info;
                CHECK(anvil_mir_get_instr_info(mir, 1, &copy1_info),
                      "second ABI arg copy should be inspectable");
                anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
                anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);
                anvil_mir_vreg_t arg0 = copy0_info.def;
                anvil_mir_vreg_t arg1 = copy1_info.def;
                anvil_mir_instr_info_t add_info;
                CHECK(anvil_mir_get_instr_info(mir, 2, &add_info),
                      "add instruction should be inspectable");
                anvil_mir_vreg_t sum_vreg = add_info.def;
                anvil_mir_instr_info_t copy_info;
                CHECK(anvil_mir_get_instr_info(mir, 3, &copy_info),
                      "return copy should be inspectable");
                anvil_mir_vreg_t ret_vreg = copy_info.def;

                check_instr(mir, 0, ANVIL_MIR_OP_COPY, arg0, 1,
                            "first MIR instruction should copy x0 into a local vreg");
                check_instr(mir, 1, ANVIL_MIR_OP_COPY, arg1, 1,
                            "second MIR instruction should copy x1 into a local vreg");
                check_instr(mir, 2, ANVIL_MIR_OP_ADD, sum_vreg, 2,
                            "third MIR instruction should be add");
                check_instr(mir, 3, ANVIL_MIR_OP_COPY, ret_vreg, 1,
                            "fourth MIR instruction should copy into return vreg");
                check_instr(mir, 4, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, 1,
                            "fifth MIR instruction should be ret");

                const anvil_mir_vreg_info_t *incoming0_info =
                    anvil_mir_get_vreg_info(mir, incoming0);
                const anvil_mir_vreg_info_t *incoming1_info =
                    anvil_mir_get_vreg_info(mir, incoming1);
                const anvil_mir_vreg_info_t *arg0_info =
                    anvil_mir_get_vreg_info(mir, arg0);
                const anvil_mir_vreg_info_t *arg1_info =
                    anvil_mir_get_vreg_info(mir, arg1);
                const anvil_mir_vreg_info_t *ret_info =
                    anvil_mir_get_vreg_info(mir, ret_vreg);
                CHECK(incoming0_info && incoming0_info->reg_class == ANVIL_MIR_REG_GPR &&
                      incoming0_info->has_fixed_reg && incoming0_info->fixed_phys_reg == 0,
                      "first incoming integer argument should be fixed to x0");
                CHECK(incoming1_info && incoming1_info->reg_class == ANVIL_MIR_REG_GPR &&
                      incoming1_info->has_fixed_reg && incoming1_info->fixed_phys_reg == 1,
                      "second incoming integer argument should be fixed to x1");
                CHECK(arg0_info && arg0_info->reg_class == ANVIL_MIR_REG_GPR &&
                      !arg0_info->has_fixed_reg,
                      "first local integer argument should be allocatable");
                CHECK(arg1_info && arg1_info->reg_class == ANVIL_MIR_REG_GPR &&
                      !arg1_info->has_fixed_reg,
                      "second local integer argument should be allocatable");
                CHECK(ret_info && ret_info->reg_class == ANVIL_MIR_REG_GPR &&
                      ret_info->has_fixed_reg && ret_info->fixed_phys_reg == 0,
                      "integer return vreg should be fixed to x0");

                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should succeed for integer add");
                const anvil_regalloc_assignment_t *ret_assignment =
                    anvil_mir_get_assignment(mir, ret_vreg);
                CHECK(ret_assignment && !ret_assignment->spilled &&
                      ret_assignment->phys_reg == 0,
                      "integer return assignment should stay in x0");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_fp_add_with_entry_abi_copies(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_fadd");
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

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "FP add should lower to MachineIR");
            if (mir) {
                anvil_mir_instr_info_t copy0_info;
                CHECK(anvil_mir_get_instr_info(mir, 0, &copy0_info),
                      "first FP ABI arg copy should be inspectable");
                anvil_mir_instr_info_t copy1_info;
                CHECK(anvil_mir_get_instr_info(mir, 1, &copy1_info),
                      "second FP ABI arg copy should be inspectable");
                anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
                anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);
                anvil_mir_vreg_t arg0 = copy0_info.def;
                anvil_mir_vreg_t arg1 = copy1_info.def;
                anvil_mir_instr_info_t add_info;
                CHECK(anvil_mir_get_instr_info(mir, 2, &add_info),
                      "FP add should be inspectable");
                anvil_mir_instr_info_t copy_info;
                CHECK(anvil_mir_get_instr_info(mir, 3, &copy_info),
                      "FP return copy should be inspectable");
                anvil_mir_vreg_t ret_vreg = copy_info.def;

                check_instr(mir, 0, ANVIL_MIR_OP_COPY, arg0, 1,
                            "first FP MIR instruction should copy d0 into a local vreg");
                check_instr(mir, 1, ANVIL_MIR_OP_COPY, arg1, 1,
                            "second FP MIR instruction should copy d1 into a local vreg");
                check_instr(mir, 2, ANVIL_MIR_OP_ADD, add_info.def, 2,
                            "third FP MIR instruction should be add");

                const anvil_mir_vreg_info_t *incoming0_info =
                    anvil_mir_get_vreg_info(mir, incoming0);
                const anvil_mir_vreg_info_t *incoming1_info =
                    anvil_mir_get_vreg_info(mir, incoming1);
                const anvil_mir_vreg_info_t *arg0_info =
                    anvil_mir_get_vreg_info(mir, arg0);
                const anvil_mir_vreg_info_t *arg1_info =
                    anvil_mir_get_vreg_info(mir, arg1);
                const anvil_mir_vreg_info_t *ret_info =
                    anvil_mir_get_vreg_info(mir, ret_vreg);
                CHECK(incoming0_info && incoming0_info->reg_class == ANVIL_MIR_REG_FPR &&
                      incoming0_info->has_fixed_reg && incoming0_info->fixed_phys_reg == 0,
                      "first incoming FP argument should be fixed to d0");
                CHECK(incoming1_info && incoming1_info->reg_class == ANVIL_MIR_REG_FPR &&
                      incoming1_info->has_fixed_reg && incoming1_info->fixed_phys_reg == 1,
                      "second incoming FP argument should be fixed to d1");
                CHECK(arg0_info && arg0_info->reg_class == ANVIL_MIR_REG_FPR &&
                      !arg0_info->has_fixed_reg,
                      "first local FP argument should be allocatable");
                CHECK(arg1_info && arg1_info->reg_class == ANVIL_MIR_REG_FPR &&
                      !arg1_info->has_fixed_reg,
                      "second local FP argument should be allocatable");
                CHECK(ret_info && ret_info->reg_class == ANVIL_MIR_REG_FPR &&
                      ret_info->has_fixed_reg && ret_info->fixed_phys_reg == 0,
                      "FP return vreg should be fixed to d0");

                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should succeed for FP add");
                const anvil_regalloc_assignment_t *ret_assignment =
                    anvil_mir_get_assignment(mir, ret_vreg);
                CHECK(ret_assignment && ret_assignment->reg_class == ANVIL_MIR_REG_FPR &&
                      !ret_assignment->spilled && ret_assignment->phys_reg == 0,
                      "FP return assignment should stay in d0");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_constants_bitwise_and_mixed_call_abi(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_call");
    CHECK(mod != NULL, "module should be created for call lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *callee_params[] = { i64, f64 };
        anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 2, false);
        anvil_func_t *callee = anvil_func_declare(mod, "callee", callee_type);
        CHECK(callee != NULL, "callee declaration should be created");

        anvil_type_t *caller_params[] = { i64, f64 };
        anvil_type_t *caller_type = anvil_type_func(ctx, i64, caller_params, 2, false);
        anvil_func_t *caller = anvil_func_create(mod, "caller", caller_type,
                                                 ANVIL_LINK_EXTERNAL);
        CHECK(caller != NULL, "caller function should be created");
        if (callee && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *x = anvil_func_get_param(caller, 0);
            anvil_value_t *y = anvil_func_get_param(caller, 1);
            anvil_value_t *masked = anvil_build_and(ctx, x, anvil_const_i64(ctx, 255),
                                                    "masked");
            anvil_value_t *args[] = { masked, y };
            anvil_value_t *call = anvil_build_call(ctx, callee_type,
                                                   anvil_func_get_value(callee),
                                                   args, 2, "call");
            anvil_build_ret(ctx, call);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
            CHECK(mir != NULL, "mixed call should lower to MachineIR");
            if (mir) {
            CHECK(anvil_mir_num_instrs(mir) == 10,
                  "mixed call MIR should contain entry copies, const, and, arg copies, call, result copy, return");

            anvil_mir_instr_info_t imm_info;
            CHECK(anvil_mir_get_instr_info(mir, 2, &imm_info),
                  "constant move should be inspectable");
                CHECK(imm_info.op == ANVIL_MIR_OP_MOV && imm_info.has_imm &&
                      imm_info.imm == 255,
                      "integer constant should lower to MOV immediate");

            anvil_mir_instr_info_t and_info;
            CHECK(anvil_mir_get_instr_info(mir, 3, &and_info),
                  "bitwise AND should be inspectable");
                CHECK(and_info.op == ANVIL_MIR_OP_AND && and_info.num_uses == 2,
                      "bitwise AND should lower to MIR AND");

            anvil_mir_instr_info_t call_info;
            CHECK(anvil_mir_get_instr_info(mir, 6, &call_info),
                  "call should be inspectable");
                CHECK(call_info.op == ANVIL_MIR_OP_CALL &&
                      call_info.symbol && strcmp(call_info.symbol, "callee") == 0 &&
                      call_info.num_uses == 2,
                      "direct call should carry symbol and ABI arg uses");

            anvil_mir_vreg_t call_arg0 = anvil_mir_get_instr_use(mir, 6, 0);
            anvil_mir_vreg_t call_arg1 = anvil_mir_get_instr_use(mir, 6, 1);
                const anvil_mir_vreg_info_t *arg0_info =
                    anvil_mir_get_vreg_info(mir, call_arg0);
                const anvil_mir_vreg_info_t *arg1_info =
                    anvil_mir_get_vreg_info(mir, call_arg1);
                CHECK(arg0_info && arg0_info->reg_class == ANVIL_MIR_REG_GPR &&
                      arg0_info->has_fixed_reg && arg0_info->fixed_phys_reg == 0,
                      "first call arg should be copied to x0");
                CHECK(arg1_info && arg1_info->reg_class == ANVIL_MIR_REG_FPR &&
                      arg1_info->has_fixed_reg && arg1_info->fixed_phys_reg == 0,
                      "first FP call arg should be copied to d0");

            CHECK(anvil_arm64_regalloc_mir(mir),
                  "ARM64 MachineIR regalloc should succeed for mixed call");
            const anvil_regalloc_assignment_t *fixed_call_assignment =
                anvil_mir_get_assignment(mir, call_info.def);
            CHECK(fixed_call_assignment && !fixed_call_assignment->spilled &&
                  fixed_call_assignment->reg_class == ANVIL_MIR_REG_GPR &&
                  fixed_call_assignment->phys_reg == 0,
                  "fixed integer call result should arrive in x0");

            anvil_mir_instr_info_t result_copy_info;
            CHECK(anvil_mir_get_instr_info(mir, 7, &result_copy_info),
                  "call result copy should be inspectable");
            CHECK(result_copy_info.op == ANVIL_MIR_OP_COPY &&
                  result_copy_info.num_uses == 1 &&
                  anvil_mir_get_instr_use(mir, 7, 0) == call_info.def,
                  "call result should be copied out of x0 after the call");
            const anvil_mir_vreg_info_t *local_call_result_info =
                anvil_mir_get_vreg_info(mir, result_copy_info.def);
            const anvil_regalloc_assignment_t *local_call_assignment =
                anvil_mir_get_assignment(mir, result_copy_info.def);
            CHECK(local_call_result_info &&
                  local_call_result_info->reg_class == ANVIL_MIR_REG_GPR &&
                  !local_call_result_info->has_fixed_reg,
                  "local call result should be allocatable after ABI copy-out");
            CHECK(local_call_assignment && !local_call_assignment->spilled &&
                  local_call_assignment->reg_class == ANVIL_MIR_REG_GPR &&
                  local_call_assignment->phys_reg != 0,
                  "local call result should not remain tied to x0");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_stack_call_args_to_machineir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_stack_call");
    CHECK(mod != NULL, "module should be created for stack call lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *callee_params[10];
        for (size_t i = 0; i < 10; i++) {
            callee_params[i] = i64;
        }
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

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
            CHECK(mir != NULL,
                  "call with arguments beyond x7 should lower to MachineIR");
            if (mir) {
                bool saw_stack_arg0 = false;
                bool saw_stack_arg8 = false;
                bool saw_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "stack call MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) {
                        CHECK(info.has_imm && info.num_uses == 1,
                              "stack arg MIR instruction should carry offset and one source");
                        if (info.imm == 0) saw_stack_arg0 = true;
                        if (info.imm == 8) saw_stack_arg8 = true;
                    } else if (info.op == ANVIL_MIR_OP_CALL) {
                        saw_call = true;
                        CHECK(info.num_uses == 8,
                              "stack call MIR call should only keep register arguments as call uses");
                    }
                }
                CHECK(saw_stack_arg0,
                      "ninth integer call arg should lower to CALL_STACK_ARG offset 0");
                CHECK(saw_stack_arg8,
                      "tenth integer call arg should lower to CALL_STACK_ARG offset 8");
                CHECK(saw_call,
                      "stack call lowering should still emit direct CALL instruction");
                CHECK(anvil_arm64_regalloc_mir(mir),
                      "stack call MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_arm64_emit_mir(mir, &asm_text, &asm_len),
                      "stack call MIR should emit ARM64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "[sp, #0]\n",
                                   "ninth integer call arg should be stored at stack offset 0");
                    check_contains(asm_text, "[sp, #8]\n",
                                   "tenth integer call arg should be stored at stack offset 8");
                    check_contains(asm_text, "\tbl callee10\n",
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

static void test_arm64_lowers_fp_stack_call_args_to_machineir(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_fp_stack_call");
    CHECK(mod != NULL, "module should be created for FP stack call lowering");
    if (mod) {
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *callee_params[10];
        for (size_t i = 0; i < 10; i++) {
            callee_params[i] = f64;
        }
        anvil_type_t *callee_type = anvil_type_func(ctx, f64, callee_params, 10, false);
        anvil_func_t *callee = anvil_func_declare(mod, "fp_callee10", callee_type);
        CHECK(callee != NULL, "fp_callee10 declaration should be created");

        anvil_type_t *caller_type = anvil_type_func(ctx, f64, NULL, 0, false);
        anvil_func_t *caller = anvil_func_create(mod, "fp_stack_call",
                                                 caller_type, ANVIL_LINK_EXTERNAL);
        CHECK(caller != NULL, "FP stack call function should be created");
        if (callee && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *args[10];
            for (size_t i = 0; i < 10; i++) {
                args[i] = anvil_const_f64(ctx, (double)i + 0.5);
            }
            anvil_value_t *call = anvil_build_call(ctx, f64,
                                                   anvil_func_get_value(callee),
                                                   args, 10, "call");
            anvil_build_ret(ctx, call);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
            CHECK(mir != NULL,
                  "FP call with arguments beyond d7 should lower to MachineIR");
            if (mir) {
                size_t stack_arg_count = 0;
                bool saw_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "FP stack call MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) {
                        stack_arg_count++;
                        anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
                        const anvil_mir_vreg_info_t *src_info =
                            anvil_mir_get_vreg_info(mir, src);
                        CHECK(src_info && src_info->reg_class == ANVIL_MIR_REG_FPR,
                              "FP stack arg source should stay in FPR class");
                    } else if (info.op == ANVIL_MIR_OP_CALL) {
                        saw_call = true;
                        CHECK(info.num_uses == 8,
                              "FP stack call should only keep d0-d7 as call uses");
                    }
                }
                CHECK(stack_arg_count == 2,
                      "FP stack call should lower exactly two stack arguments");
                CHECK(saw_call, "FP stack call lowering should emit CALL instruction");

                CHECK(anvil_arm64_regalloc_mir(mir),
                      "FP stack call MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_arm64_emit_mir(mir, &asm_text, &asm_len),
                      "FP stack call MIR should emit ARM64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tstr d",
                                   "FP stack call assembly should store FPR stack args");
                    check_contains(asm_text, "[sp, #0]\n",
                                   "ninth FP call arg should use stack offset 0");
                    check_contains(asm_text, "[sp, #8]\n",
                                   "tenth FP call arg should use stack offset 8");
                    check_contains(asm_text, "\tbl fp_callee10\n",
                                   "FP stack call MIR should emit direct call");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_darwin_variadic_args_to_stack_machineir(void)
{
    anvil_ctx_t *ctx = new_arm64_darwin_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_darwin_vararg");
    CHECK(mod != NULL, "module should be created for Darwin variadic lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *callee_params[] = { i64 };
        anvil_type_t *callee_type = anvil_type_func(ctx, i64, callee_params, 1, true);
        anvil_func_t *callee = anvil_func_declare(mod, "darwin_vararg", callee_type);
        CHECK(callee != NULL, "Darwin variadic callee declaration should be created");

        anvil_type_t *caller_type = anvil_type_func(ctx, i64, NULL, 0, false);
        anvil_func_t *caller = anvil_func_create(mod, "darwin_vararg_call",
                                                 caller_type, ANVIL_LINK_EXTERNAL);
        CHECK(caller != NULL, "Darwin variadic caller should be created");
        if (callee && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *args[] = {
                anvil_const_i64(ctx, 10),
                anvil_const_i64(ctx, 20),
                anvil_const_i64(ctx, 30)
            };
            anvil_value_t *call = anvil_build_call(ctx, i64,
                                                   anvil_func_get_value(callee),
                                                   args, 3, "call");
            anvil_build_ret(ctx, call);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(caller);
            CHECK(mir != NULL,
                  "Darwin variadic call should lower to MachineIR");
            if (mir) {
                size_t stack_arg_count = 0;
                bool saw_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "Darwin variadic MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) {
                        stack_arg_count++;
                        CHECK(info.has_imm && info.num_uses == 1,
                              "Darwin variadic stack arg should carry offset and source");
                    } else if (info.op == ANVIL_MIR_OP_CALL) {
                        saw_call = true;
                        CHECK(info.num_uses == 1,
                              "Darwin variadic MIR call should keep only fixed arg in register uses");
                    }
                }
                CHECK(stack_arg_count == 2,
                      "Darwin variadic lowering should put variadic args on stack");
                CHECK(saw_call,
                      "Darwin variadic lowering should still emit direct CALL");

                CHECK(anvil_arm64_regalloc_mir(mir),
                      "Darwin variadic MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_arm64_emit_mir_abi(mir, ANVIL_ABI_DARWIN,
                                               &asm_text, &asm_len),
                      "Darwin variadic MIR should emit Darwin ARM64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "[sp, #0]\n",
                                   "first Darwin variadic arg should use stack offset 0");
                    check_contains(asm_text, "[sp, #8]\n",
                                   "second Darwin variadic arg should use stack offset 8");
                    check_contains(asm_text, "\tbl _darwin_vararg\n",
                                   "Darwin variadic MIR should emit underscored call");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_div_mod_and_cmp_predicates_without_losing_semantics(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_predicates");
    CHECK(mod != NULL, "module should be created for predicate lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *u64 = anvil_type_u64(ctx);
        anvil_type_t *params[] = { i64, i64, u64, u64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 4, false);
        anvil_func_t *fn = anvil_func_create(mod, "predicates",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "predicate lowering function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *ua = anvil_func_get_param(fn, 2);
            anvil_value_t *ub = anvil_func_get_param(fn, 3);
            anvil_value_t *sd = anvil_build_sdiv(ctx, a, b, "sd");
            anvil_value_t *ud = anvil_build_udiv(ctx, ua, ub, "ud");
            anvil_value_t *sm = anvil_build_smod(ctx, a, b, "sm");
            anvil_value_t *um = anvil_build_umod(ctx, ua, ub, "um");
            anvil_value_t *lt = anvil_build_cmp_lt(ctx, a, b, "lt");
            anvil_value_t *ult = anvil_build_cmp_ult(ctx, ua, ub, "ult");
            anvil_value_t *ud_i = anvil_build_bitcast(ctx, ud, i64, "ud_i");
            anvil_value_t *um_i = anvil_build_bitcast(ctx, um, i64, "um_i");
            anvil_value_t *lt_i = anvil_build_zext(ctx, lt, i64, "lt_i");
            anvil_value_t *ult_i = anvil_build_zext(ctx, ult, i64, "ult_i");
            anvil_value_t *acc0 = anvil_build_add(ctx, sd, ud_i, "acc0");
            anvil_value_t *acc1 = anvil_build_add(ctx, sm, um_i, "acc1");
            anvil_value_t *acc2 = anvil_build_add(ctx, acc0, acc1, "acc2");
            anvil_value_t *acc3 = anvil_build_add(ctx, acc2, lt_i, "acc3");
            anvil_value_t *acc4 = anvil_build_add(ctx, acc3, ult_i, "acc4");
            anvil_build_ret(ctx, acc4);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
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

                CHECK(anvil_arm64_regalloc_mir(mir),
                      "predicate MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_arm64_emit_mir(mir, &asm_text, &asm_len),
                      "predicate MIR should emit ARM64 assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tsdiv ",
                                   "predicate assembly should contain signed division");
                    check_contains(asm_text, "\tudiv ",
                                   "predicate assembly should contain unsigned division");
                    check_contains(asm_text, "\tcset ",
                                   "predicate assembly should materialize comparisons");
                    check_contains(asm_text, " lt\n",
                                   "predicate assembly should use signed lt condition");
                    check_contains(asm_text, " lo\n",
                                   "predicate assembly should use unsigned lo condition");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_if_else_branches_to_mir_blocks(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_ifelse");
    CHECK(mod != NULL, "module should be created for if/else lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "select_sign", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "if/else test function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *then_block = anvil_block_create(fn, "then");
            anvil_block_t *else_block = anvil_block_create(fn, "else");
            CHECK(entry && then_block && else_block,
                  "if/else blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_value_t *x = anvil_func_get_param(fn, 0);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, x, anvil_const_i64(ctx, 0),
                                                     "is_pos");
            anvil_build_br_cond(ctx, cond, then_block, else_block);

            anvil_set_insert_point(ctx, then_block);
            anvil_build_ret(ctx, anvil_const_i64(ctx, 1));

            anvil_set_insert_point(ctx, else_block);
            anvil_build_ret(ctx, anvil_const_i64(ctx, -1));

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "if/else should lower to MachineIR blocks");
            if (mir) {
                CHECK(anvil_mir_num_blocks(mir) == 3,
                      "if/else MIR should have entry, then, and else blocks");

                anvil_mir_block_t mir_entry = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_then = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_else = ANVIL_MIR_NO_BLOCK;
                for (size_t i = 0; i < anvil_mir_num_blocks(mir); i++) {
                    anvil_mir_block_info_t info;
                    CHECK(anvil_mir_get_block_info(mir, (anvil_mir_block_t)i, &info),
                          "MIR block should be inspectable");
                    if (info.name && strcmp(info.name, "entry") == 0) {
                        mir_entry = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "then") == 0) {
                        mir_then = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "else") == 0) {
                        mir_else = (anvil_mir_block_t)i;
                    }
                }
                CHECK(mir_entry != ANVIL_MIR_NO_BLOCK &&
                      mir_then != ANVIL_MIR_NO_BLOCK &&
                      mir_else != ANVIL_MIR_NO_BLOCK,
                      "lowered MIR should preserve source block names");

                bool found_cond_branch = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_BR_COND) {
                        found_cond_branch = true;
                        CHECK(info.block == mir_entry &&
                              info.true_block == mir_then &&
                              info.false_block == mir_else &&
                              info.num_uses == 1,
                              "conditional branch should target then/else MIR blocks");
                    }
                }
                CHECK(found_cond_branch,
                      "if/else lowering should emit a conditional MIR branch");
                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should handle lowered if/else");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_phi_as_predecessor_edge_copies(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_phi");
    CHECK(mod != NULL, "module should be created for PHI lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "phi_join", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "PHI test function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *then_block = anvil_block_create(fn, "then");
            anvil_block_t *else_block = anvil_block_create(fn, "else");
            anvil_block_t *merge_block = anvil_block_create(fn, "merge");
            CHECK(entry && then_block && else_block && merge_block,
                  "PHI test blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_value_t *x = anvil_func_get_param(fn, 0);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, x, anvil_const_i64(ctx, 0),
                                                     "is_pos");
            anvil_build_br_cond(ctx, cond, then_block, else_block);

            anvil_set_insert_point(ctx, then_block);
            anvil_build_br(ctx, merge_block);

            anvil_set_insert_point(ctx, else_block);
            anvil_build_br(ctx, merge_block);

            anvil_set_insert_point(ctx, merge_block);
            anvil_value_t *phi = anvil_build_phi(ctx, i64, "p");
            anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 11), then_block);
            anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 22), else_block);
            anvil_build_ret(ctx, phi);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL, "PHI join should lower to MachineIR");
            if (mir) {
                anvil_mir_block_t mir_then = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_else = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_merge = ANVIL_MIR_NO_BLOCK;
                for (size_t i = 0; i < anvil_mir_num_blocks(mir); i++) {
                    anvil_mir_block_info_t info;
                    CHECK(anvil_mir_get_block_info(mir, (anvil_mir_block_t)i, &info),
                          "PHI MIR block should be inspectable");
                    if (info.name && strcmp(info.name, "then") == 0) {
                        mir_then = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "else") == 0) {
                        mir_else = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "merge") == 0) {
                        mir_merge = (anvil_mir_block_t)i;
                    }
                }

                anvil_mir_vreg_t phi_vreg = ANVIL_MIR_NO_VREG;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PHI MIR instruction should be inspectable");
                    if (info.block == mir_merge && info.op == ANVIL_MIR_OP_COPY) {
                        phi_vreg = anvil_mir_get_instr_use(mir, i, 0);
                        break;
                    }
                }
                CHECK(phi_vreg != ANVIL_MIR_NO_VREG,
                      "merge return should consume the lowered PHI vreg");

                bool then_has_phi_copy = false;
                bool else_has_phi_copy = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PHI copy instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_COPY && info.def == phi_vreg) {
                        if (info.block == mir_then) then_has_phi_copy = true;
                        if (info.block == mir_else) else_has_phi_copy = true;
                    }
                }
                CHECK(then_has_phi_copy,
                      "then predecessor should copy incoming value into PHI vreg");
                CHECK(else_has_phi_copy,
                      "else predecessor should copy incoming value into PHI vreg");
                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should handle lowered PHI copies");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_splits_conditional_phi_edges(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_cond_phi");
    CHECK(mod != NULL, "module should be created for conditional PHI lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "cond_phi_direct",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "conditional PHI test function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *positive = anvil_block_create(fn, "positive");
            anvil_block_t *merge = anvil_block_create(fn, "merge");
            CHECK(entry && positive && merge,
                  "conditional PHI test blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_value_t *x = anvil_func_get_param(fn, 0);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, x, anvil_const_i64(ctx, 0),
                                                     "is_pos");
            anvil_build_br_cond(ctx, cond, positive, merge);

            anvil_set_insert_point(ctx, positive);
            anvil_build_br(ctx, merge);

            anvil_set_insert_point(ctx, merge);
            anvil_value_t *phi = anvil_build_phi(ctx, i64, "p");
            anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 1), positive);
            anvil_phi_add_incoming(phi, anvil_const_i64(ctx, 0), entry);
            anvil_build_ret(ctx, phi);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL,
                  "conditional branch into PHI block should lower with an edge block");
            if (mir) {
                CHECK(anvil_mir_num_blocks(mir) == 4,
                      "conditional PHI lowering should insert one edge block");

                anvil_mir_block_t mir_entry = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_positive = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_merge = ANVIL_MIR_NO_BLOCK;
                for (size_t i = 0; i < anvil_mir_num_blocks(mir); i++) {
                    anvil_mir_block_info_t info;
                    CHECK(anvil_mir_get_block_info(mir, (anvil_mir_block_t)i, &info),
                          "conditional PHI MIR block should be inspectable");
                    if (info.name && strcmp(info.name, "entry") == 0) {
                        mir_entry = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "positive") == 0) {
                        mir_positive = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "merge") == 0) {
                        mir_merge = (anvil_mir_block_t)i;
                    }
                }
                CHECK(mir_entry != ANVIL_MIR_NO_BLOCK &&
                      mir_positive != ANVIL_MIR_NO_BLOCK &&
                      mir_merge != ANVIL_MIR_NO_BLOCK,
                      "conditional PHI lowering should preserve source block names");

                anvil_mir_vreg_t phi_vreg = ANVIL_MIR_NO_VREG;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "conditional PHI MIR instruction should be inspectable");
                    if (info.block == mir_merge && info.op == ANVIL_MIR_OP_COPY) {
                        phi_vreg = anvil_mir_get_instr_use(mir, i, 0);
                        break;
                    }
                }
                CHECK(phi_vreg != ANVIL_MIR_NO_VREG,
                      "conditional PHI merge return should consume PHI vreg");

                anvil_mir_block_t edge_block = ANVIL_MIR_NO_BLOCK;
                bool found_entry_cond = false;
                bool edge_has_phi_copy = false;
                bool edge_branches_to_merge = false;
                bool positive_has_phi_copy = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "conditional PHI MIR edge instruction should be inspectable");
                    if (info.block == mir_entry && info.op == ANVIL_MIR_OP_BR_COND) {
                        found_entry_cond = true;
                        CHECK(info.true_block == mir_positive,
                              "true edge without PHI should still target positive block");
                        CHECK(info.false_block != mir_merge &&
                              info.false_block != ANVIL_MIR_NO_BLOCK,
                              "false edge into PHI should target inserted edge block");
                        edge_block = info.false_block;
                    } else if (info.block == mir_positive &&
                               info.op == ANVIL_MIR_OP_COPY &&
                               info.def == phi_vreg) {
                        positive_has_phi_copy = true;
                    }
                }

                CHECK(found_entry_cond,
                      "conditional PHI lowering should keep conditional branch");
                CHECK(edge_block != ANVIL_MIR_NO_BLOCK,
                      "conditional PHI lowering should expose inserted edge block");

                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "inserted edge block instruction should be inspectable");
                    if (info.block == edge_block && info.op == ANVIL_MIR_OP_COPY &&
                        info.def == phi_vreg) {
                        edge_has_phi_copy = true;
                    } else if (info.block == edge_block && info.op == ANVIL_MIR_OP_BR) {
                        edge_branches_to_merge = info.true_block == mir_merge;
                    }
                }

                CHECK(edge_has_phi_copy,
                      "inserted conditional edge block should copy PHI incoming value");
                CHECK(edge_branches_to_merge,
                      "inserted conditional edge block should branch to PHI merge");
                CHECK(positive_has_phi_copy,
                      "ordinary predecessor should still copy into PHI vreg");
                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should handle conditional PHI edge block");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_lowers_phi_swap_with_parallel_edge_copy(void)
{
    anvil_ctx_t *ctx = new_arm64_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "arm64_mir_phi_swap");
    CHECK(mod != NULL, "module should be created for PHI swap lowering");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "phi_swap",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "PHI swap test function should be created");
        if (fn) {
            anvil_block_t *entry = anvil_func_get_entry(fn);
            anvil_block_t *header = anvil_block_create(fn, "header");
            anvil_block_t *body = anvil_block_create(fn, "body");
            anvil_block_t *exit_block = anvil_block_create(fn, "exit");
            CHECK(entry && header && body && exit_block,
                  "PHI swap test blocks should be created");

            anvil_set_insert_point(ctx, entry);
            anvil_build_br(ctx, header);

            anvil_set_insert_point(ctx, header);
            anvil_value_t *a = anvil_build_phi(ctx, i64, "a");
            anvil_value_t *b = anvil_build_phi(ctx, i64, "b");
            anvil_phi_add_incoming(a, anvil_const_i64(ctx, 1), entry);
            anvil_phi_add_incoming(a, b, body);
            anvil_phi_add_incoming(b, anvil_const_i64(ctx, 2), entry);
            anvil_phi_add_incoming(b, a, body);
            anvil_value_t *x = anvil_func_get_param(fn, 0);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, x, anvil_const_i64(ctx, 0),
                                                     "again");
            anvil_build_br_cond(ctx, cond, body, exit_block);

            anvil_set_insert_point(ctx, body);
            anvil_build_br(ctx, header);

            anvil_set_insert_point(ctx, exit_block);
            anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(fn);
            CHECK(mir != NULL,
                  "PHI swap should lower with cycle-safe parallel copies");
            if (mir) {
                anvil_mir_block_t mir_body = ANVIL_MIR_NO_BLOCK;
                anvil_mir_block_t mir_exit = ANVIL_MIR_NO_BLOCK;
                for (size_t i = 0; i < anvil_mir_num_blocks(mir); i++) {
                    anvil_mir_block_info_t info;
                    CHECK(anvil_mir_get_block_info(mir, (anvil_mir_block_t)i, &info),
                          "PHI swap MIR block should be inspectable");
                    if (info.name && strcmp(info.name, "body") == 0) {
                        mir_body = (anvil_mir_block_t)i;
                    } else if (info.name && strcmp(info.name, "exit") == 0) {
                        mir_exit = (anvil_mir_block_t)i;
                    }
                }

                anvil_mir_vreg_t a_vreg = ANVIL_MIR_NO_VREG;
                anvil_mir_vreg_t b_vreg = ANVIL_MIR_NO_VREG;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PHI swap MIR instruction should be inspectable");
                    if (info.block == mir_exit && info.op == ANVIL_MIR_OP_ADD &&
                        info.num_uses == 2) {
                        a_vreg = anvil_mir_get_instr_use(mir, i, 0);
                        b_vreg = anvil_mir_get_instr_use(mir, i, 1);
                        break;
                    }
                }
                CHECK(a_vreg != ANVIL_MIR_NO_VREG &&
                      b_vreg != ANVIL_MIR_NO_VREG &&
                      a_vreg != b_vreg,
                      "exit add should consume both PHI vregs");

                bool saved_temp = false;
                bool copied_to_a = false;
                bool copied_to_b = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PHI swap copy should be inspectable");
                    if (info.block != mir_body || info.op != ANVIL_MIR_OP_COPY ||
                        info.num_uses != 1) {
                        continue;
                    }

                    anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
                    if (info.def == a_vreg) {
                        copied_to_a = true;
                    } else if (info.def == b_vreg) {
                        copied_to_b = true;
                    } else if (src == a_vreg || src == b_vreg) {
                        saved_temp = true;
                    }
                }

                CHECK(saved_temp,
                      "PHI swap should save one original PHI value into a temporary");
                CHECK(copied_to_a && copied_to_b,
                      "PHI swap should copy both destinations on the backedge");
                CHECK(anvil_arm64_regalloc_mir(mir),
                      "ARM64 MachineIR regalloc should handle PHI swap copies");

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_arm64_regalloc_materializes_machineir_spills(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("arm64_mir_spill_pressure");
    CHECK(mir != NULL, "ARM64 spill pressure MIR function should be created");
    if (!mir) return;

    enum { NUM_VALUES = 20 };
    anvil_mir_vreg_t values[NUM_VALUES];
    for (size_t i = 0; i < NUM_VALUES; i++) {
        values[i] = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
        CHECK(values[i] != ANVIL_MIR_NO_VREG,
              "ARM64 spill pressure vreg should be created");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_MOV, values[i], NULL, 0),
              "ARM64 spill pressure value should be defined");
    }

    for (size_t i = 0; i < NUM_VALUES; i++) {
        anvil_mir_vreg_t uses[] = { values[i] };
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_KEEPALIVE,
                                  ANVIL_MIR_NO_VREG, uses, 1),
              "ARM64 spill pressure value should stay live until use");
    }
    anvil_mir_vreg_t ret_uses[] = { values[0] };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "ARM64 spill pressure function should terminate");

    CHECK(anvil_arm64_regalloc_mir(mir),
          "ARM64 MIR regalloc should handle and materialize spills");
    CHECK(anvil_mir_num_spills(mir) > 0,
          "ARM64 spill pressure should force spill slots");

    bool saw_spill_load = false;
    bool saw_spill_store = false;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        CHECK(anvil_mir_get_instr_info(mir, i, &info),
              "ARM64 materialized spill instruction should be inspectable");
        if (info.op == ANVIL_MIR_OP_SPILL_LOAD) {
            saw_spill_load = true;
            const anvil_regalloc_assignment_t *assignment =
                anvil_mir_get_assignment(mir, info.def);
            CHECK(assignment && !assignment->spilled &&
                  assignment->reg_class == ANVIL_MIR_REG_GPR &&
                  assignment->phys_reg >= 12 && assignment->phys_reg <= 15,
                  "ARM64 spill load temp should use reserved scratch GPR");
        } else if (info.op == ANVIL_MIR_OP_SPILL_STORE) {
            saw_spill_store = true;
            anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
            const anvil_regalloc_assignment_t *assignment =
                anvil_mir_get_assignment(mir, src);
            CHECK(assignment && !assignment->spilled &&
                  assignment->reg_class == ANVIL_MIR_REG_GPR &&
                  assignment->phys_reg >= 12 && assignment->phys_reg <= 15,
                  "ARM64 spill store temp should use reserved scratch GPR");
        }
    }

    CHECK(saw_spill_load, "ARM64 regalloc should emit spill loads");
    CHECK(saw_spill_store, "ARM64 regalloc should emit spill stores");

    anvil_mir_func_destroy(mir);
}

static void test_arm64_emits_allocated_mir_with_integer_fp_memory_call_and_cfg(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("mir_emit_full");
    CHECK(mir != NULL, "ARM64 MIR emitter test function should be created");
    if (!mir) return;

    anvil_mir_block_t entry = anvil_mir_current_block(mir);
    anvil_mir_block_t then_block = anvil_mir_add_block(mir, "then");
    anvil_mir_block_t else_block = anvil_mir_add_block(mir, "else");
    anvil_mir_block_t exit_block = anvil_mir_add_block(mir, "exit");
    CHECK(entry != ANVIL_MIR_NO_BLOCK &&
          then_block != ANVIL_MIR_NO_BLOCK &&
          else_block != ANVIL_MIR_NO_BLOCK &&
          exit_block != ANVIL_MIR_NO_BLOCK,
          "ARM64 MIR emitter CFG blocks should be created");

    anvil_mir_vreg_t x0 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t x1 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t d0 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t d1 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);

    anvil_mir_vreg_t imm = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t diff = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t prod = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sdiv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t udiv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t smod = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t umod = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t andv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t orv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t xorv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t shlv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t shrv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sarv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t negv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t notv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t cmp = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t fadd = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t fsub = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t fmul = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t fdiv = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t fneg = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);

    CHECK(anvil_mir_set_current_block(mir, entry), "entry should be selected");
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, x0, 1) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, x1, 2) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, d0, 0) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, d1, 0),
          "manual emitter operands should have explicit definitions");
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, imm, 42),
          "emitter test should add MOV immediate");
    anvil_mir_vreg_t uses2[] = { x0, imm };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, sum, uses2, 2),
          "emitter test should add integer ADD");
    anvil_mir_vreg_t sum_x1[] = { sum, x1 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SUB, diff, sum_x1, 2),
          "emitter test should add integer SUB");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_MUL, prod, sum_x1, 2),
          "emitter test should add integer MUL");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SDIV, sdiv, sum_x1, 2),
          "emitter test should add signed DIV");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_UDIV, udiv, sum_x1, 2),
          "emitter test should add unsigned DIV");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SMOD, smod, sum_x1, 2),
          "emitter test should add signed MOD");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_UMOD, umod, sum_x1, 2),
          "emitter test should add unsigned MOD");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_AND, andv, sum_x1, 2),
          "emitter test should add AND");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_OR, orv, sum_x1, 2),
          "emitter test should add OR");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_XOR, xorv, sum_x1, 2),
          "emitter test should add XOR");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SHL, shlv, sum_x1, 2),
          "emitter test should add SHL");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SHR, shrv, sum_x1, 2),
          "emitter test should add SHR");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SAR, sarv, sum_x1, 2),
          "emitter test should add SAR");
    anvil_mir_vreg_t one_use[] = { sum };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_NEG, negv, one_use, 1),
          "emitter test should add NEG");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_NOT, notv, one_use, 1),
          "emitter test should add NOT");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_CMP_GT, cmp, sum_x1, 2),
          "emitter test should add predicate CMP");

    anvil_mir_vreg_t fuses[] = { d0, d1 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, fadd, fuses, 2),
          "emitter test should add FP ADD");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SUB, fsub, fuses, 2),
          "emitter test should add FP SUB");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_MUL, fmul, fuses, 2),
          "emitter test should add FP MUL");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_FDIV, fdiv, fuses, 2),
          "emitter test should add FP DIV");
    anvil_mir_vreg_t fone[] = { d0 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_NEG, fneg, fone, 1),
          "emitter test should add FP NEG");
    CHECK(anvil_mir_add_cond_branch(mir, cmp, then_block, else_block),
          "emitter test should add conditional branch");

    CHECK(anvil_mir_set_current_block(mir, then_block), "then should be selected");
    anvil_mir_vreg_t call_arg0 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t call_arg1 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t call_ret = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    CHECK(anvil_mir_set_fixed_reg(mir, call_arg0, 0), "call integer arg fixed");
    CHECK(anvil_mir_set_fixed_reg(mir, call_arg1, 0), "call FP arg fixed");
    CHECK(anvil_mir_set_fixed_reg(mir, call_ret, 0), "call return fixed");
    anvil_mir_vreg_t call_arg0_use[] = { sum };
    anvil_mir_vreg_t call_arg1_use[] = { fadd };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, call_arg0,
                              call_arg0_use, 1),
          "emitter test should copy integer call arg");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, call_arg1,
                              call_arg1_use, 1),
          "emitter test should copy FP call arg");
    anvil_mir_vreg_t call_uses[] = { call_arg0, call_arg1 };
    CHECK(anvil_mir_add_instr_symbol(mir, ANVIL_MIR_OP_CALL, call_ret,
                                     call_uses, 2, "callee"),
          "emitter test should add call");
    CHECK(anvil_mir_add_branch(mir, exit_block),
          "emitter test should branch from then to exit");

    CHECK(anvil_mir_set_current_block(mir, else_block), "else should be selected");
    anvil_mir_vreg_t loaded = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t ptr_use[] = { x0 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, loaded, ptr_use, 1),
          "emitter test should add load");
    anvil_mir_vreg_t store_uses[] = { loaded, x1 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG,
                              store_uses, 2),
          "emitter test should add store");
    CHECK(anvil_mir_add_branch(mir, exit_block),
          "emitter test should branch from else to exit");

    CHECK(anvil_mir_set_current_block(mir, exit_block), "exit should be selected");
    anvil_mir_vreg_t ret = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    CHECK(anvil_mir_set_fixed_reg(mir, ret, 0), "return vreg should be fixed");
    anvil_mir_vreg_t ret_use[] = { x0 };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_COPY, ret, ret_use, 1),
          "emitter test should copy return value");
    anvil_mir_vreg_t ret_uses[] = { ret };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "emitter test should add ret");

    char mir_error[256] = { 0 };
    bool mir_legal = anvil_arm64_verify_mir_legal(
        mir, mir_error, sizeof(mir_error));
    if (!mir_legal) fprintf(stderr, "[DIAG] %s\n", mir_error);
    CHECK(mir_legal, "manual ARM64 emitter MIR should verify before regalloc");
    CHECK(anvil_arm64_regalloc_mir(mir),
          "ARM64 MIR emitter test should allocate registers");

    char *asm_text = NULL;
    size_t asm_len = 0;
    CHECK(anvil_arm64_emit_mir(mir, &asm_text, &asm_len),
          "ARM64 MIR emitter should produce assembly");
    CHECK(asm_text != NULL && asm_len > 0,
          "ARM64 MIR emitter should return non-empty assembly");
    if (asm_text) {
        check_contains(asm_text, "\t.globl mir_emit_full\n",
                       "MIR assembly should declare global function");
        check_contains(asm_text, "mir_emit_full:\n",
                       "MIR assembly should emit function label");
        check_contains(asm_text, ".Lmir_emit_full_entry:\n",
                       "MIR assembly should emit entry label");
        check_contains(asm_text, ".Lmir_emit_full_then:\n",
                       "MIR assembly should emit then label");
        check_contains(asm_text, ".Lmir_emit_full_else:\n",
                       "MIR assembly should emit else label");
        check_contains(asm_text, ".Lmir_emit_full_exit:\n",
                       "MIR assembly should emit exit label");
        check_contains(asm_text, "\tstp x29, x30, [sp, #-16]!\n",
                       "MIR call function should save frame pointer and link register");
        check_contains(asm_text, "\tmov x29, sp\n",
                       "MIR assembly should establish frame pointer");
        check_contains(asm_text, "\tstr x19, [x29, #-",
                       "MIR assembly should preserve used callee-saved GPRs");
        check_contains(asm_text, "\tldr x19, [x29, #-",
                       "MIR assembly should restore used callee-saved GPRs");
        check_contains(asm_text, "\tmov ", "MIR assembly should emit moves");
        check_contains(asm_text, "\tadd ", "MIR assembly should emit add");
        check_contains(asm_text, "\tsub ", "MIR assembly should emit sub");
        check_contains(asm_text, "\tmul ", "MIR assembly should emit mul");
        check_contains(asm_text, "\tsdiv ", "MIR assembly should emit signed div");
        check_contains(asm_text, "\tudiv ", "MIR assembly should emit unsigned div");
        check_contains(asm_text, "\tmsub ", "MIR assembly should emit mod via msub");
        check_contains(asm_text, "\tand ", "MIR assembly should emit and");
        check_contains(asm_text, "\torr ", "MIR assembly should emit or");
        check_contains(asm_text, "\teor ", "MIR assembly should emit xor");
        check_contains(asm_text, "\tlsl ", "MIR assembly should emit shift left");
        check_contains(asm_text, "\tlsr ", "MIR assembly should emit logical shift right");
        check_contains(asm_text, "\tasr ", "MIR assembly should emit arithmetic shift right");
        check_contains(asm_text, "\tneg ", "MIR assembly should emit neg");
        check_contains(asm_text, "\tmvn ", "MIR assembly should emit not");
        check_contains(asm_text, "\tcmp ", "MIR assembly should emit cmp");
        check_contains(asm_text, "\tcset ", "MIR assembly should materialize cmp bool");
        check_contains(asm_text, "\tfadd ", "MIR assembly should emit FP add");
        check_contains(asm_text, "\tfsub ", "MIR assembly should emit FP sub");
        check_contains(asm_text, "\tfmul ", "MIR assembly should emit FP mul");
        check_contains(asm_text, "\tfdiv ", "MIR assembly should emit FP div");
        check_contains(asm_text, "\tfneg ", "MIR assembly should emit FP neg");
        check_contains(asm_text, "\tcbnz ", "MIR assembly should emit conditional branch");
        check_contains(asm_text, "\tbl callee\n", "MIR assembly should emit direct call");
        check_contains(asm_text, "\tldr ", "MIR assembly should emit load");
        check_contains(asm_text, "\tstr ", "MIR assembly should emit store");
        check_contains(asm_text, "\tret\n", "MIR assembly should emit return");
        free(asm_text);
    }

    anvil_mir_func_destroy(mir);
}

static void test_arm64_emits_materialized_spills_with_frame_slots(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("mir_emit_spills");
    CHECK(mir != NULL, "ARM64 MIR spill emitter function should be created");
    if (!mir) return;

    enum { NUM_VALUES = 20 };
    anvil_mir_vreg_t values[NUM_VALUES];
    for (size_t i = 0; i < NUM_VALUES; i++) {
        values[i] = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
        CHECK(values[i] != ANVIL_MIR_NO_VREG,
              "spill emitter vreg should be created");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_MOV, values[i], NULL, 0),
              "spill emitter value should be defined");
    }
    for (size_t i = 0; i < NUM_VALUES; i++) {
        anvil_mir_vreg_t uses[] = { values[i] };
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_KEEPALIVE,
                                  ANVIL_MIR_NO_VREG, uses, 1),
              "spill emitter value should stay live");
    }
    anvil_mir_vreg_t ret_uses[] = { values[0] };
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "spill emitter should return");

    CHECK(anvil_arm64_regalloc_mir(mir),
          "spill emitter should allocate and materialize spills");
    CHECK(anvil_mir_num_spills(mir) > 0,
          "spill emitter should have spill slots");

    char *asm_text = NULL;
    size_t asm_len = 0;
    CHECK(anvil_arm64_emit_mir(mir, &asm_text, &asm_len),
          "ARM64 MIR emitter should handle materialized spills");
    CHECK(asm_text != NULL && asm_len > 0,
          "ARM64 MIR spill emitter should return assembly");
    if (asm_text) {
        check_contains(asm_text, "mir_emit_spills:\n",
                       "spill assembly should emit function label");
        check_contains(asm_text, "\tstp x29, x30, [sp, #-16]!\n",
                       "spill assembly should create a frame");
        check_contains(asm_text, "\tsub sp, sp, #",
                       "spill assembly should allocate spill frame space");
        check_contains(asm_text, "\tstr x12, [x29, #-",
                       "spill assembly should store scratch GPR into spill slot");
        check_contains(asm_text, "\tldr x12, [x29, #-",
                       "spill assembly should reload scratch GPR from spill slot");
        check_contains(asm_text, "\tldp x29, x30, [sp], #16\n",
                       "spill assembly should restore frame");
        check_contains(asm_text, "\tret\n",
                       "spill assembly should return");
        free(asm_text);
    }

    anvil_mir_func_destroy(mir);
}

static void test_arm64_mir_legalizer_rejects_target_illegal_pointer_load(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("arm64_bad_pointer_load");
    CHECK(mir != NULL, "ARM64 legalizer pointer-load test function should be created");
    if (!mir) return;

    anvil_mir_vreg_t ptr32 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t ptr_use[] = { ptr32 };
    anvil_mir_vreg_t ret_uses[] = { value };
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, ptr32, 0),
          "bad pointer-load test should define a pointer-like vreg");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, value, ptr_use, 1),
          "generic MIR should accept a load from a GPR vreg");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "bad pointer-load test should terminate");

    char error[160] = { 0 };
    CHECK(anvil_mir_verify(mir, error, sizeof(error)),
          "generic MIR verifier should accept the target-illegal load");
    error[0] = '\0';
    CHECK(!anvil_arm64_verify_mir_legal(mir, error, sizeof(error)),
          "ARM64 MIR legalizer should reject non-64-bit pointer operands");
    CHECK(strstr(error, "pointer") != NULL,
          "ARM64 MIR legalizer should explain pointer operand failures");

    anvil_mir_func_destroy(mir);
}

static void test_arm64_mir_legalizer_rejects_unfixed_call_arguments(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("arm64_bad_call_arg");
    CHECK(mir != NULL, "ARM64 legalizer call-arg test function should be created");
    if (!mir) return;

    anvil_mir_vreg_t arg = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t call_ret = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t call_uses[] = { arg };
    anvil_mir_vreg_t ret_uses[] = { call_ret };
    CHECK(anvil_mir_set_fixed_reg(mir, call_ret, 0),
          "call result should be fixed to x0");
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, arg, 7),
          "bad call-arg test should define argument");
    CHECK(anvil_mir_add_instr_symbol(mir, ANVIL_MIR_OP_CALL, call_ret,
                                     call_uses, 1, "callee"),
          "generic MIR should accept a call with an unfixed argument vreg");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "bad call-arg test should terminate");

    char error[160] = { 0 };
    CHECK(anvil_mir_verify(mir, error, sizeof(error)),
          "generic MIR verifier should accept the target-illegal call");
    error[0] = '\0';
    CHECK(!anvil_arm64_verify_mir_legal(mir, error, sizeof(error)),
          "ARM64 MIR legalizer should reject call args not fixed to ABI regs");
    CHECK(strstr(error, "fixed") != NULL,
          "ARM64 MIR legalizer should explain fixed-register call failures");

    anvil_mir_func_destroy(mir);
}

int main(void)
{
    test_arm64_lowers_integer_add_with_entry_abi_copies();
    test_arm64_lowers_fp_add_with_entry_abi_copies();
    test_arm64_lowers_constants_bitwise_and_mixed_call_abi();
    test_arm64_lowers_stack_call_args_to_machineir();
    test_arm64_lowers_fp_stack_call_args_to_machineir();
    test_arm64_lowers_darwin_variadic_args_to_stack_machineir();
    test_arm64_lowers_div_mod_and_cmp_predicates_without_losing_semantics();
    test_arm64_lowers_if_else_branches_to_mir_blocks();
    test_arm64_lowers_phi_as_predecessor_edge_copies();
    test_arm64_splits_conditional_phi_edges();
    test_arm64_lowers_phi_swap_with_parallel_edge_copy();
    test_arm64_regalloc_materializes_machineir_spills();
    test_arm64_emits_allocated_mir_with_integer_fp_memory_call_and_cfg();
    test_arm64_emits_materialized_spills_with_frame_slots();
    test_arm64_mir_legalizer_rejects_target_illegal_pointer_load();
    test_arm64_mir_legalizer_rejects_unfixed_call_arguments();

    if (failures) {
        fprintf(stderr, "%d ARM64 MachineIR lowering regression test(s) failed\n",
                failures);
        return 1;
    }

    printf("ARM64 MachineIR lowering regression tests passed\n");
    return 0;
}
