#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool run_case(unsigned mode, size_t allowance, bool *completed)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "vector_contracts");
    anvil_type_t *element = anvil_type_f32(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, element);
    anvil_type_t *type = anvil_type_func(ctx, anvil_type_void(ctx), &pointer, 1, false);
    anvil_func_t *func = anvil_func_create(module, "pack_values", type, ANVIL_LINK_EXTERNAL);
    anvil_func_set_fp_vectorization(func, mode != 1);
    anvil_set_insert_point(ctx, func->entry);
    anvil_type_t *array = anvil_type_array(ctx, element, 16);
    anvil_value_t *storage = anvil_build_alloca(ctx, array, "storage");
    anvil_value_t *base = mode == 4 ? anvil_func_get_param(func, 0) : anvil_build_bitcast(ctx, storage, pointer, "base");
    anvil_memory_access_t access = { .is_volatile = mode == 2 };
    for (size_t lane = 0; lane < 4; lane++)
    {
        anvil_value_t *a = anvil_const_i64(ctx, (int64_t)lane);
        anvil_value_t *b = anvil_const_i64(ctx, (int64_t)lane + 4);
        anvil_value_t *c = anvil_const_i64(ctx, (int64_t)lane + (mode == 3 ? 2 : 8));
        anvil_value_t *left_address = anvil_build_gep(ctx, element, base, &a, 1, "left.address");
        anvil_value_t *right_address = anvil_build_gep(ctx, element, base, &b, 1, "right.address");
        anvil_value_t *destination = anvil_build_gep(ctx, element, base, &c, 1, "destination");
        anvil_value_t *left = anvil_build_load_ex(ctx, element, left_address, &access, "left");
        anvil_value_t *right = anvil_build_load(ctx, element, right_address, "right");
        anvil_value_t *value = anvil_build_fadd(ctx, left, right, "value");
        anvil_build_store(ctx, value, destination);
    }

    anvil_build_ret_void(ctx);
    if (allowance != SIZE_MAX)
        anvil_test_fail_alloc_after(ctx, allowance);

    anvil_pass_result_t status = anvil_pass_vectorize(func);
    anvil_test_disable_alloc_fail(ctx);
    anvil_ctx_clear_error(ctx);
    char error[256] = { 0 };
    bool valid = anvil_func_verify(func, error, sizeof(error));
    *completed = status != ANVIL_PASS_RUN_ERROR;
    if (*completed)
    {
        valid &= status == (mode == 0 ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
        char *assembly = NULL;
        size_t length = 0;
        valid &= anvil_module_codegen(module, &assembly, &length) == ANVIL_OK;
        if (mode == 0)
            valid &= assembly && strstr(assembly, "addps") && strstr(assembly, "movups");

        free(assembly);
    }

    if (!valid)
    {
        fprintf(stderr, "vector failure: mode=%u allowance=%zu status=%d error=%s context=%s\n", mode, allowance, status, error, anvil_ctx_get_error(ctx));
        anvil_dump_module(stderr, module);
    }

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    bool completed = false;
    for (size_t allowance = 0; allowance < 300 && !completed; allowance++)
    {
        if (!run_case(0, allowance, &completed))
            return 1;
    }

    if (!completed)
        return 1;

    for (unsigned mode = 1; mode < 5; mode++)
    {
        if (!run_case(mode, SIZE_MAX, &completed) || !completed)
            return 1;
    }

    return 0;
}
