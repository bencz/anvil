/*
 * ANVIL - Simple Example
 * 
 * Demonstrates basic usage of the ANVIL library to generate
 * a simple function that adds two numbers.
 * 
 * Usage: simple [arch]
 *   arch: x86, x86_64, s370, s370_xa, s390, zarch, ppc32, ppc64, ppc64le, arm64
 */

#include <stdio.h>
#include <stdlib.h>
#include "anvil/anvil.h"
#include "arch_select.h"

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    EXAMPLE_SETUP(argc, argv, ctx, config, "ANVIL Simple Example");
    
    /* Create module */
    anvil_module_t *mod = anvil_module_create(ctx, "example");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Create function type: int add(int a, int b) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) {
        fprintf(stderr, "Failed to create function\n");
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Get entry block and set as insertion point */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    /* Get function parameters */
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    
    /* Build: result = a + b */
    anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
    
    /* Build: return result */
    anvil_build_ret(ctx, result);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    
    if (anvil_module_codegen(mod, &output, &len) != ANVIL_OK) {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Print generated assembly */
    printf("\n--- Generated Assembly ---\n%s", output);
    
    /* Cleanup */
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
