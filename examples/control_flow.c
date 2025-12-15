/*
 * ANVIL Control Flow Example
 * 
 * Demonstrates IF/ELSE and loops using ANVIL IR
 * 
 * Equivalent C code:
 *   int sum_to_n(int n) {
 *       int sum = 0;
 *       int i = 1;
 *       while (i <= n) {
 *           sum = sum + i;
 *           i = i + 1;
 *       }
 *       return sum;
 *   }
 * 
 * Usage: control_flow [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Control Flow Example");
    
    anvil_module_t *mod = anvil_module_create(ctx, "ctrlflow");
    
    /* Create function: int sum_to_n(int n) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    
    anvil_func_t *func = anvil_func_create(mod, "sum_to_n", func_type, ANVIL_LINK_EXTERNAL);
    anvil_value_t *n_param = anvil_func_get_param(func, 0);
    
    /* Create basic blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
    anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
    anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
    
    /* Entry block: initialize sum=0, i=1 */
    anvil_set_insert_point(ctx, entry);
    
    /* Allocate local variables */
    anvil_value_t *sum_ptr = anvil_build_alloca(ctx, i32, "sum");
    anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
    
    /* sum = 0 */
    anvil_value_t *zero = anvil_const_i32(ctx, 0);
    anvil_build_store(ctx, zero, sum_ptr);
    
    /* i = 1 */
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_build_store(ctx, one, i_ptr);
    
    /* Jump to loop condition */
    anvil_build_br(ctx, loop_cond);
    
    /* Loop condition: while (i <= n) */
    anvil_set_insert_point(ctx, loop_cond);
    anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
    anvil_value_t *cmp = anvil_build_cmp_le(ctx, i_val, n_param, "cmp");
    anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
    
    /* Loop body: sum = sum + i; i = i + 1 */
    anvil_set_insert_point(ctx, loop_body);
    anvil_value_t *sum_val = anvil_build_load(ctx, i32, sum_ptr, "sum_val");
    anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
    anvil_value_t *new_sum = anvil_build_add(ctx, sum_val, i_val2, "new_sum");
    anvil_build_store(ctx, new_sum, sum_ptr);
    
    anvil_value_t *new_i = anvil_build_add(ctx, i_val2, one, "new_i");
    anvil_build_store(ctx, new_i, i_ptr);
    
    /* Jump back to condition */
    anvil_build_br(ctx, loop_cond);
    
    /* Loop end: return sum */
    anvil_set_insert_point(ctx, loop_end);
    anvil_value_t *result = anvil_build_load(ctx, i32, sum_ptr, "result");
    anvil_build_ret(ctx, result);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s\n", output);
        free(output);
    } else {
        fprintf(stderr, "Code generation failed\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
