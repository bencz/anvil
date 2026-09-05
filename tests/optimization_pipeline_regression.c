#include <anvil/anvil_internal.h>
#include <anvil/anvil_analysis.h>
#include <anvil/anvil_opt.h>
#include <anvil/anvil_x86_64_mir.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void test_machine_call_effects(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "machine_effects");
    anvil_type_t *type = anvil_type_func(ctx, anvil_type_void(ctx), NULL, 0, false);
    anvil_func_t *callee = anvil_func_declare(module, "pure_void", type);
    anvil_func_set_effects(callee, 0);
    anvil_func_t *caller = anvil_func_create(module, "caller", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
    anvil_value_t *unused = NULL;
    anvil_build_call_checked(ctx, anvil_func_get_value(callee), NULL, 0, "call", &unused);
    anvil_build_ret_void(ctx);

    anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(caller);
    bool found = false;
    if (mir)
    {
        for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++)
        {
            anvil_mir_instr_info_t info;
            anvil_mir_get_instr_info(mir, index, &info);
            if (info.op == ANVIL_MIR_OP_CALL)
                found = info.call_effects == 0;
        }
    }

    if (!found || anvil_pass_dce(caller) != ANVIL_PASS_RUN_CHANGED)
    {
        fprintf(stderr, "FAIL: pure void calls preserve their contract through MIR and DCE\n");
        failures++;
    }

    anvil_mir_func_destroy(mir);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_register_clobbers(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("clobbers");
    anvil_mir_block_t block = anvil_mir_current_block(mir);
    anvil_mir_set_current_block(mir, block);
    anvil_mir_vreg_t across = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t after = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, across, 17);
    anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "callee", ANVIL_CC_SYSV, false, 0);
    anvil_mir_set_instr_clobbers(mir, 1, ANVIL_MIR_REG_GPR, UINT64_C(1));
    anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, after, 25);
    anvil_mir_vreg_t operands[] = { across, after };
    anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, sum, operands, 2);
    anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, &sum, 1);
    const int registers[] = { 0, 1 };
    const anvil_regalloc_class_config_t config = { .reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = 2, .phys_regs = registers };
    bool allocated = anvil_regalloc_linear_scan_classes(mir, &config, 1);
    const anvil_regalloc_assignment_t *preserved = anvil_mir_get_assignment(mir, across);
    const anvil_regalloc_assignment_t *temporary = anvil_mir_get_assignment(mir, after);
    if (!allocated || !preserved || !temporary || preserved->spilled || temporary->spilled || preserved->phys_reg != 1 || temporary->phys_reg != 0)
    {
        fprintf(stderr, "FAIL: allocation must distinguish values crossing a physical clobber\n");
        failures++;
    }

    char error[256] = { 0 };
    if (!anvil_mir_verify(mir, error, sizeof(error)))
    {
        fprintf(stderr, "FAIL: valid register allocation rejected: %s\n", error);
        failures++;
    }

    if (preserved)
    {
        anvil_regalloc_assignment_t *corrupted = (anvil_regalloc_assignment_t *)preserved;
        corrupted->phys_reg = 0;
        if (anvil_mir_verify(mir, error, sizeof(error)) || !strstr(error, "clobbers"))
        {
            fprintf(stderr, "FAIL: verifier accepted a live value destroyed by a call\n");
            failures++;
        }
    }

    anvil_mir_func_destroy(mir);
}

