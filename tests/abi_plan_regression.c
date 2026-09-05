#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    if (!ctx || anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64) != ANVIL_OK)
        return 1;

    const size_t sizes[] = { 1, 2, 3, 4, 5, 8, 16, 24, 83 };
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
    {
        size_t size = sizes[index];
        anvil_type_t *field = anvil_type_array(ctx, anvil_type_u8(ctx), size);
        anvil_type_t *record = anvil_type_literal_struct(ctx, &field, 1, false);
        bool direct = size == 1 || size == 2 || size == 4 || size == 8;
        for (unsigned result = 0; result < 2; result++)
        {
            anvil_abi_value_plan_t plan;
            if (anvil_abi_classify_value(ctx, record, result != 0, &plan) != ANVIL_OK ||
                plan.kind != (direct ? ANVIL_ABI_VALUE_INTEGER : ANVIL_ABI_VALUE_INDIRECT) ||
                (direct && (!anvil_type_is_integer(plan.transport_type) || anvil_type_size(plan.transport_type) != size)) ||
                (!direct && (!anvil_type_is_pointer(plan.transport_type) || plan.temporary_alignment != 16)))
            {
                fprintf(stderr, "incorrect Win64 aggregate plan: size=%zu return=%u\n", size, result);
                return 1;
            }
        }
    }

    anvil_abi_value_plan_t plan;
    anvil_type_t *real = anvil_type_f64(ctx);
    if (anvil_abi_classify_value(ctx, real, false, &plan) != ANVIL_OK ||
        plan.kind != ANVIL_ABI_VALUE_DIRECT || plan.transport_type != real ||
        anvil_abi_classify_value(ctx, anvil_type_void(ctx), false, &plan) != ANVIL_ERR_INVALID_TYPE ||
        anvil_abi_classify_value(ctx, anvil_type_void(ctx), true, &plan) != ANVIL_OK)
        return 1;

    if (anvil_build_va_start(ctx, "invalid") != NULL)
        return 1;

    anvil_ctx_clear_error(ctx);
    anvil_module_t *module = anvil_module_create(ctx, "varargs_api");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *function_type = anvil_type_func(ctx, i32, &i32, 1, true);
    anvil_func_t *function = anvil_func_create(module, "first_argument", function_type, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(function));
    anvil_value_t *cursor = anvil_build_va_start(ctx, "cursor");
    anvil_value_t *storage = anvil_build_alloca(ctx, anvil_type_ptr(ctx, anvil_type_i8(ctx)), "storage");
    if (!cursor || !anvil_build_store(ctx, cursor, storage))
        return 1;

    anvil_value_t *argument = anvil_build_va_arg(ctx, storage, i32, "argument");
    if (!argument || !anvil_build_ret(ctx, anvil_build_load(ctx, i32, argument, "value")))
        return 1;

    char *assembly = NULL;
    size_t length = 0;
    if (anvil_module_codegen(module, &assembly, &length) != ANVIL_OK || !length)
        return 1;

    free(assembly);
    anvil_module_destroy(module);

    anvil_ctx_destroy(ctx);
    ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_type_t *field = anvil_type_i64(ctx);
    anvil_type_t *record = anvil_type_literal_struct(ctx, &field, 1, false);
    if (anvil_abi_classify_value(ctx, record, false, &plan) != ANVIL_ERR_INVALID_TYPE)
        return 1;

    anvil_ctx_destroy(ctx);
    return 0;
}
