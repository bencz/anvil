/*
 * ANVIL Floating-Point Example
 * 
 * Demonstrates floating-point operations for mainframe architectures.
 * Shows how to select between HFP and IEEE 754 formats.
 * 
 * Equivalent C code:
 *   double compute(double a, double b) {
 *       double sum = a + b;
 *       double product = a * b;
 *       double result = sum / product;
 *       return result;
 *   }
 * 
 * Usage: floating_point [arch] [fp_format]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 *   fp_format: hfp, ieee (only for s390 and zarch)
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include "arch_select.h"

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Floating-Point Example");
    
    /* Create module */
    anvil_module_t *mod = anvil_module_create(ctx, "fptest");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Create function type: double compute(double a, double b) */
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64, f64 };
    anvil_type_t *func_type = anvil_type_func(ctx, f64, params, 2, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(mod, "compute", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) {
        fprintf(stderr, "Failed to create function\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Get entry block and set insert point */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    /* Get parameters */
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    
    /* Build: sum = a + b */
    anvil_value_t *sum = anvil_build_fadd(ctx, a, b, "sum");
    
    /* Build: product = a * b */
    anvil_value_t *product = anvil_build_fmul(ctx, a, b, "product");
    
    /* Build: result = sum / product */
    anvil_value_t *result = anvil_build_fdiv(ctx, sum, product, "result");
    
    /* Build: return result */
    anvil_build_ret(ctx, result);
    
    /* ================================================================
     * Create a second function demonstrating more FP operations
     * float simple_calc(float x)
     * {
     *     float neg = -x;
     *     float abs_val = fabs(neg);
     *     return abs_val;
     * }
     * ================================================================ */
    
    anvil_type_t *f32 = anvil_type_f32(ctx);
    anvil_type_t *params2[] = { f32 };
    anvil_type_t *func_type2 = anvil_type_func(ctx, f32, params2, 1, false);
    
    anvil_func_t *func2 = anvil_func_create(mod, "simple_calc", func_type2, ANVIL_LINK_EXTERNAL);
    if (!func2) {
        fprintf(stderr, "Failed to create function\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_block_t *entry2 = anvil_func_get_entry(func2);
    anvil_set_insert_point(ctx, entry2);
    
    anvil_value_t *x = anvil_func_get_param(func2, 0);
    
    /* Build: neg = -x */
    anvil_value_t *neg = anvil_build_fneg(ctx, x, "neg");
    
    /* Build: abs_val = fabs(neg) */
    anvil_value_t *abs_val = anvil_build_fabs(ctx, neg, "abs_val");
    
    /* Build: return abs_val */
    anvil_build_ret(ctx, abs_val);
    
    /* ================================================================
     * Create a third function demonstrating int<->float conversion
     * int float_to_int(double d)
     * {
     *     return (int)d;
     * }
     * ================================================================ */
    
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params3[] = { f64 };
    anvil_type_t *func_type3 = anvil_type_func(ctx, i32, params3, 1, false);
    
    anvil_func_t *func3 = anvil_func_create(mod, "float_to_int", func_type3, ANVIL_LINK_EXTERNAL);
    if (!func3) {
        fprintf(stderr, "Failed to create function\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_block_t *entry3 = anvil_func_get_entry(func3);
    anvil_set_insert_point(ctx, entry3);
    
    anvil_value_t *d = anvil_func_get_param(func3, 0);
    
    /* Build: result = (int)d */
    anvil_value_t *int_result = anvil_build_fptosi(ctx, d, i32, "int_result");
    
    /* Build: return int_result */
    anvil_build_ret(ctx, int_result);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("Generated %zu bytes of assembly:\n", len);
        printf("----------------------------------------\n");
        printf("%s\n", output);
        free(output);
    } else {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
