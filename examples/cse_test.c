/*
 * ANVIL - Common Subexpression Elimination (CSE) Test
 * 
 * Demonstrates the CSE optimization pass.
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

/*
 * Test 1: Basic CSE
 * 
 * int test_cse(int x, int y) {
 *     int a = x + y;
 *     int b = x + y;  // Same computation - should reuse 'a'
 *     return a * b;
 * }
 */
static void test_basic_cse(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 1: Basic CSE\n");
    printf("========================================\n");
    printf("a = x + y; b = x + y; return a * b;\n");
    printf("Should become: a = x + y; return a * a;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "cse_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(mod, "test_cse", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *y = anvil_func_get_param(func, 1);
    
    /* a = x + y */
    anvil_value_t *a = anvil_build_add(ctx, x, y, "a");
    
    /* b = x + y (common subexpression) */
    anvil_value_t *b = anvil_build_add(ctx, x, y, "b");
    
    /* result = a * b */
    anvil_value_t *result = anvil_build_mul(ctx, a, b, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (CSE applied)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 2: Commutative CSE
 * 
 * int test_commutative(int x, int y) {
 *     int a = x + y;
 *     int b = y + x;  // Same as x + y (commutative)
 *     return a + b;
 * }
 */
static void test_commutative_cse(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 2: Commutative CSE\n");
    printf("========================================\n");
    printf("a = x + y; b = y + x; return a + b;\n");
    printf("Should become: a = x + y; return a + a;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "commutative_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(mod, "test_commutative", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *y = anvil_func_get_param(func, 1);
    
    /* a = x + y */
    anvil_value_t *a = anvil_build_add(ctx, x, y, "a");
    
    /* b = y + x (same as x + y due to commutativity) */
    anvil_value_t *b = anvil_build_add(ctx, y, x, "b");
    
    /* result = a + b */
    anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (commutative CSE)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 3: Multiple CSE opportunities
 * 
 * int test_multiple(int x, int y, int z) {
 *     int a = x * y;
 *     int b = y * z;
 *     int c = x * y;  // Same as 'a'
 *     int d = y * z;  // Same as 'b'
 *     return a + b + c + d;
 * }
 */
static void test_multiple_cse(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 3: Multiple CSE Opportunities\n");
    printf("========================================\n");
    printf("a = x*y; b = y*z; c = x*y; d = y*z;\n");
    printf("Should eliminate c and d\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "multiple_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 3, false);
    anvil_func_t *func = anvil_func_create(mod, "test_multiple", func_type, ANVIL_LINK_EXTERNAL);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *y = anvil_func_get_param(func, 1);
    anvil_value_t *z = anvil_func_get_param(func, 2);
    
    /* a = x * y */
    anvil_value_t *a = anvil_build_mul(ctx, x, y, "a");
    
    /* b = y * z */
    anvil_value_t *b = anvil_build_mul(ctx, y, z, "b");
    
    /* c = x * y (same as a) */
    anvil_value_t *c = anvil_build_mul(ctx, x, y, "c");
    
    /* d = y * z (same as b) */
    anvil_value_t *d = anvil_build_mul(ctx, y, z, "d");
    
    /* result = a + b + c + d */
    anvil_value_t *t1 = anvil_build_add(ctx, a, b, "t1");
    anvil_value_t *t2 = anvil_build_add(ctx, c, d, "t2");
    anvil_value_t *result = anvil_build_add(ctx, t1, t2, "result");
    
    anvil_build_ret(ctx, result);
    
    print_code(mod, "Before Optimization");
    
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

int main(void)
{
    printf("ANVIL Common Subexpression Elimination Test\n");
    printf("============================================\n");
    printf("Target: IBM S/390\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_S390);
    
    test_basic_cse(ctx);
    test_commutative_cse(ctx);
    test_multiple_cse(ctx);
    
    printf("\n=== CSE tests completed ===\n");
    
    anvil_ctx_destroy(ctx);
    
    return 0;
}
