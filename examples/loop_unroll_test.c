/*
 * ANVIL - Loop Unrolling Test Example
 * 
 * Demonstrates the loop unrolling optimization pass.
 * Tests both full unrolling (small trip count) and partial unrolling.
 * 
 * Usage: loop_unroll_test [arch]
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
 * Test 1: Simple loop with constant bounds (full unroll candidate)
 * 
 * int sum_4() {
 *     int sum = 0;
 *     for (int i = 0; i < 4; i++) {
 *         sum += i;
 *     }
 *     return sum;  // Should be 0+1+2+3 = 6
 * }
 */
static void test_full_unroll(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 1: Full Loop Unrolling (trip count = 4)\n");
    printf("========================================\n");
    printf("Loop: for (i = 0; i < 4; i++) sum += i;\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "full_unroll_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *func_type = anvil_type_func(ctx, i32, NULL, 0, false);
    anvil_func_t *func = anvil_func_create(mod, "sum_4", func_type, ANVIL_LINK_EXTERNAL);
    
    /* Create blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *loop_header = anvil_block_create(func, "loop");
    anvil_block_t *loop_body = anvil_block_create(func, "body");
    anvil_block_t *loop_exit = anvil_block_create(func, "exit");
    
    /* Entry block: initialize and jump to loop */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *four = anvil_const_i32(ctx, 4);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_build_br(ctx, loop_header);
    
    /* Loop header: PHI nodes and condition check */
    anvil_set_insert_point(ctx, loop_header);
    anvil_value_t *i_phi = anvil_build_phi(ctx, i32, "i");
    anvil_value_t *sum_phi = anvil_build_phi(ctx, i32, "sum");
    anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_phi, four, "cmp");
    anvil_build_br_cond(ctx, cmp, loop_body, loop_exit);
    
    /* Loop body: sum += i; i++ */
    anvil_set_insert_point(ctx, loop_body);
    anvil_value_t *new_sum = anvil_build_add(ctx, sum_phi, i_phi, "new_sum");
    anvil_value_t *new_i = anvil_build_add(ctx, i_phi, one, "new_i");
    anvil_build_br(ctx, loop_header);
    
    /* Add PHI incoming values */
    anvil_phi_add_incoming(i_phi, zero, entry);
    anvil_phi_add_incoming(i_phi, new_i, loop_body);
    anvil_phi_add_incoming(sum_phi, zero, entry);
    anvil_phi_add_incoming(sum_phi, new_sum, loop_body);
    
    /* Exit block: return sum */
    anvil_set_insert_point(ctx, loop_exit);
    anvil_build_ret(ctx, sum_phi);
    
    print_code(mod, "Before Optimization (with loop)");
    
    /* Enable O3 optimization (includes loop unrolling) */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_AGGRESSIVE);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (loop should be unrolled)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 2: Array sum with parameter (shows loop structure)
 * 
 * int sum_array(int *arr, int n) {
 *     int sum = 0;
 *     for (int i = 0; i < n; i++) {
 *         sum += arr[i];
 *     }
 *     return sum;
 * }
 */
