#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <anvil/anvil_x86_64_mir.h>
#include <stdio.h>
#include <stdlib.h>

static size_t count_calls(anvil_func_t *func)
{
    size_t count = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            count += instr->op == ANVIL_OP_CALL;
    }

    return count;
}

static bool run_case(anvil_arch_t arch, anvil_linkage_t linkage, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "inline_contracts");
    anvil_type_t *integer = anvil_type_i32(ctx);
    anvil_type_t *type = anvil_type_func(ctx, integer, &integer, 1, false);
    anvil_func_t *callee = anvil_func_create(module, "select_value", type, linkage);
    anvil_value_t *parameter = anvil_func_get_param(callee, 0);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_block_t *positive = anvil_block_create(callee, "positive");
    anvil_block_t *negative = anvil_block_create(callee, "negative");
    anvil_set_insert_point(ctx, callee->entry);
    anvil_instr_t *selection = anvil_build_switch(ctx, parameter, positive);
    anvil_switch_add_case(selection, zero, negative);
    anvil_set_insert_point(ctx, positive);
    anvil_build_ret(ctx, anvil_build_add(ctx, parameter, one, "incremented"));
    anvil_set_insert_point(ctx, negative);
    anvil_build_ret(ctx, one);

    anvil_func_t *caller = anvil_func_create(module, "caller", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(caller, "header");
    anvil_block_t *body = anvil_block_create(caller, "body");
    anvil_block_t *exit = anvil_block_create(caller, "exit");
    anvil_set_insert_point(ctx, caller->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, integer, "index");
    anvil_value_t *condition = anvil_build_cmp_lt(ctx, index, anvil_func_get_param(caller, 0), "condition");
    anvil_build_br_cond(ctx, condition, body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_value_t *next = NULL;
    anvil_build_call_checked(ctx, anvil_func_get_value(callee), &index, 1, "next", &next);
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, zero, caller->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, index);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);

    anvil_pass_result_t status = anvil_pass_inline(caller);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = { 0 };
    bool valid = anvil_func_verify(caller, error, sizeof(error)) && anvil_func_verify(callee, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed)
    {
        bool eligible = linkage == ANVIL_LINK_INTERNAL;
        valid &= status == (eligible ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        valid &= count_calls(caller) == (eligible ? 0 : 1);
        valid &= anvil_pass_inline(caller) == ANVIL_PASS_RUN_UNCHANGED;
        anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
        anvil_pass_manager_set_level(manager, ANVIL_OPT_AGGRESSIVE);
        valid &= anvil_pass_manager_run_module(manager, module) != ANVIL_PASS_RUN_ERROR;
        anvil_pass_manager_destroy(manager);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length != 0;
        free(assembly);
    }
    else
        valid &= count_calls(caller) == 1;

    if (!valid)
    {
        fprintf(stderr, "inline failure: arch=%d linkage=%d allowance=%zu status=%d error=%s context=%s\n", arch, linkage, allowance, status, error, anvil_ctx_get_error(ctx));
        anvil_dump_module(stderr, module);
        if (arch == ANVIL_ARCH_X86_64)
        {
            anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(caller);
            if (mir)
            {
                bool legal = anvil_x86_64_verify_mir_legal(mir, error, sizeof(error));
                fprintf(stderr, "lowered legal=%d %s\n", legal, error);
                bool allocated = anvil_x86_64_regalloc_mir(mir);
                legal = anvil_x86_64_verify_mir_legal(mir, error, sizeof(error));
                fprintf(stderr, "allocated=%d legal=%d %s\n", allocated, legal, error);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    bool completed = false;
    for (size_t allowance = 0; allowance < 500 && !completed; allowance++)
    {
        if (!run_case(ANVIL_ARCH_X86_64, ANVIL_LINK_INTERNAL, allowance, &completed))
            return 1;
    }

    if (!completed)
        return 1;

    for (anvil_arch_t arch = ANVIL_ARCH_X86; arch < ANVIL_ARCH_COUNT; arch++)
    {
        if (!run_case(arch, ANVIL_LINK_INTERNAL, SIZE_MAX, &completed) || !completed)
            return 1;
    }

    if (!run_case(ANVIL_ARCH_X86_64, ANVIL_LINK_EXTERNAL, SIZE_MAX, &completed) || !completed)
        return 1;

    if (!run_case(ANVIL_ARCH_X86_64, ANVIL_LINK_WEAK, SIZE_MAX, &completed) || !completed)
        return 1;

    return 0;
}