static void check(bool condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static size_t count_opcode(const anvil_func_t *func, anvil_op_t opcode)
{
    size_t count = 0;
    for (const anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (const anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            if (instr->op == opcode)
                count++;
        }
    }

    return count;
}

static void test_promotion(anvil_arch_t arch, bool loop, bool initialized, bool escaped)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "promotion");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(module, "promotion", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *left = anvil_block_create(func, "left");
    anvil_block_t *right = anvil_block_create(func, "right");
    anvil_block_t *join = anvil_block_create(func, "join");
    anvil_value_t *argument = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *two = anvil_const_i32(ctx, 2);
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *slot = anvil_build_alloca(ctx, i32, "slot");
    if (initialized)
        check(anvil_build_store(ctx, one, slot), "initialize promoted slot");

    if (escaped)
    {
        anvil_type_t *callee_params[] = { slot->type };
        anvil_type_t *callee_type = anvil_type_func(ctx, anvil_type_void(ctx), callee_params, 1, false);
        anvil_func_t *callee = anvil_func_declare(module, "observe", callee_type);
        anvil_value_t *args[] = { slot };
        anvil_value_t *result = NULL;
        check(anvil_build_call_checked(ctx, anvil_func_get_value(callee), args, 1, NULL, &result), "escape allocation to call");
    }

    anvil_value_t *condition = anvil_build_cmp_eq(ctx, argument, one, "condition");
    check(anvil_build_br_cond(ctx, condition, left, right), "branch before promotion");
    anvil_set_insert_point(ctx, left);
    check(anvil_build_store(ctx, two, slot), "left definition");
    check(anvil_build_br(ctx, join), "left branch");
    anvil_set_insert_point(ctx, right);
    check(anvil_build_br(ctx, join), "right branch");
    anvil_set_insert_point(ctx, join);
    anvil_value_t *value = anvil_build_load(ctx, i32, slot, "joined");
    if (loop)
    {
        anvil_block_t *body = anvil_block_create(func, "body");
        anvil_block_t *exit = anvil_block_create(func, "exit");
        anvil_value_t *again = anvil_build_cmp_lt(ctx, value, argument, "again");
        check(anvil_build_br_cond(ctx, again, body, exit), "loop condition");
        anvil_set_insert_point(ctx, body);
        anvil_value_t *next = anvil_build_add(ctx, value, one, "next");
        check(anvil_build_store(ctx, next, slot), "loop definition");
        check(anvil_build_br(ctx, join), "loop backedge");
        anvil_set_insert_point(ctx, exit);
    }

    check(anvil_build_ret(ctx, value), "return joined value");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "valid input to promotion");
    bool eligible = initialized && !escaped;
    check(anvil_pass_mem2reg(func) == (eligible ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED), "promotion eligibility");
    if (!anvil_func_verify(func, error, sizeof(error)))
    {
        fprintf(stderr, "promotion verifier: %s\n", error);
        check(false, "valid promoted SSA");
    }

    check(count_opcode(func, ANVIL_OP_ALLOCA) == (eligible ? 0u : 1u), "only eligible allocation disappears");
    check(count_opcode(func, ANVIL_OP_PHI) == (eligible ? 1u : 0u), "join and loop PHI placement");
    check(anvil_pass_mem2reg(func) == ANVIL_PASS_RUN_UNCHANGED, "promotion reaches a fixpoint");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "lower promoted joins and loops");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_memory_contract(anvil_arch_t arch, bool is_volatile)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    check(ctx != NULL, "create target context");
    if (!ctx)
        return;

    anvil_module_t *module = anvil_module_create(ctx, "memory_contract");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(module, "memory_contract", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));

    anvil_value_t *slot = anvil_build_alloca(ctx, i32, "slot");
    anvil_memory_access_t access = { .alignment = 4, .is_volatile = is_volatile };
    check(anvil_build_store_ex(ctx, anvil_const_i32(ctx, 1), slot, &access), "first store");
    check(anvil_build_store_ex(ctx, anvil_const_i32(ctx, 2), slot, &access), "second store");
    anvil_value_t *first = anvil_build_load_ex(ctx, i32, slot, &access, "first");
    anvil_value_t *second = anvil_build_load_ex(ctx, i32, slot, &access, "second");
    check(anvil_build_ret(ctx, anvil_build_add(ctx, first, second, "sum")), "memory return");

    check(anvil_ctx_set_opt_level(ctx, ANVIL_OPT_AGGRESSIVE) == ANVIL_OK, "select optimization level");
    check(anvil_module_optimize(module) == ANVIL_OK, "optimize memory function");
    check(count_opcode(func, ANVIL_OP_STORE) == (is_volatile ? 2u : 0u), "preserve observable stores and promote ordinary storage");
    check(count_opcode(func, ANVIL_OP_LOAD) == (is_volatile ? 2u : 0u), "preserve volatile reads and forward ordinary reads");

    if (arch == ANVIL_ARCH_X86_64)
    {
        anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(func);
        check(mir != NULL, "lower memory function to MIR");
        size_t ordered = 0;
        for (size_t index = 0; mir && index < anvil_mir_num_instrs(mir); index++)
        {
            anvil_mir_instr_info_t info;
            check(anvil_mir_get_instr_info(mir, index, &info), "read MIR instruction");
            if (info.memory_access.is_volatile)
                ordered++;
        }

        check(ordered == (is_volatile ? 4u : 0u), "volatile attributes survive MIR lowering");
        anvil_mir_func_destroy(mir);
    }

    char *assembly = NULL;
    size_t length = 0;
    anvil_error_t status = anvil_module_codegen(module, &assembly, &length);
    if (status != ANVIL_OK)
        fprintf(stderr, "target %d: %s\n", (int)arch, anvil_ctx_get_error(ctx));

    check(status == ANVIL_OK && length > 0, "generate target assembly with memory semantics");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_promotion_allocation_failure(void)
{
    bool succeeded = false;
    for (size_t allowance = 0; allowance < 80 && !succeeded; allowance++)
    {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_module_t *module = anvil_module_create(ctx, "promotion_failure");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { anvil_type_i1(ctx) };
        anvil_type_t *type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(module, "promotion_failure", type, ANVIL_LINK_EXTERNAL);
        anvil_block_t *left = anvil_block_create(func, "left");
        anvil_block_t *join = anvil_block_create(func, "join");
        anvil_set_insert_point(ctx, anvil_func_get_entry(func));
        anvil_value_t *slot = anvil_build_alloca(ctx, i32, "slot");
        anvil_build_store(ctx, anvil_const_i32(ctx, 1), slot);
        anvil_build_br_cond(ctx, anvil_func_get_param(func, 0), left, join);
        anvil_set_insert_point(ctx, left);
        anvil_build_store(ctx, anvil_const_i32(ctx, 2), slot);
        anvil_build_br(ctx, join);
        anvil_set_insert_point(ctx, join);
        anvil_build_ret(ctx, anvil_build_load(ctx, i32, slot, "joined"));

        anvil_test_fail_alloc_after(ctx, allowance);
        anvil_pass_result_t result = anvil_pass_mem2reg(func);
        anvil_test_disable_alloc_fail(ctx);
        if (result == ANVIL_PASS_RUN_ERROR)
        {
            check(anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM, "promotion reports allocation failure");
            check(count_opcode(func, ANVIL_OP_ALLOCA) == 1, "failed promotion preserves original allocation");
        }
        else
        {
            check(result == ANVIL_PASS_RUN_CHANGED, "promotion succeeds after all allocations are available");
            succeeded = true;
        }

        anvil_ctx_clear_error(ctx);
        char error[256] = { 0 };
        check(anvil_func_verify(func, error, sizeof(error)), "allocation failure preserves valid IR");
        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
    }

    check(succeeded, "exercise every promotion allocation failure");
}