static void test_array_sum(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 2: Array Sum Loop (variable bound)\n");
    printf("========================================\n");
    printf("Loop: for (i = 0; i < n; i++) sum += arr[i];\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "array_sum_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(mod, "sum_array", func_type, ANVIL_LINK_EXTERNAL);
    
    /* Create blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *loop_header = anvil_block_create(func, "loop");
    anvil_block_t *loop_body = anvil_block_create(func, "body");
    anvil_block_t *loop_exit = anvil_block_create(func, "exit");
    
    /* Entry block */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *arr = anvil_func_get_param(func, 0);
    anvil_value_t *n = anvil_func_get_param(func, 1);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_build_br(ctx, loop_header);
    
    /* Loop header */
    anvil_set_insert_point(ctx, loop_header);
    anvil_value_t *i_phi = anvil_build_phi(ctx, i32, "i");
    anvil_value_t *sum_phi = anvil_build_phi(ctx, i32, "sum");
    anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_phi, n, "cmp");
    anvil_build_br_cond(ctx, cmp, loop_body, loop_exit);
    
    /* Loop body */
    anvil_set_insert_point(ctx, loop_body);
    anvil_value_t *indices[] = { i_phi };
    anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
    anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
    anvil_value_t *new_sum = anvil_build_add(ctx, sum_phi, elem, "new_sum");
    anvil_value_t *new_i = anvil_build_add(ctx, i_phi, one, "new_i");
    anvil_build_br(ctx, loop_header);
    
    /* PHI incoming */
    anvil_phi_add_incoming(i_phi, zero, entry);
    anvil_phi_add_incoming(i_phi, new_i, loop_body);
    anvil_phi_add_incoming(sum_phi, zero, entry);
    anvil_phi_add_incoming(sum_phi, new_sum, loop_body);
    
    /* Exit */
    anvil_set_insert_point(ctx, loop_exit);
    anvil_build_ret(ctx, sum_phi);
    
    print_code(mod, "Before Optimization");
    
    /* Enable O3 optimization */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_AGGRESSIVE);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

/*
 * Test 3: Simple multiplication by repeated addition
 * 
 * int mul_by_8(int x) {
 *     int result = 0;
 *     for (int i = 0; i < 8; i++) {
 *         result += x;
 *     }
 *     return result;  // Should be x * 8
 * }
 */
static void test_mul_loop(anvil_ctx_t *ctx)
{
    printf("\n========================================\n");
    printf("Test 3: Multiplication Loop (trip count = 8)\n");
    printf("========================================\n");
    printf("Loop: for (i = 0; i < 8; i++) result += x;\n");
    printf("This computes x * 8 via repeated addition.\n\n");
    
    anvil_module_t *mod = anvil_module_create(ctx, "mul_loop_test");
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    anvil_func_t *func = anvil_func_create(mod, "mul_by_8", func_type, ANVIL_LINK_EXTERNAL);
    
    /* Create blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *loop_header = anvil_block_create(func, "loop");
    anvil_block_t *loop_body = anvil_block_create(func, "body");
    anvil_block_t *loop_exit = anvil_block_create(func, "exit");
    
    /* Entry block */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *x = anvil_func_get_param(func, 0);
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_value_t *eight = anvil_const_i32(ctx, 8);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_build_br(ctx, loop_header);
    
    /* Loop header */
    anvil_set_insert_point(ctx, loop_header);
    anvil_value_t *i_phi = anvil_build_phi(ctx, i32, "i");
    anvil_value_t *result_phi = anvil_build_phi(ctx, i32, "result");
    anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_phi, eight, "cmp");
    anvil_build_br_cond(ctx, cmp, loop_body, loop_exit);
    
    /* Loop body */
    anvil_set_insert_point(ctx, loop_body);
    anvil_value_t *new_result = anvil_build_add(ctx, result_phi, x, "new_result");
    anvil_value_t *new_i = anvil_build_add(ctx, i_phi, one, "new_i");
    anvil_build_br(ctx, loop_header);
    
    /* PHI incoming */
    anvil_phi_add_incoming(i_phi, zero, entry);
    anvil_phi_add_incoming(i_phi, new_i, loop_body);
    anvil_phi_add_incoming(result_phi, zero, entry);
    anvil_phi_add_incoming(result_phi, new_result, loop_body);
    
    /* Exit */
    anvil_set_insert_point(ctx, loop_exit);
    anvil_build_ret(ctx, result_phi);
    
    print_code(mod, "Before Optimization");
    
    /* Enable O3 optimization */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_AGGRESSIVE);
    anvil_module_optimize(mod);
    
    print_code(mod, "After Optimization (8 additions unrolled)");
    
    anvil_module_destroy(mod);
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_NONE);
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Loop Unrolling Test");
    
    /* Run tests */
    test_full_unroll(ctx);
    
    printf("\n=== Loop unrolling tests completed ===\n");
    
    anvil_ctx_destroy(ctx);
    
    return 0;
}
