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

/* (x op1 c1) op2 c2 folds to x +- c: for every x of an 8-bit type the
 * folded form gives the same value modulo 256; constants that cancel leave
 * x itself; no-wrap survives only when both had it and the combined
 * constant fits the type. */
static bool chain_case(anvil_arch_t arch, bool is_signed, bool first_sub, int c1, bool second_sub, int c2, bool no_wrap)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    anvil_module_t *module = anvil_module_create(ctx, "constant_chain");
    anvil_type_t *type = is_signed ? anvil_type_i8(ctx) : anvil_type_u8(ctx);
    anvil_func_t *function = anvil_func_create(module, "chain", anvil_type_func(ctx, type, &type, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(function));
    anvil_value_t *x = anvil_func_get_param(function, 0);
    anvil_value_t *first_constant = is_signed ? anvil_const_i8(ctx, (int8_t)c1) : anvil_const_u8(ctx, (uint8_t)c1);
    anvil_value_t *second_constant = is_signed ? anvil_const_i8(ctx, (int8_t)c2) : anvil_const_u8(ctx, (uint8_t)c2);
    anvil_value_t *first = first_sub ? anvil_build_sub(ctx, x, first_constant, "first") : anvil_build_add(ctx, x, first_constant, "first");
    anvil_set_no_wrap(first, no_wrap);
    anvil_value_t *second = second_sub ? anvil_build_sub(ctx, first, second_constant, "second") : anvil_build_add(ctx, first, second_constant, "second");
    anvil_set_no_wrap(second, no_wrap);
    anvil_build_ret(ctx, second);
    bool valid = anvil_pass_const_fold(function) == ANVIL_PASS_RUN_CHANGED;
    const anvil_value_t *result = function->entry->last->operands[0];
    int v1 = (int)(is_signed ? (int8_t)c1 : (uint8_t)c1), v2 = (int)(is_signed ? (int8_t)c2 : (uint8_t)c2);
    int total = (first_sub ? -v1 : v1) + (second_sub ? -v2 : v2);
    if (result == x)
    {
        valid &= (total & 0xff) == 0;
    }
    else
    {
        const anvil_instr_t *folded = result->kind == ANVIL_VAL_INSTR ? result->data.instr : NULL;
        valid &= folded && folded->operands[0] == x && folded->operands[1]->kind == ANVIL_VAL_CONST_INT &&
                 (folded->op == ANVIL_OP_ADD || folded->op == ANVIL_OP_SUB);
        if (valid)
        {
            int constant = (int)(folded->operands[1]->data.i & 0xff);
            for (int value = 0; value < 256; value++)
                valid &= ((value + (folded->op == ANVIL_OP_SUB ? -constant : constant)) & 0xff) == ((value + total) & 0xff);
            int magnitude = total < 0 ? -total : total;
            bool fits = magnitude <= (is_signed ? 127 : 255);
            /* With a zero constant the identity leaves one operation, its flag kept. */
            valid &= v1 && v2 ? folded->no_wrap == (no_wrap && fits) : !folded->no_wrap || no_wrap;
        }
    }
    char error[256] = { 0 };
    valid &= anvil_func_verify(function, error, sizeof(error));
    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

static bool constant_chains(void)
{
    static const int constants[] = { 0, 1, 3, 5, 100, 127, 128, 200, 255 };
    const size_t count = sizeof(constants) / sizeof(constants[0]);
    for (anvil_arch_t arch = ANVIL_ARCH_X86; arch < ANVIL_ARCH_COUNT; arch++)
        for (int mode = 0; mode < 32; mode++)
            for (size_t a = 0; a < count; a++)
                for (size_t b = 0; b < count; b++)
                {
                    if (arch != ANVIL_ARCH_X86_64 && (a != 2 || b != 2))
                        continue;
                    if (!chain_case(arch, mode & 1, (mode >> 1) & 1, constants[a], (mode >> 2) & 1, constants[b], (mode >> 3) & 1))
                    {
                        fprintf(stderr, "constant chain failed: arch=%d mode=%d c1=%d c2=%d\n", (int)arch, mode, constants[a], constants[b]);
                        return false;
                    }
                }
    return true;
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

    return allocation_failures(false) && allocation_failures(true) && constant_chains() ? 0 : 1;
}
