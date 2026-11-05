#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>

static bool run_case(anvil_arch_t arch, int count, bool descending, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "unroll_contracts");
    anvil_type_t *integer = anvil_type_i32(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, integer);
    anvil_type_t *type = anvil_type_func(ctx, integer, &pointer, 1, false);
    anvil_func_t *func = anvil_func_create(module, "iterate", type, ANVIL_LINK_EXTERNAL);
    anvil_value_t *address = anvil_func_get_param(func, 0);
    anvil_block_t *header = anvil_block_create(func, "header");
    anvil_block_t *body = anvil_block_create(func, "body");
    anvil_block_t *exit = anvil_block_create(func, "exit");
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *bound = anvil_const_i32(ctx, count);
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, integer, "index");
    anvil_value_t *a = anvil_build_phi(ctx, integer, "a");
    anvil_value_t *b = anvil_build_phi(ctx, integer, "b");
    anvil_value_t *condition = descending ? anvil_build_cmp_gt(ctx, index, zero, "condition") : anvil_build_cmp_lt(ctx, index, bound, "condition");
    anvil_build_br_cond(ctx, condition, body, exit);

    anvil_set_insert_point(ctx, body);
    anvil_memory_access_t access = { .is_volatile = true };
    anvil_build_store_ex(ctx, a, address, &access);
    anvil_value_t *loaded = anvil_build_load_ex(ctx, integer, address, &access, "loaded");
    anvil_value_t *sum = anvil_build_add(ctx, loaded, b, "sum");
    anvil_value_t *next = descending ? anvil_build_sub(ctx, index, one, "next") : anvil_build_add(ctx, index, one, "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, descending ? bound : zero, func->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_phi_add_incoming(a, zero, func->entry);
    anvil_phi_add_incoming(a, b, body);
    anvil_phi_add_incoming(b, one, func->entry);
    anvil_phi_add_incoming(b, sum, body);
    anvil_set_insert_point(ctx, exit);
    anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
    anvil_build_ret(ctx, result);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);

    anvil_pass_result_t status = anvil_pass_unroll(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = { 0 };
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed)
    {
        bool eligible = count <= 8;
        valid &= status == (eligible ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        size_t loads = 0;
        size_t stores = 0;
        size_t phis = 0;
        for (anvil_block_t *block = func->blocks; block; block = block->next)
        {
            for (anvil_instr_t *instruction = block->first; instruction; instruction = instruction->next)
            {
                loads += instruction->op == ANVIL_OP_LOAD && instruction->memory_access.is_volatile;
                stores += instruction->op == ANVIL_OP_STORE && instruction->memory_access.is_volatile;
                phis += instruction->op == ANVIL_OP_PHI;
            }
        }

        valid &= loads == (eligible ? (size_t)count : 1) && stores == loads && phis == (eligible ? 0 : 3);
        valid &= anvil_pass_unroll(func) == ANVIL_PASS_RUN_UNCHANGED;
        anvil_pass_manager_t *manager = anvil_pass_manager_create(ctx);
        anvil_pass_manager_set_level(manager, ANVIL_OPT_AGGRESSIVE);
        valid &= anvil_pass_manager_run_module(manager, module) != ANVIL_PASS_RUN_ERROR;
        anvil_pass_manager_destroy(manager);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length != 0;
        free(assembly);
    }

    if (!valid)
    {
        fprintf(stderr, "unroll failure: arch=%d count=%d descending=%d allowance=%zu status=%d error=%s context=%s\n", arch, count, descending, allowance, status, error, anvil_ctx_get_error(ctx));
        anvil_dump_module(stderr, module);
    }

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

/* A narrow counter as C promotes it: i is u16 from 0 while < 16, the
 * header widens it (read by the body and after the loop), the step is
 * trunc(zext(i) + 4); or i is i8 from 8 while > -8, by trunc(sext(i) - 3).
 * Four or six iterations unroll; the value read after the loop is the
 * header's work once more, on the final counter (16, or -10). */
static bool run_narrow_case(anvil_arch_t arch, bool down, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "unroll_narrow");
    anvil_type_t *narrow = down ? anvil_type_i8(ctx) : anvil_type_u16(ctx), *i32 = anvil_type_i32(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, i32);
    anvil_func_t *func = anvil_func_create(module, "narrow", anvil_type_func(ctx, i32, &pointer, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_block_t *header = anvil_block_create(func, "header"), *body = anvil_block_create(func, "body"), *exit = anvil_block_create(func, "exit");
    anvil_set_insert_point(ctx, func->entry);
    anvil_build_br(ctx, header);
    anvil_set_insert_point(ctx, header);
    anvil_value_t *index = anvil_build_phi(ctx, narrow, "i");
    anvil_value_t *wide = down ? anvil_build_sext(ctx, index, i32, "wide") : anvil_build_zext(ctx, index, i32, "wide");
    anvil_value_t *more = down ? anvil_build_cmp_gt(ctx, index, anvil_const_i8(ctx, -8), "more") : anvil_build_cmp_ult(ctx, index, anvil_const_u16(ctx, 16), "more");
    anvil_build_br_cond(ctx, more, body, exit);
    anvil_set_insert_point(ctx, body);
    anvil_memory_access_t access = { .is_volatile = true };
    anvil_build_store_ex(ctx, wide, anvil_func_get_param(func, 0), &access);
    anvil_value_t *step = down ? anvil_build_sub(ctx, wide, anvil_const_i32(ctx, 3), "step") : anvil_build_add(ctx, wide, anvil_const_i32(ctx, 4), "step");
    anvil_value_t *next = anvil_build_trunc(ctx, step, narrow, "next");
    anvil_build_br(ctx, header);
    anvil_phi_add_incoming(index, down ? anvil_const_i8(ctx, 8) : anvil_const_u16(ctx, 0), func->entry);
    anvil_phi_add_incoming(index, next, body);
    anvil_set_insert_point(ctx, exit);
    anvil_build_ret(ctx, wide);

    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);
    anvil_pass_result_t status = anvil_pass_unroll(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = { 0 };
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed)
    {
        size_t stores = 0, phis = 0;
        for (anvil_block_t *block = func->blocks; block; block = block->next)
            for (anvil_instr_t *instruction = block->first; instruction; instruction = instruction->next)
            {
                stores += instruction->op == ANVIL_OP_STORE;
                phis += instruction->op == ANVIL_OP_PHI;
            }
        valid &= status == ANVIL_PASS_RUN_CHANGED && stores == (down ? 6u : 4u) && phis == 0;
        valid &= anvil_pass_const_fold(func) != ANVIL_PASS_RUN_ERROR;
        const anvil_value_t *returned = exit->last && exit->last->op == ANVIL_OP_RET ? exit->last->operands[0] : NULL;
        valid &= returned && returned->kind == ANVIL_VAL_CONST_INT && returned->data.i == (down ? -10 : 16);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK && length != 0;
        free(assembly);
    }
    if (!valid)
    {
        fprintf(stderr, "narrow unroll failure: arch=%d down=%d allowance=%zu status=%d error=%s\n", arch, down, allowance, status, error);
        anvil_dump_module(stderr, module);
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
        if (!run_case(ANVIL_ARCH_X86_64, 5, false, allowance, &completed))
            return 1;
    }

    if (!completed)
        return 1;

    completed = false;
    for (size_t allowance = 0; allowance < 500 && !completed; allowance++)
    {
        if (!run_narrow_case(ANVIL_ARCH_X86_64, false, allowance, &completed))
            return 1;
    }
    for (anvil_arch_t arch = ANVIL_ARCH_X86; arch < ANVIL_ARCH_COUNT; arch++)
    {
        for (int down = 0; down < 2; down++)
        {
            if (!run_narrow_case(arch, down != 0, SIZE_MAX, &completed) || !completed)
                return 1;
        }
    }

    for (anvil_arch_t arch = ANVIL_ARCH_X86; arch < ANVIL_ARCH_COUNT; arch++)
    {
        for (int count = 0; count <= 9; count++)
        {
            for (unsigned descending = 0; descending < 2; descending++)
            {
                if (!run_case(arch, count, descending != 0, SIZE_MAX, &completed) || !completed)
                    return 1;
            }
        }
    }

    return 0;
}
