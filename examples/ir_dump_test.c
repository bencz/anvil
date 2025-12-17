/*
 * ANVIL - IR Dump Test
 * 
 * Tests the IR dump functionality for debugging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anvil/anvil.h"

/* Build a simple function to test IR dump */
static anvil_func_t *build_test_func(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    /* Create function type: int test(int a, int b) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(mod, "test_func", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    /* Create basic blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *then_block = anvil_block_create(func, "then");
    anvil_block_t *else_block = anvil_block_create(func, "else");
    anvil_block_t *merge = anvil_block_create(func, "merge");
    
    /* Entry block */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    
    /* Compare a > b */
    anvil_value_t *cmp = anvil_build_cmp_gt(ctx, a, b, "cmp");
    anvil_build_br_cond(ctx, cmp, then_block, else_block);
    
    /* Then block: return a + b */
    anvil_set_insert_point(ctx, then_block);
    anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
    anvil_build_br(ctx, merge);
    
    /* Else block: return a - b */
    anvil_set_insert_point(ctx, else_block);
    anvil_value_t *diff = anvil_build_sub(ctx, a, b, "diff");
    anvil_build_br(ctx, merge);
    
    /* Merge block: use PHI to select result */
    anvil_set_insert_point(ctx, merge);
    anvil_value_t *phi = anvil_build_phi(ctx, i32, "result");
    anvil_phi_add_incoming(phi, sum, then_block);
    anvil_phi_add_incoming(phi, diff, else_block);
    anvil_build_ret(ctx, phi);
    
    return func;
}

/* Build factorial function */
static anvil_func_t *build_factorial(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    
    anvil_func_t *func = anvil_func_create(mod, "factorial", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    anvil_value_t *func_val = anvil_func_get_value(func);
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *recurse = anvil_block_create(func, "recurse");
    anvil_block_t *base_case = anvil_block_create(func, "base_case");
    
    /* Entry: check if n <= 1 */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *n = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *cmp = anvil_build_cmp_le(ctx, n, one, "cmp");
    anvil_build_br_cond(ctx, cmp, base_case, recurse);
    
    /* Base case: return 1 */
    anvil_set_insert_point(ctx, base_case);
    anvil_build_ret(ctx, one);
    
    /* Recursive case */
    anvil_set_insert_point(ctx, recurse);
    anvil_value_t *n_minus_1 = anvil_build_sub(ctx, n, one, "n_minus_1");
    anvil_value_t *call_args[] = { n_minus_1 };
    anvil_value_t *rec_result = anvil_build_call(ctx, i32, func_val, call_args, 1, "rec_result");
    anvil_value_t *product = anvil_build_mul(ctx, n, rec_result, "product");
    anvil_build_ret(ctx, product);
    
    return func;
}

int main(void)
{
    printf("ANVIL IR Dump Test\n");
    printf("==================\n\n");
    
    /* Create context */
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Set target to ARM64 */
    if (anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64) != ANVIL_OK) {
        fprintf(stderr, "Failed to set target\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Create module */
    anvil_module_t *mod = anvil_module_create(ctx, "test_module");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Add an external function declaration */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *printf_params[] = { ptr_i8 };
    anvil_type_t *printf_type = anvil_type_func(ctx, i32, printf_params, 1, true);
    anvil_module_add_extern(mod, "printf", printf_type);
    
    /* Add a global variable */
    anvil_value_t *global = anvil_module_add_global(mod, "counter", i32, ANVIL_LINK_EXTERNAL);
    anvil_global_set_initializer(global, anvil_const_i32(ctx, 42));
    
    /* Build functions */
    build_test_func(ctx, mod);
    build_factorial(ctx, mod);
    
    /* Dump the IR */
    printf("=== IR Dump ===\n\n");
    anvil_print_module(mod);
    
    /* Also dump to string and show length */
    char *ir_str = anvil_module_to_string(mod);
    if (ir_str) {
        printf("\n=== IR String Length: %zu bytes ===\n", strlen(ir_str));
        free(ir_str);
    }
    
    /* Generate ARM64 code */
    printf("\n=== Generated ARM64 Assembly ===\n\n");
    char *asm_output = NULL;
    size_t asm_len = 0;
    
    if (anvil_module_codegen(mod, &asm_output, &asm_len) == ANVIL_OK) {
        printf("%s", asm_output);
        free(asm_output);
    } else {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
    }
    
    /* Cleanup */
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    printf("\nDone!\n");
    return 0;
}
