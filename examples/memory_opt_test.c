/*
 * ANVIL - Memory Optimization Test Example
 * 
 * Demonstrates the memory-related optimization passes:
 * - Copy Propagation
 * - Dead Store Elimination
 * - Redundant Load Elimination
 * 
 * Usage: memory_opt_test [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

/* Helper to print generated code */
static void print_code(anvil_module_t *mod, const char *title)
{
    char *output = NULL;
    size_t len = 0;
    
    if (anvil_module_codegen(mod, &output, &len) == ANVIL_OK) {
        printf("=== %s ===\n%s\n", title, output);
        free(output);
    }
}

/*
 * Test 1: Copy Propagation
 * 
 * int test_copy_prop(int x) {
 *     int y = x + 0;  // This is effectively y = x (copy)
 *     return y + 1;   // Should become x + 1
 * }
 */
static void test_copy_propagation(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 1: Copy Propagation\n");
    printf("========================================\n");
    printf("y = x + 0; return y + 1; -> return x + 1;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "copy_prop_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_copy_prop", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    
    /* y = x + 0 (effectively a copy) */
    anvil_value_t *y = anvil_build_add(ctx, x, zero, "y");
    
    /* result = y + 1 */
    anvil_value_t *result = anvil_build_add(ctx, y, one, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_BASIC);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (copy propagated)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 2: Dead Store Elimination
 * 
 * void test_dead_store(int *p) {
 *     *p = 1;  // Dead store - overwritten before read
 *     *p = 2;  // This store survives
 * }
 */
static void test_dead_store(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 2: Dead Store Elimination\n");
    printf("========================================\n");
    printf("*p = 1; *p = 2; -> *p = 2;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "dead_store_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *void_type = anvil_type_void(ctx);
    anvil_type_t *params[] = { ptr_i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, void_type, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_dead_store", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *p = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *two = anvil_const_i32(ctx, 2);
    
    /* *p = 1 (dead store) */
    anvil_build_store(ctx, one, p);
    
    /* *p = 2 (overwrites previous store) */
    anvil_build_store(ctx, two, p);
    
    anvil_build_ret_void(ctx);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (dead store eliminated)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 3: Redundant Load Elimination
 * 
 * int test_load_elim(int *p) {
 *     int x = *p;
 *     int y = *p;  // Redundant load - reuse x
 *     return x + y;
 * }
 */
static void test_load_elim(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 3: Redundant Load Elimination\n");
    printf("========================================\n");
    printf("x = *p; y = *p; return x + y; -> x = *p; return x + x;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "load_elim_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_load_elim", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *p = anvil_func_get_param(func, 0);
    
    /* x = *p */
    anvil_value_t *x = anvil_build_load(ctx, i32, p, "x");
    
    /* y = *p (redundant) */
    anvil_value_t *y = anvil_build_load(ctx, i32, p, "y");
    
    /* result = x + y */
    anvil_value_t *result = anvil_build_add(ctx, x, y, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (redundant load eliminated)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 4: Combined Optimizations
 * 
 * int test_combined(int *p) {
 *     *p = 10;     // Dead store
 *     *p = 20;     // Survives
 *     int x = *p;  // Load
 *     int y = *p;  // Redundant load
 *     int z = x + 0;  // Copy
 *     return z + y;   // Should become x + x
 * }
 */
static void test_combined(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 4: Combined Optimizations\n");
    printf("========================================\n");
    printf("Multiple optimizations working together\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "combined_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_combined", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *p = anvil_func_get_param(func, 0);
    anvil_value_t *ten = anvil_const_i32(ctx, 10);
    anvil_value_t *twenty = anvil_const_i32(ctx, 20);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    
    /* *p = 10 (dead store) */
    anvil_build_store(ctx, ten, p);
    
    /* *p = 20 */
    anvil_build_store(ctx, twenty, p);
    
    /* x = *p */
    anvil_value_t *x = anvil_build_load(ctx, i32, p, "x");
    
    /* y = *p (redundant) */
    anvil_value_t *y = anvil_build_load(ctx, i32, p, "y");
    
    /* z = x + 0 (copy) */
    anvil_value_t *z = anvil_build_add(ctx, x, zero, "z");
    
    /* result = z + y */
    anvil_value_t *result = anvil_build_add(ctx, z, y, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Memory Optimization Test");
    
    test_copy_propagation(ctx);
    test_dead_store(ctx);
    test_load_elim(ctx);
    test_combined(ctx);
    
    printf("\n=== Memory optimization tests completed ===\n");
    
    anvil_ctx_destroy(ctx);
    
    return 0;
}
