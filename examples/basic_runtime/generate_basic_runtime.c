/*
 * ANVIL Basic Runtime Generator
 *
 * Generates a small assembly module used by make test-examples. The generated
 * functions intentionally exercise stack slots, GEP, casts, select, FP
 * conversion, and dynamic alloca.
 */

#include <anvil/anvil.h>

#include <stdio.h>
#include <stdlib.h>

#include "../arch_select.h"

static bool create_memory_select_func(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *array_i64_4 = anvil_type_array(ctx, i64, 4);
    anvil_type_t *params[] = { i64, i64, i64, i8, f64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 5, false);
    anvil_func_t *fn = anvil_func_create(mod, "anvil_rt_memory_select",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    if (!fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *idx = anvil_func_get_param(fn, 0);
    anvil_value_t *a = anvil_func_get_param(fn, 1);
    anvil_value_t *b = anvil_func_get_param(fn, 2);
    anvil_value_t *tiny = anvil_func_get_param(fn, 3);
    anvil_value_t *fp = anvil_func_get_param(fn, 4);

    anvil_value_t *items = anvil_build_alloca(ctx, array_i64_4, "items");
    anvil_value_t *indices[] = { anvil_const_i64(ctx, 0), idx };
    anvil_value_t *slot =
        anvil_build_gep(ctx, array_i64_4, items, indices, 2, "slot");
    anvil_build_store(ctx, a, slot);

    anvil_value_t *loaded = anvil_build_load(ctx, i64, slot, "loaded");
    anvil_value_t *cond = anvil_build_cmp_gt(ctx, loaded, b, "gt");
    anvil_value_t *picked = anvil_build_select(ctx, cond, loaded, b, "picked");
    anvil_value_t *signed_wide = anvil_build_sext(ctx, tiny, i64, "signed_wide");
    anvil_value_t *unsigned_wide = anvil_build_zext(ctx, tiny, i64, "unsigned_wide");
    anvil_value_t *abs_fp = anvil_build_fabs(ctx, fp, "abs_fp");
    anvil_value_t *fp_int = anvil_build_fptosi(ctx, abs_fp, i64, "fp_int");
    anvil_value_t *acc0 = anvil_build_add(ctx, picked, signed_wide, "acc0");
    anvil_value_t *acc1 = anvil_build_add(ctx, acc0, unsigned_wide, "acc1");
    anvil_value_t *acc2 = anvil_build_add(ctx, acc1, fp_int, "acc2");
    anvil_build_ret(ctx, acc2);
    return true;
}

static bool create_dynamic_alloca_func(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "anvil_rt_dynamic_alloca",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    if (!fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *count = anvil_func_get_param(fn, 0);
    anvil_value_t *items = anvil_build_alloca_dyn(ctx, i64, count, "items");
    anvil_build_store(ctx, anvil_const_i64(ctx, 77), items);
    anvil_value_t *loaded = anvil_build_load(ctx, i64, items, "loaded");
    anvil_build_ret(ctx, loaded);
    return true;
}

static bool create_global_string_funcs(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *u8 = anvil_type_u8(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);

    anvil_value_t *seed = anvil_module_add_global(mod, "anvil_rt_global_seed",
                                                  i64, ANVIL_LINK_EXTERNAL);
    anvil_value_t *signed_byte = anvil_module_add_global(mod, "anvil_rt_sbyte",
                                                         i8, ANVIL_LINK_EXTERNAL);
    anvil_value_t *unsigned_byte = anvil_module_add_global(mod, "anvil_rt_ubyte",
                                                           u8, ANVIL_LINK_EXTERNAL);
    if (!seed || !signed_byte || !unsigned_byte) return false;

    anvil_global_set_initializer(seed, anvil_const_i64(ctx, 41));
    anvil_global_set_initializer(signed_byte, anvil_const_i8(ctx, -5));
    anvil_global_set_initializer(unsigned_byte, anvil_const_u8(ctx, 250));

    anvil_type_t *global_params[] = { i64 };
    anvil_type_t *global_fn_type = anvil_type_func(ctx, i64, global_params, 1, false);
    anvil_func_t *global_fn = anvil_func_create(mod, "anvil_rt_global_plus",
                                                global_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    if (!global_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(global_fn));
    anvil_value_t *x = anvil_func_get_param(global_fn, 0);
    anvil_value_t *loaded_seed = anvil_build_load(ctx, i64, seed, "loaded_seed");
    anvil_value_t *sum = anvil_build_add(ctx, loaded_seed, x, "sum");
    anvil_build_ret(ctx, sum);

    anvil_type_t *i8_ptr = anvil_type_ptr(ctx, i8);
    anvil_type_t *string_fn_type = anvil_type_func(ctx, i8_ptr, NULL, 0, false);
    anvil_func_t *string_fn = anvil_func_create(mod, "anvil_rt_string",
                                                string_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    if (!string_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(string_fn));
    anvil_build_ret(ctx, anvil_const_string(ctx, "anvil-mir"));

    anvil_type_t *signed_fn_type = anvil_type_func(ctx, i8, NULL, 0, false);
    anvil_func_t *signed_fn = anvil_func_create(mod, "anvil_rt_signed_byte",
                                                signed_fn_type,
                                                ANVIL_LINK_EXTERNAL);
    if (!signed_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(signed_fn));
    anvil_value_t *loaded_signed = anvil_build_load(ctx, i8, signed_byte,
                                                    "loaded_signed");
    anvil_build_ret(ctx, loaded_signed);

    anvil_type_t *unsigned_fn_type = anvil_type_func(ctx, u8, NULL, 0, false);
    anvil_func_t *unsigned_fn = anvil_func_create(mod, "anvil_rt_unsigned_byte",
                                                  unsigned_fn_type,
                                                  ANVIL_LINK_EXTERNAL);
    if (!unsigned_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(unsigned_fn));
    anvil_value_t *loaded_unsigned = anvil_build_load(ctx, u8, unsigned_byte,
                                                      "loaded_unsigned");
    anvil_build_ret(ctx, loaded_unsigned);

    return true;
}

static bool create_switch_func(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64 };
    anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 1, false);
    anvil_func_t *fn = anvil_func_create(mod, "anvil_rt_switch_pick",
                                         fn_type, ANVIL_LINK_EXTERNAL);
    if (!fn) return false;

    anvil_block_t *case_zero = anvil_block_create(fn, "case_zero");
    anvil_block_t *case_positive = anvil_block_create(fn, "case_positive");
    anvil_block_t *case_negative = anvil_block_create(fn, "case_negative");
    anvil_block_t *default_block = anvil_block_create(fn, "default");
    if (!case_zero || !case_positive || !case_negative || !default_block) {
        return false;
    }

    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_instr_t *sw = anvil_build_switch(ctx,
                                           anvil_func_get_param(fn, 0),
                                           default_block);
    if (!sw ||
        !anvil_switch_add_case(sw, anvil_const_i64(ctx, 0), case_zero) ||
        !anvil_switch_add_case(sw, anvil_const_i64(ctx, 7), case_positive) ||
        !anvil_switch_add_case(sw, anvil_const_i64(ctx, -3), case_negative)) {
        return false;
    }

    anvil_set_insert_point(ctx, case_zero);
    anvil_build_ret(ctx, anvil_const_i64(ctx, 10));

    anvil_set_insert_point(ctx, case_positive);
    anvil_build_ret(ctx, anvil_const_i64(ctx, 70));

    anvil_set_insert_point(ctx, case_negative);
    anvil_build_ret(ctx, anvil_const_i64(ctx, 33));

    anvil_set_insert_point(ctx, default_block);
    anvil_build_ret(ctx, anvil_const_i64(ctx, -1));
    return true;
}

static bool create_incoming_stack_arg_funcs(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *i64_params[10];
    for (size_t i = 0; i < 10; i++) {
        i64_params[i] = i64;
    }

    anvil_type_t *sum_type = anvil_type_func(ctx, i64, i64_params, 10, false);
    anvil_func_t *sum_fn = anvil_func_create(mod, "anvil_rt_stack_sum10",
                                             sum_type, ANVIL_LINK_EXTERNAL);
    if (!sum_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(sum_fn));
    anvil_value_t *acc = anvil_func_get_param(sum_fn, 0);
    for (size_t i = 1; i < 10; i++) {
        acc = anvil_build_add(ctx, acc, anvil_func_get_param(sum_fn, i), "acc");
    }
    anvil_build_ret(ctx, acc);

    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *f64_params[9];
    for (size_t i = 0; i < 9; i++) {
        f64_params[i] = f64;
    }

    anvil_type_t *fp_type = anvil_type_func(ctx, f64, f64_params, 9, false);
    anvil_func_t *fp_fn = anvil_func_create(mod, "anvil_rt_fp_stack_arg",
                                            fp_type, ANVIL_LINK_EXTERNAL);
    if (!fp_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(fp_fn));
    anvil_value_t *fp_sum = anvil_build_fadd(ctx,
                                             anvil_func_get_param(fp_fn, 0),
                                             anvil_func_get_param(fp_fn, 8),
                                             "fp_sum");
    anvil_build_ret(ctx, fp_sum);

    anvil_type_t *i8_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *ptr_params[9];
    for (size_t i = 0; i < 9; i++) {
        ptr_params[i] = i8_ptr;
    }

    anvil_type_t *ptr_type = anvil_type_func(ctx, i8_ptr, ptr_params, 9, false);
    anvil_func_t *ptr_fn = anvil_func_create(mod, "anvil_rt_ptr_stack_arg",
                                             ptr_type, ANVIL_LINK_EXTERNAL);
    if (!ptr_fn) return false;

    anvil_set_insert_point(ctx, anvil_func_get_entry(ptr_fn));
    anvil_build_ret(ctx, anvil_func_get_param(ptr_fn, 8));

    return true;
}

int main(int argc, char **argv)
{
    arch_config_t config;
    if (!parse_arch_args(argc, argv, &config)) {
        return 1;
    }

    anvil_ctx_t *ctx = anvil_ctx_create_for_target(config.arch);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    if (!setup_arch_context(ctx, &config)) {
        anvil_ctx_destroy(ctx);
        return 1;
    }

    anvil_module_t *mod = anvil_module_create(ctx, "basic_runtime");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }

    if (!create_memory_select_func(ctx, mod) ||
        !create_dynamic_alloca_func(ctx, mod) ||
        !create_global_string_funcs(ctx, mod) ||
        !create_switch_func(ctx, mod) ||
        !create_incoming_stack_arg_funcs(ctx, mod)) {
        fprintf(stderr, "Failed to create runtime functions\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }

    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    if (err != ANVIL_OK || !output) {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }

    (void)len;
    printf("%s", output);
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
