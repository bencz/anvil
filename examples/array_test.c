/*
 * ANVIL Array Example
 * 
 * Demonstrates array access using GEP (Get Element Pointer)
 * 
 * Equivalent C code:
 *   int sum_array(int *arr, int n) {
 *       int sum = 0;
 *       int i = 0;
 *       while (i < n) {
 *           sum = sum + arr[i];
 *           i = i + 1;
 *       }
 *       return sum;
 *   }
 * 
 * Usage: array_test [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch (default: s390)
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    anvil_arch_t arch = ANVIL_ARCH_S390;
    const char *arch_name = "S/390";
    
    if (argc > 1) {
        if (strcmp(argv[1], "x86") == 0) {
            arch = ANVIL_ARCH_X86;
            arch_name = "x86";
        } else if (strcmp(argv[1], "x86_64") == 0) {
            arch = ANVIL_ARCH_X86_64;
            arch_name = "x86-64";
        } else if (strcmp(argv[1], "s370") == 0) {
            arch = ANVIL_ARCH_S370;
            arch_name = "S/370";
        } else if (strcmp(argv[1], "s370_xa") == 0) {
            arch = ANVIL_ARCH_S370_XA;
            arch_name = "S/370-XA";
        } else if (strcmp(argv[1], "s390") == 0) {
            arch = ANVIL_ARCH_S390;
            arch_name = "S/390";
        } else if (strcmp(argv[1], "zarch") == 0) {
            arch = ANVIL_ARCH_ZARCH;
            arch_name = "z/Architecture";
        } else {
            fprintf(stderr, "Unknown architecture: %s\n", argv[1]);
            fprintf(stderr, "Supported: x86, x86_64, s370, s370_xa, s390, zarch\n");
            return 1;
        }
    }
    
    printf("=== ANVIL Array Example ===\n");
    printf("Target: %s\n\n", arch_name);
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    anvil_ctx_set_target(ctx, arch);
    
    anvil_module_t *mod = anvil_module_create(ctx, "array_test");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Types */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *i32_ptr = anvil_type_ptr(ctx, i32);
    
    /* Function: int sum_array(int *arr, int n) */
    anvil_type_t *params[] = { i32_ptr, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *func = anvil_func_create(mod, "sum_array", func_type, ANVIL_LINK_EXTERNAL);
    
    /* Create blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
    anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
    anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
    
    /* Get parameters */
    anvil_value_t *arr = anvil_func_get_param(func, 0);  /* int *arr */
    anvil_value_t *n = anvil_func_get_param(func, 1);    /* int n */
    
    /* Entry block: initialize sum=0, i=0 */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *sum_ptr = anvil_build_alloca(ctx, i32, "sum");
    anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_build_store(ctx, zero, sum_ptr);
    anvil_build_store(ctx, zero, i_ptr);
    anvil_build_br(ctx, loop_cond);
    
    /* Loop condition: while (i < n) */
    anvil_set_insert_point(ctx, loop_cond);
    anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
    anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, n, "cmp");
    anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
    
    /* Loop body: sum = sum + arr[i]; i = i + 1 */
    anvil_set_insert_point(ctx, loop_body);
    
    /* Load current values */
    anvil_value_t *sum_val = anvil_build_load(ctx, i32, sum_ptr, "sum_val");
    anvil_value_t *idx = anvil_build_load(ctx, i32, i_ptr, "idx");
    
    /* GEP: compute &arr[i] */
    anvil_value_t *indices[] = { idx };
    anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
    
    /* Load arr[i] */
    anvil_value_t *elem_val = anvil_build_load(ctx, i32, elem_ptr, "elem_val");
    
    /* sum = sum + arr[i] */
    anvil_value_t *new_sum = anvil_build_add(ctx, sum_val, elem_val, "new_sum");
    anvil_build_store(ctx, new_sum, sum_ptr);
    
    /* i = i + 1 */
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *new_i = anvil_build_add(ctx, idx, one, "new_i");
    anvil_build_store(ctx, new_i, i_ptr);
    
    anvil_build_br(ctx, loop_cond);
    
    /* Loop end: return sum */
    anvil_set_insert_point(ctx, loop_end);
    anvil_value_t *result = anvil_build_load(ctx, i32, sum_ptr, "result");
    anvil_build_ret(ctx, result);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err != ANVIL_OK) {
        fprintf(stderr, "Code generation failed: %d\n", err);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    printf("Generated %zu bytes of assembly:\n", len);
    printf("----------------------------------------\n");
    printf("%s\n", output);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
