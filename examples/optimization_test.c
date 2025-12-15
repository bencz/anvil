/*
 * ANVIL - Optimization Test Example
 * 
 * Demonstrates the optimization pass infrastructure.
 * Shows constant folding, dead code elimination, and strength reduction.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>
#include <stdio.h>
#include <stdlib.h>

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

/* Test constant folding: compute 3 + 5 at compile time */
static void test_const_fold(anvil_ctx_t *ctx)
{
    printf("\n--- Test: Constant Folding ---\n");
    printf("Expression: 3 + 5 should be folded to 8\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "const_fold_test");
    
    /* Create function: int test() { return 3 + 5; } */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *func_type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(mod, "test_const_fold", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    /* Build: 3 + 5 (should be folded to 8) */
    anvil_value_t *c3 = anvil_const_i32(ctx, 3);
    anvil_value_t *c5 = anvil_const_i32(ctx, 5);
    anvil_value_t *sum = anvil_build_add(ctx, c3, c5, "sum");
    anvil_build_ret(ctx, sum);
    
    print_code(mod, "Before Optimization");
    
    /* Enable optimization and run */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_BASIC);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/* Test strength reduction: x * 8 -> x << 3 */
static void test_strength_reduce(anvil_ctx_t *ctx)
{
    printf("\n--- Test: Strength Reduction ---\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "strength_reduce_test");
    
    /* Create function: int test(int x) { return x * 8; } */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_strength", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *c8 = anvil_const_i32(ctx, 8);
    
    /* Build: x * 8 (should become x << 3) */
    anvil_value_t *result = anvil_build_mul(ctx, x, c8, "result");
    anvil_build_ret(ctx, result);
    
    printf("Before optimization:\n");
    print_code(mod, "Unoptimized");
    
    /* Enable optimization and run */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    printf("After optimization (strength reduction: mul -> shl):\n");
    print_code(mod, "Optimized");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/* Test algebraic identities: x + 0, x * 1, x & x */
static void test_identities(anvil_ctx_t *ctx)
{
    printf("\n--- Test: Algebraic Identities ---\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "identity_test");
    
    /* Create function: int test(int x) { return (x + 0) * 1; } */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "test_identity", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *c0 = anvil_const_i32(ctx, 0);
    anvil_value_t *c1 = anvil_const_i32(ctx, 1);
    
    /* Build: (x + 0) * 1 (should simplify to just x) */
    anvil_value_t *add_zero = anvil_build_add(ctx, x, c0, "add_zero");
    anvil_value_t *mul_one = anvil_build_mul(ctx, add_zero, c1, "mul_one");
    anvil_build_ret(ctx, mul_one);
    
    printf("Before optimization:\n");
    print_code(mod, "Unoptimized");
    
    /* Enable optimization and run */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_BASIC);
    anvil_module_optimize(mod);
    
    printf("After optimization (x + 0 -> x, x * 1 -> x):\n");
    print_code(mod, "Optimized");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/* Test with different architectures (only mainframe backends are complete) */
static void test_multiarch(void)
{
    printf("\n--- Test: Multi-Architecture Optimization ---\n");
    
    anvil_arch_t archs[] = { ANVIL_ARCH_S370, ANVIL_ARCH_S390, ANVIL_ARCH_ZARCH };
    const char *arch_names[] = { "S/370", "S/390", "z/Architecture" };
    
    for (int i = 0; i < 3; i++) {
        printf("\n=== Architecture: %s ===\n", arch_names[i]);
        
        anvil_ctx_t *ctx = anvil_ctx_create();
        anvil_ctx_set_target(ctx, archs[i]);
        
        anvil_module_t *mod = anvil_module_create(ctx, "multiarch_test");
        
        /* Create function: int double_it(int x) { return x * 2; } */
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "double_it", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *x = anvil_func_get_param(func, 0);
        anvil_value_t *c2 = anvil_const_i32(ctx, 2);
        anvil_value_t *result = anvil_build_mul(ctx, x, c2, "result");
        anvil_build_ret(ctx, result);
        
        /* Optimize: x * 2 -> x << 1 */
        anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
        anvil_module_optimize(mod);
        
        print_code(mod, "Optimized (x * 2 -> x << 1)");
        
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

int main(void)
{
    printf("ANVIL Optimization Test\n");
    printf("========================\n");
    
    /* Create context with S/390 target (mainframe backends are complete) */
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_S390);
    
    /* Run tests */
    test_const_fold(ctx);
    test_strength_reduce(ctx);
    test_identities(ctx);
    
    anvil_ctx_destroy(ctx);
    
    /* Multi-architecture test (mainframe only) */
    test_multiarch();
    
    printf("\n=== All optimization tests completed ===\n");
    
    return 0;
}