static void test_global_values(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "global_values");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(module, "global_values", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *exit = anvil_block_create(func, "exit");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *argument = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_build_br(ctx, body);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *first = anvil_build_add(ctx, argument, one, "first");
    anvil_build_br(ctx, exit);
    anvil_set_insert_point(ctx, exit);
    anvil_value_t *second = anvil_build_add(ctx, one, argument, "second");
    anvil_build_ret(ctx, anvil_build_mul(ctx, first, second, "product"));

    check(anvil_pass_gvn(func) == ANVIL_PASS_RUN_CHANGED, "GVN reuses a dominating commuted expression");
    check(count_opcode(func, ANVIL_OP_ADD) == 1, "GVN removes cross-block duplicate");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "GVN preserves SSA dominance");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "lower definition preceding use in CFG but following it in block list");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_call_effects(unsigned effects)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "call_effects");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *callee_type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *callee = anvil_func_declare(module, "callee", callee_type);
    check(anvil_func_get_effects(callee) == ANVIL_EFFECT_ALL, "unknown declarations retain every effect");
    check(anvil_func_set_effects(callee, effects) == ANVIL_OK, "set explicit call contract");
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32) };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(module, "call_effects", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *pointer = anvil_func_get_param(func, 0);
    anvil_value_t *first = anvil_build_load(ctx, i32, pointer, "first");
    anvil_value_t *unused = NULL;
    check(anvil_build_call_checked(ctx, anvil_func_get_value(callee), NULL, 0, "unused", &unused), "create contracted call");
    anvil_value_t *second = anvil_build_load(ctx, i32, pointer, "second");
    anvil_build_ret(ctx, anvil_build_add(ctx, first, second, "sum"));

    bool barrier = (effects & (ANVIL_EFFECT_WRITE_MEMORY | ANVIL_EFFECT_OBSERVABLE)) != 0;
    check(anvil_pass_load_elim(func) == (barrier ? ANVIL_PASS_RUN_UNCHANGED : ANVIL_PASS_RUN_CHANGED), "memory forwarding respects call effects");
    check(count_opcode(func, ANVIL_OP_LOAD) == (barrier ? 2u : 1u), "load count reflects memory contract");
    check(anvil_pass_dce(func) == (effects ? ANVIL_PASS_RUN_UNCHANGED : ANVIL_PASS_RUN_CHANGED), "unused call removal requires no effects");
    check(count_opcode(func, ANVIL_OP_CALL) == (effects ? 1u : 0u), "observable calls remain");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "call transformations preserve valid IR");
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_scalar_replacement(anvil_arch_t arch, bool is_volatile)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "scalar_replacement");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *array = anvil_type_array(ctx, i32, 3);
    anvil_type_t *fields[] = { i32, array };
    anvil_type_t *record = anvil_type_struct(ctx, "record", fields, 2);
    anvil_type_t *type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(module, "scalar_replacement", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *storage = anvil_build_alloca(ctx, record, "record");
    anvil_value_t *first = anvil_build_struct_gep(ctx, record, storage, 0, "first");
    anvil_value_t *elements = anvil_build_struct_gep(ctx, record, storage, 1, "elements");
    anvil_value_t *indices[] = { anvil_const_i32(ctx, 0), anvil_const_i32(ctx, 2) };
    anvil_value_t *last = anvil_build_gep(ctx, array, elements, indices, 2, "last");
    anvil_memory_access_t access = { .is_volatile = is_volatile };
    anvil_build_store_ex(ctx, anvil_const_i32(ctx, 11), first, &access);
    anvil_build_store(ctx, anvil_const_i32(ctx, 31), last);
    anvil_value_t *left = anvil_build_load_ex(ctx, i32, first, &access, "left");
    anvil_value_t *right = anvil_build_load(ctx, i32, last, "right");
    anvil_build_ret(ctx, anvil_build_add(ctx, left, right, "sum"));

    check(anvil_pass_sroa(func) == (is_volatile ? ANVIL_PASS_RUN_UNCHANGED : ANVIL_PASS_RUN_CHANGED), "SROA respects observable aggregate accesses");
    check(anvil_pass_mem2reg(func) == (is_volatile ? ANVIL_PASS_RUN_UNCHANGED : ANVIL_PASS_RUN_CHANGED), "promote scalarized fields");
    check(count_opcode(func, ANVIL_OP_ALLOCA) == (is_volatile ? 1u : 0u), "scalarized aggregate leaves no stack allocation");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "SROA preserves valid IR");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "generate scalarized nested aggregate");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void observe_pass(const anvil_func_t *func, const anvil_pass_info_t *pass, const anvil_pass_statistics_t *event, void *user_data)
{
    size_t *events = user_data;
    check(func && pass && event->runs == 1, "observer receives a pass/function event");
    check(event->failures == 0 && event->cpu_seconds >= 0.0, "observer receives successful pass statistics");
    (*events)++;
}

