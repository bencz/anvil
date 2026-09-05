#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>

static bool narrow_case(int value, bool modulo)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "signed_narrow");
    anvil_type_t *type = anvil_type_i8(ctx);
    anvil_type_t *function_type = anvil_type_func(ctx, type, NULL, 0, false);
    anvil_func_t *function = anvil_func_create(module, "calculate", function_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(function));
    anvil_value_t *left = anvil_const_i8(ctx, (int8_t)value);
    anvil_value_t *right = anvil_const_i8(ctx, 8);
    anvil_value_t *result = modulo ? anvil_build_smod(ctx, left, right, "remainder") : anvil_build_sdiv(ctx, left, right, "quotient");
    anvil_build_ret(ctx, result);
    bool valid = anvil_pass_strength_reduce(function) == ANVIL_PASS_RUN_CHANGED;
    valid &= anvil_pass_const_fold(function) == ANVIL_PASS_RUN_CHANGED;
    result = function->entry->last->operands[0];
    int expected = modulo ? value % 8 : value / 8;
    valid &= result->kind == ANVIL_VAL_CONST_INT && result->data.i == expected;
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

static bool allocation_failures(bool modulo)
{
    bool completed = false;
    for (size_t allowance = 0; allowance < 80 && !completed; allowance++)
    {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        anvil_module_t *module = anvil_module_create(ctx, "signed_failure");
        anvil_type_t *type = anvil_type_i64(ctx);
        anvil_type_t *function_type = anvil_type_func(ctx, type, &type, 1, false);
        anvil_func_t *function = anvil_func_create(module, "calculate", function_type, ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(function));
        anvil_value_t *left = anvil_func_get_param(function, 0);
        anvil_value_t *right = anvil_const_i64(ctx, 1024);
        anvil_value_t *result = modulo ? anvil_build_smod(ctx, left, right, "remainder") : anvil_build_sdiv(ctx, left, right, "quotient");
        anvil_build_ret(ctx, result);

        anvil_test_fail_alloc_after(ctx, allowance);
        anvil_pass_result_t status = anvil_pass_strength_reduce(function);
        anvil_test_disable_alloc_fail(ctx);
        anvil_ctx_clear_error(ctx);
        char error[256] = { 0 };
        bool valid = anvil_func_verify(function, error, sizeof(error));
        if (status == ANVIL_PASS_RUN_ERROR)
            valid &= result->data.instr->op == (modulo ? ANVIL_OP_SMOD : ANVIL_OP_SDIV);
        else
            completed = status == ANVIL_PASS_RUN_CHANGED;

        if (!valid)
            fprintf(stderr, "allocation failure: allowance=%zu modulo=%u status=%d error=%s\n", allowance, modulo, status, error);

        anvil_module_destroy(module);
        anvil_ctx_destroy(ctx);
        if (!valid)
            return false;
    }

    if (!completed)
        fprintf(stderr, "allocation test did not reach successful completion: modulo=%u\n", modulo);

    return completed;
}

int main(void)
{
    for (int value = -128; value <= 127; value++)
    {
        if (!narrow_case(value, false) || !narrow_case(value, true))
        {
            fprintf(stderr, "signed division reduction failed for %d\n", value);
            return 1;
        }
    }

    return allocation_failures(false) && allocation_failures(true) ? 0 : 1;
}
