/*
 * ANVIL Floating-Point Math Library Generator
 * 
 * This program generates assembly code for a math library with
 * floating-point functions (add, sub, mul, div) that can be
 * linked with C code.
 * 
 * The generated functions are:
 *   double fp_add(double a, double b);  // returns a + b
 *   double fp_sub(double a, double b);  // returns a - b
 *   double fp_mul(double a, double b);  // returns a * b
 *   double fp_div(double a, double b);  // returns a / b
 *   double fp_neg(double a);            // returns -a
 *   double fp_abs(double a);            // returns |a|
 * 
 * Usage: generate_math [arch] > math_lib.s
 *   arch: x86_64, arm64, arm64_macos, ppc32, ppc64, ppc64le, etc.
 * 
 * Then compile and link:
 *   # For x86_64 Linux:
 *   as math_lib.s -o math_lib.o
 *   gcc -c test_math.c -o test_math.o
 *   gcc test_math.o math_lib.o -o test_math -lm
 *   ./test_math
 * 
 *   # For ARM64 macOS:
 *   ./generate_math arm64_macos > math_lib.s
 *   as math_lib.s -o math_lib.o
 *   clang -c test_math.c -o test_math.o
 *   clang test_math.o math_lib.o -o test_math
 *   ./test_math
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arch_select.h"

/* Create a binary FP function: double func(double a, double b) */
static anvil_func_t *create_binary_fp_func(anvil_ctx_t *ctx, anvil_module_t *mod,
                                            const char *name,
                                            anvil_value_t *(*build_op)(anvil_ctx_t*, anvil_value_t*, anvil_value_t*, const char*))
{
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64, f64 };
    anvil_type_t *func_type = anvil_type_func(ctx, f64, params, 2, false);
    
    anvil_func_t *func = anvil_func_create(mod, name, func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    
    anvil_value_t *result = build_op(ctx, a, b, "result");
    anvil_build_ret(ctx, result);
    
    return func;
}

/* Create a unary FP function: double func(double a) */
static anvil_func_t *create_unary_fp_func(anvil_ctx_t *ctx, anvil_module_t *mod,
                                           const char *name,
                                           anvil_value_t *(*build_op)(anvil_ctx_t*, anvil_value_t*, const char*))
{
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64 };
    anvil_type_t *func_type = anvil_type_func(ctx, f64, params, 1, false);
    
    anvil_func_t *func = anvil_func_create(mod, name, func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *a = anvil_func_get_param(func, 0);
    
    anvil_value_t *result = build_op(ctx, a, "result");
    anvil_build_ret(ctx, result);
    
    return func;
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    /* Parse architecture arguments */
    if (!parse_arch_args(argc, argv, &config)) {
        return 1;
    }
    
    /* Create context */
    ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Setup architecture */
    if (!setup_arch_context(ctx, &config)) {
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Print info to stderr so stdout is clean for assembly */
    fprintf(stderr, "Generating math library for: %s\n", config.arch_name);
    
    /* Create module */
    anvil_module_t *mod = anvil_module_create(ctx, "fp_math_lib");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Create fp_add: double fp_add(double a, double b) { return a + b; } */
    if (!create_binary_fp_func(ctx, mod, "fp_add", anvil_build_fadd)) {
        fprintf(stderr, "Failed to create fp_add\n");
        goto error;
    }
    
    /* Create fp_sub: double fp_sub(double a, double b) { return a - b; } */
    if (!create_binary_fp_func(ctx, mod, "fp_sub", anvil_build_fsub)) {
        fprintf(stderr, "Failed to create fp_sub\n");
        goto error;
    }
    
    /* Create fp_mul: double fp_mul(double a, double b) { return a * b; } */
    if (!create_binary_fp_func(ctx, mod, "fp_mul", anvil_build_fmul)) {
        fprintf(stderr, "Failed to create fp_mul\n");
        goto error;
    }
    
    /* Create fp_div: double fp_div(double a, double b) { return a / b; } */
    if (!create_binary_fp_func(ctx, mod, "fp_div", anvil_build_fdiv)) {
        fprintf(stderr, "Failed to create fp_div\n");
        goto error;
    }
    
    /* Create fp_neg: double fp_neg(double a) { return -a; } */
    if (!create_unary_fp_func(ctx, mod, "fp_neg", anvil_build_fneg)) {
        fprintf(stderr, "Failed to create fp_neg\n");
        goto error;
    }
    
    /* Create fp_abs: double fp_abs(double a) { return fabs(a); } */
    if (!create_unary_fp_func(ctx, mod, "fp_abs", anvil_build_fabs)) {
        fprintf(stderr, "Failed to create fp_abs\n");
        goto error;
    }
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        /* Output assembly to stdout */
        printf("%s", output);
        free(output);
        fprintf(stderr, "Generated %zu bytes of assembly\n", len);
    } else {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        goto error;
    }
    
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;

error:
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 1;
}