static void test_loop_motion(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "loop_motion");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(module, "loop_motion", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *header = anvil_block_create(func, "header");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_block_t *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, i32, "index");
    anvil_value_t *sum = anvil_build_phi(ctx, i32, "sum");
    anvil_value_t *limit = anvil_func_get_param(func, 0);
    anvil_value_t *divisor = anvil_func_get_param(func, 1);
    anvil_value_t *condition = anvil_build_cmp_lt(ctx, index, limit, "condition");
    anvil_build_br_cond(ctx, condition, body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *invariant = anvil_build_mul(ctx, limit, anvil_const_i32(ctx, 7), "invariant");
    anvil_value_t *division = anvil_build_sdiv(ctx, invariant, divisor, "may_trap");
    anvil_value_t *next_sum = anvil_build_add(ctx, sum, division, "next_sum");
    anvil_value_t *next_index = anvil_build_add(ctx, index, anvil_const_i32(ctx, 1), "next_index");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, anvil_const_i32(ctx, 0), entry);
    anvil_phi_add_incoming(index, next_index, body);
    anvil_phi_add_incoming(sum, anvil_const_i32(ctx, 0), entry);
    anvil_phi_add_incoming(sum, next_sum, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, sum);

    anvil_pass_manager_t *pm = anvil_pass_manager_create(ctx);
    check(anvil_pass_manager_set_level(pm, ANVIL_OPT_STANDARD) == ANVIL_OK, "select O2 pipeline");
    check(!anvil_pass_manager_is_enabled(pm, ANVIL_PASS_LICM), "O2 does not enable loop speculation");
    check(anvil_pass_manager_set_level(pm, ANVIL_OPT_AGGRESSIVE) == ANVIL_OK, "select O3 pipeline");
    check(anvil_pass_manager_is_enabled(pm, ANVIL_PASS_LICM), "O3 enables loop invariant motion");
    size_t events = 0;
    anvil_pass_manager_set_observer(pm, observe_pass, &events);
    check(anvil_pass_manager_run_func(pm, func) == ANVIL_PASS_RUN_CHANGED, "optimize loop with statistics");
    check(invariant->data.instr->parent == entry, "hoist invariant into preheader");
    check(division->data.instr->parent == body, "do not speculate potentially trapping division in zero-trip loop");
    anvil_pass_statistics_t statistics;
    check(anvil_pass_manager_get_statistics(pm, ANVIL_PASS_LICM, &statistics), "retrieve LICM statistics");
    check(statistics.runs >= 2 && statistics.changes == 1 && statistics.failures == 0 && events > statistics.runs, "statistics track changes and fixpoint iterations");
    anvil_pass_manager_reset_statistics(pm);
    check(anvil_pass_manager_get_statistics(pm, ANVIL_PASS_LICM, &statistics) && statistics.runs == 0, "reset collected statistics");
    anvil_pass_manager_destroy(pm);
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "generate optimized loop");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_conditional_constants(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "conditional_constants");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(module, "conditional_constants", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *left = anvil_block_create(func, "left");
    anvil_block_t *right = anvil_block_create(func, "right");
    anvil_block_t *join = anvil_block_create(func, "join");
    anvil_block_t *good = anvil_block_create(func, "good");
    anvil_block_t *bad = anvil_block_create(func, "bad");
    anvil_set_insert_point(ctx, entry);
    anvil_build_br_cond(ctx, anvil_const_i1(ctx, true), left, right);
    anvil_set_insert_point(ctx, left);
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, right);
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, join);
    anvil_value_t *phi = anvil_build_phi(ctx, i32, "selected");
    anvil_value_t *eleven = anvil_const_i32(ctx, 11);
    anvil_phi_add_incoming(phi, eleven, left);
    anvil_phi_add_incoming(phi, anvil_const_i32(ctx, 99), right);
    anvil_value_t *condition = anvil_build_cmp_eq(ctx, phi, eleven, "known");
    anvil_build_br_cond(ctx, condition, good, bad);
    anvil_set_insert_point(ctx, good);
    anvil_build_ret(ctx, anvil_build_add(ctx, phi, anvil_const_i32(ctx, 3), "result"));
    anvil_set_insert_point(ctx, bad);
    anvil_build_ret(ctx, anvil_build_sdiv(ctx, eleven, anvil_const_i32(ctx, 0), "unreachable_trap"));

    check(anvil_pass_sccp(func) == ANVIL_PASS_RUN_CHANGED, "SCCP propagates executable PHI inputs");
    check(count_opcode(func, ANVIL_OP_PHI) == 0 && count_opcode(func, ANVIL_OP_SDIV) == 0, "SCCP removes infeasible path and its trap");
    check(func->entry->last->op == ANVIL_OP_RET && func->entry->last->operands[0]->kind == ANVIL_VAL_CONST_INT &&
          func->entry->last->operands[0]->data.i == 14, "SCCP computes result across two conditional joins");
    check(anvil_pass_sccp(func) == ANVIL_PASS_RUN_UNCHANGED, "SCCP reaches a fixpoint");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "SCCP preserves verified IR");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "generate conditional constant result");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_cfg_cache(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "cfg_cache");
    anvil_type_t *params[] = { anvil_type_i1(ctx) };
    anvil_type_t *type = anvil_type_func(ctx, anvil_type_i32(ctx), params, 1, false);
    anvil_func_t *func = anvil_func_create(module, "cfg_cache", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *left = anvil_block_create(func, "left");
    anvil_block_t *right = anvil_block_create(func, "right");
    anvil_set_insert_point(ctx, entry);
    anvil_build_br_cond(ctx, anvil_func_get_param(func, 0), left, right);
    anvil_set_insert_point(ctx, left);
    anvil_build_ret(ctx, anvil_const_i32(ctx, 1));
    anvil_set_insert_point(ctx, right);
    anvil_build_ret(ctx, anvil_const_i32(ctx, 2));

    anvil_opt_cfg_t original;
    anvil_opt_cfg_t reused;
    anvil_opt_cfg_t rebuilt;
    check(anvil_opt_cfg_build(func, &original), "build cached CFG");
    check(anvil_opt_cfg_build(func, &reused), "reuse cached CFG");
    check(original.successors == reused.successors, "unchanged CFG shares its immutable snapshot");
    entry->last->true_block = right;
    check(anvil_opt_cfg_build(func, &rebuilt), "rebuild after direct terminator mutation");
    check(original.successors != rebuilt.successors, "CFG mutation invalidates cached analyses");
    check(original.successor_offsets[1] == 2 && rebuilt.successor_offsets[1] == 1, "retain old snapshot and deduplicate new parallel edges");
    check(original.reachable_count == 3 && rebuilt.reachable_count == 2, "recompute reachability after mutation");
    anvil_func_invalidate_cfg(func);
    check(rebuilt.successor_offsets[1] == 1, "explicit invalidation preserves acquired snapshots");
    anvil_opt_cfg_destroy(&original);
    anvil_opt_cfg_destroy(&reused);
    anvil_opt_cfg_destroy(&rebuilt);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_parallel_copies(void)
{
    uint32_t random = UINT32_C(0x1937a501);
    for (size_t trial = 0; trial < 160; trial++)
    {
        anvil_mir_func_t *mir = anvil_mir_func_create("parallel_copies");
        size_t count = 3 + trial % 67;
        anvil_mir_parallel_copy_t copies[70];
        uint64_t expected[70];
        for (size_t index = 0; index < count; index++)
        {
            anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, 64, false);
            check(value == index && anvil_mir_set_live_in(mir, value, true), "create parallel copy live-in");
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            copies[index].dst = value;
            copies[index].src = random % count;
            expected[index] = UINT64_C(0x1234000000000000) + copies[index].src;
        }

        check(anvil_mir_emit_parallel_copies(mir, copies, count), "emit parallel assignments with cycles and fanout");
        size_t registers = anvil_mir_num_vregs(mir);
        uint64_t *values = calloc(registers, sizeof(*values));
        check(values != NULL, "allocate copy execution state");
        if (!values)
        {
            anvil_mir_func_destroy(mir);
            return;
        }

        for (size_t index = 0; index < count; index++)
            values[index] = UINT64_C(0x1234000000000000) + index;

        for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++)
        {
            anvil_mir_instr_info_t instr;
            check(anvil_mir_get_instr_info(mir, index, &instr) && instr.op == ANVIL_MIR_OP_COPY && instr.num_uses == 1, "parallel assignments lower to scalar copies");
            anvil_mir_vreg_t source = anvil_mir_get_instr_use(mir, index, 0);
            values[instr.def] = values[source];
        }

        for (size_t index = 0; index < count; index++)
            check(values[index] == expected[index], "sequential copy execution matches simultaneous assignment");

        size_t instructions = anvil_mir_num_instrs(mir);
        anvil_mir_parallel_copy_t conflicting[] = { { 0, 1 }, { 0, 2 } };
        check(!anvil_mir_emit_parallel_copies(mir, conflicting, 2), "reject duplicate parallel destinations");
        check(anvil_mir_num_instrs(mir) == instructions && anvil_mir_num_vregs(mir) == registers, "invalid copies leave MIR unchanged");
        free(values);
        anvil_mir_func_destroy(mir);
    }
}

