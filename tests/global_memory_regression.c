#include <anvil/anvil_internal.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>

static bool test_paths(unsigned variant)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    anvil_module_t *module = anvil_module_create(ctx, "global_memory");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32), anvil_type_i1(ctx) };
    anvil_type_t *type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *function = anvil_func_create(module, "read_paths", type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(function);
    anvil_block_t *left = anvil_block_create(function, "left");
    anvil_block_t *right = anvil_block_create(function, "right");
    anvil_block_t *join = anvil_block_create(function, "join");
    anvil_value_t *pointer = anvil_func_get_param(function, 0);
    anvil_value_t *condition = anvil_func_get_param(function, 1);
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *first;
    if (variant == 6)
    {
        first = anvil_const_i32(ctx, 17);
        anvil_build_store(ctx, first, pointer);
    }
    else
    {
        first = anvil_build_load(ctx, i32, pointer, "first");
    }
    anvil_build_br_cond(ctx, condition, left, right);

    anvil_set_insert_point(ctx, left);
    if (variant == 1)
        anvil_build_store(ctx, anvil_const_i32(ctx, 23), pointer);
    if (variant == 2)
    {
        anvil_memory_access_t access = { .is_volatile = true };
        anvil_build_load_ex(ctx, i32, pointer, &access, "observed");
    }
    if (variant == 3)
    {
        anvil_type_t *callee_type = anvil_type_func(ctx, anvil_type_void(ctx), NULL, 0, false);
        anvil_func_t *callee = anvil_func_declare(module, "readonly", callee_type);
        anvil_func_set_effects(callee, ANVIL_EFFECT_READ_MEMORY);
        anvil_value_t *unused = NULL;
        anvil_build_call_checked(ctx, anvil_func_get_value(callee), NULL, 0, "call", &unused);
    }
    anvil_build_br(ctx, join);
    anvil_set_insert_point(ctx, right);
    anvil_build_br(ctx, join);

    anvil_set_insert_point(ctx, join);
    anvil_value_t *second = anvil_build_load(ctx, i32, pointer, "second");
    anvil_block_t *exit = join;
    if (variant == 4 || variant == 5)
    {
        if (variant == 5)
            anvil_build_store(ctx, anvil_const_i32(ctx, 99), pointer);

        exit = anvil_block_create(function, "exit");
        anvil_build_br_cond(ctx, condition, left, exit);
        anvil_set_insert_point(ctx, exit);
    }
    anvil_build_ret(ctx, anvil_build_add(ctx, first, second, "sum"));

    bool removable = variant == 0 || variant == 3 || variant == 4 || variant == 6;
    anvil_pass_result_t status = anvil_pass_load_elim(function);
    bool valid = status == (removable ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED);
    valid &= (second->data.instr->parent == NULL) == removable;
    char error[256] = { 0 };
    valid &= anvil_func_verify(function, error, sizeof(error));
    if (!valid)
        fprintf(stderr, "global memory variant=%u status=%d error=%s\n", variant, status, error);

    anvil_module_destroy(module);
    anvil_ctx_destroy(ctx);
    return valid;
}

int main(void)
{
    for (unsigned variant = 0; variant < 7; variant++)
    {
        if (!test_paths(variant))
            return 1;
    }

    return 0;
}