static void test_spill_slot_reuse(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("spill_slot_reuse");
    anvil_mir_vreg_t result = ANVIL_MIR_NO_VREG;
    for (size_t phase = 0; phase < 12; phase++)
    {
        anvil_mir_vreg_t left = anvil_mir_add_vreg(mir);
        anvil_mir_vreg_t right = anvil_mir_add_vreg(mir);
        result = anvil_mir_add_vreg(mir);
        anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, left, 17);
        anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, right, 25);
        anvil_mir_vreg_t uses[] = { left, right };
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, result, uses, 2);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, &result, 1);
    }

    anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, &result, 1);
    check(anvil_regalloc_linear_scan(mir, 1), "allocate consecutive regions under register pressure");
    check(anvil_mir_num_spills(mir) == 1, "reuse one spill slot across twelve disjoint live intervals");
    int scratch_registers[] = { 2, 3, 4 };
    anvil_regalloc_class_config_t scratch[] = { { ANVIL_MIR_REG_GPR, 3, scratch_registers } };
    check(anvil_mir_materialize_spills(mir, scratch, 1), "rematerialize spilled integer constants");
    size_t spill_accesses = 0;
    uint64_t *values = calloc(anvil_mir_num_vregs(mir), sizeof(*values));
    check(values != NULL, "allocate rematerialization execution state");
    for (size_t index = 0; values && index < anvil_mir_num_instrs(mir); index++)
    {
        anvil_mir_instr_info_t instr;
        anvil_mir_get_instr_info(mir, index, &instr);
        if (instr.op == ANVIL_MIR_OP_SPILL_LOAD || instr.op == ANVIL_MIR_OP_SPILL_STORE)
            spill_accesses++;
        else if (instr.op == ANVIL_MIR_OP_MOV && instr.has_imm)
            values[instr.def] = (uint64_t)instr.imm;
        else if (instr.op == ANVIL_MIR_OP_ADD)
            values[instr.def] = values[anvil_mir_get_instr_use(mir, index, 0)] + values[anvil_mir_get_instr_use(mir, index, 1)];
        else if (instr.op == ANVIL_MIR_OP_RET)
            check(values[anvil_mir_get_instr_use(mir, index, 0)] == 42, "rematerialized constants preserve execution result");
    }

    check(spill_accesses == 0, "integer constants require no spill loads or stores");
    free(values);
    char error[256] = { 0 };
    check(anvil_mir_verify(mir, error, sizeof(error)), "reused spill storage preserves valid MIR");
    anvil_mir_func_destroy(mir);
}

static void test_bit_facts(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "bit_facts");
    anvil_type_t *u32 = anvil_type_u32(ctx);
    anvil_type_t *params[] = { u32 };
    anvil_type_t *type = anvil_type_func(ctx, u32, params, 1, false);
    anvil_func_t *func = anvil_func_create(module, "bit_facts", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *argument = anvil_func_get_param(func, 0);
    anvil_value_t *masked = anvil_build_and(ctx, argument, anvil_const_u32(ctx, 15), "masked");
    anvil_value_t *bounded = anvil_build_cmp_ult(ctx, masked, anvil_const_u32(ctx, 16), "bounded");
    anvil_value_t *positive = anvil_build_zext(ctx, bounded, u32, "positive");
    anvil_value_t *negative = anvil_build_sext(ctx, bounded, u32, "negative");
    anvil_value_t *shifted = anvil_build_shl(ctx, masked, anvil_const_u32(ctx, 8), "shifted");
    anvil_value_t *zero = anvil_build_and(ctx, shifted, anvil_const_u32(ctx, 255), "zero");
    anvil_value_t *sum = anvil_build_add(ctx, positive, negative, "wrap");
    anvil_build_ret(ctx, anvil_build_or(ctx, sum, zero, "result"));
    check(anvil_pass_known_bits(func) == ANVIL_PASS_RUN_CHANGED, "bit masks and unsigned bounds expose constants");
    check(func->entry->last->operands[0]->kind == ANVIL_VAL_CONST_INT && func->entry->last->operands[0]->data.u == 0,
          "bit facts preserve modulo arithmetic and i1 sign extension");

    anvil_type_t *boolean_type = anvil_type_func(ctx, anvil_type_i1(ctx), params, 1, false);
    anvil_func_t *uncertain = anvil_func_create(module, "uncertain_range", boolean_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(uncertain));
    argument = anvil_func_get_param(uncertain, 0);
    masked = anvil_build_and(ctx, argument, anvil_const_u32(ctx, 15), "masked");
    anvil_build_ret(ctx, anvil_build_cmp_ult(ctx, masked, anvil_const_u32(ctx, 7), "uncertain"));
    check(anvil_pass_known_bits(uncertain) == ANVIL_PASS_RUN_UNCHANGED, "do not fold bounds that straddle the comparison threshold");

    anvil_type_t *signed_type = anvil_type_func(ctx, anvil_type_i32(ctx), NULL, 0, false);
    anvil_func_t *sign = anvil_func_create(module, "sign_extension", signed_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(sign));
    anvil_build_ret(ctx, anvil_build_sext(ctx, anvil_const_i1(ctx, true), anvil_type_i32(ctx), "sign"));
    check(anvil_pass_sccp(sign) == ANVIL_PASS_RUN_CHANGED && sign->entry->last->operands[0]->data.i == -1, "SCCP uses the one-bit source width for sign extension");

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *signed_params[] = { i32 };
    anvil_type_t *predicate_type = anvil_type_func(ctx, anvil_type_i1(ctx), signed_params, 1, false);
    anvil_func_t *range = anvil_func_create(module, "signed_range", predicate_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(range));
    argument = anvil_func_get_param(range, 0);
    masked = anvil_build_and(ctx, argument, anvil_const_i32(ctx, 127), "nonnegative");
    anvil_value_t *sign_bit = anvil_build_or(ctx, argument, anvil_const_i32(ctx, INT32_MIN), "negative");
    anvil_value_t *nonnegative = anvil_build_cmp_ge(ctx, masked, anvil_const_i32(ctx, 0), "lower.bound");
    anvil_value_t *is_negative = anvil_build_cmp_lt(ctx, sign_bit, anvil_const_i32(ctx, 0), "upper.bound");
    anvil_build_ret(ctx, anvil_build_and(ctx, nonnegative, is_negative, "signed.bounds"));
    check(anvil_pass_known_bits(range) == ANVIL_PASS_RUN_CHANGED && range->entry->last->operands[0]->kind == ANVIL_VAL_CONST_INT &&
          range->entry->last->operands[0]->data.u == 1, "signed bounds preserve sign-bit ordering");

    char error[256] = { 0 };
    check(anvil_module_verify(module, error, sizeof(error)), "bit facts preserve verified IR");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "generate bit fact regressions");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_memory_aliasing(anvil_arch_t arch, bool overwrite)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "memory_aliasing");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *fields[] = { i32, i32 };
    anvil_type_t *record = anvil_type_struct(ctx, "pair", fields, 2);
    anvil_type_t *type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(module, "memory_aliasing", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *continuation = anvil_block_create(func, "continuation");
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *storage = anvil_build_alloca(ctx, record, "pair");
    anvil_value_t *first = anvil_build_struct_gep(ctx, record, storage, 0, "first");
    anvil_value_t *second = anvil_build_struct_gep(ctx, record, storage, 1, "second");
    anvil_build_store(ctx, anvil_const_i32(ctx, 11), first);
    anvil_value_t *before = anvil_build_load(ctx, i32, first, "before");
    anvil_build_br(ctx, continuation);
    anvil_set_insert_point(ctx, continuation);
    anvil_build_store(ctx, anvil_const_i32(ctx, 31), overwrite ? first : second);
    anvil_value_t *equivalent = anvil_build_struct_gep(ctx, record, storage, 0, "equivalent");
    anvil_value_t *after = anvil_build_load(ctx, i32, equivalent, "after");
    anvil_build_ret(ctx, anvil_build_add(ctx, before, after, "sum"));

    check(anvil_memory_alias(first, 4, second, 4) == ANVIL_ALIAS_NO, "disjoint fields of one allocation do not alias");
    check(anvil_memory_alias(first, 4, equivalent, 4) == ANVIL_ALIAS_MUST, "equivalent constant field addresses must alias");
    check(anvil_memory_alias(storage, 8, first, 4) == ANVIL_ALIAS_MAY, "overlapping ranges remain conservative");
    check(anvil_pass_load_elim(func) == ANVIL_PASS_RUN_CHANGED, "forward values from the relevant dominating stores");
    check(count_opcode(func, ANVIL_OP_LOAD) == 0, "known stores provide both field loads");
    check(anvil_pass_const_fold(func) == ANVIL_PASS_RUN_CHANGED && func->last_block->last->operands[0]->kind == ANVIL_VAL_CONST_INT &&
          func->last_block->last->operands[0]->data.i == (overwrite ? 42 : 22), "an aliasing store changes the forwarded value");
    char error[256] = { 0 };
    check(anvil_func_verify(func, error, sizeof(error)), "cross-block memory forwarding preserves dominance");
    char *assembly = NULL;
    size_t length = 0;
    check(anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length > 0, "generate alias-aware memory code");
    free(assembly);
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

static void test_store_before_trap(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "store_before_trap");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32), i32 };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(module, "store_before_trap", type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(func));
    anvil_value_t *pointer = anvil_func_get_param(func, 0);
    anvil_build_store(ctx, anvil_const_i32(ctx, 1), pointer);
    anvil_value_t *division = anvil_build_sdiv(ctx, anvil_const_i32(ctx, 42), anvil_func_get_param(func, 1), "may_trap");
    anvil_build_store(ctx, anvil_const_i32(ctx, 2), pointer);
    anvil_build_ret(ctx, division);
    check(anvil_pass_dead_store(func) == ANVIL_PASS_RUN_UNCHANGED && count_opcode(func, ANVIL_OP_STORE) == 2,
          "preserve a store observable by a handler before the later overwrite");
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_promotion_allocation_failure();
    test_cfg_cache();
    test_parallel_copies();
    test_spill_slot_reuse();
    test_store_before_trap();
    test_machine_call_effects();
    test_register_clobbers();
    test_call_effects(0);
    test_call_effects(ANVIL_EFFECT_READ_MEMORY);
    test_call_effects(ANVIL_EFFECT_MAY_UNWIND);
    test_call_effects(ANVIL_EFFECT_OBSERVABLE);
    test_call_effects(ANVIL_EFFECT_ALL);
    const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86, ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64,
        ANVIL_ARCH_PPC32, ANVIL_ARCH_PPC64, ANVIL_ARCH_PPC64LE,
        ANVIL_ARCH_S370, ANVIL_ARCH_S370_XA, ANVIL_ARCH_S390, ANVIL_ARCH_ZARCH,
    };
    for (size_t index = 0; index < sizeof(targets) / sizeof(targets[0]); index++)
    {
        test_memory_contract(targets[index], false);
        test_memory_contract(targets[index], true);
        test_promotion(targets[index], false, true, false);
        test_promotion(targets[index], true, true, false);
        test_promotion(targets[index], true, false, false);
        test_promotion(targets[index], false, true, true);
        test_global_values(targets[index]);
        test_scalar_replacement(targets[index], false);
        test_scalar_replacement(targets[index], true);
        test_loop_motion(targets[index]);
        test_conditional_constants(targets[index]);
        test_bit_facts(targets[index]);
        test_memory_aliasing(targets[index], false);
        test_memory_aliasing(targets[index], true);
    }

    return failures ? 1 : 0;
}
