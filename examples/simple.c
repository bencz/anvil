/*
 * ANVIL - Simple Example
 * 
 * Demonstrates basic usage of the ANVIL library to generate
 * a simple function that adds two numbers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anvil/anvil.h"

int main(int argc, char **argv)
{
    anvil_arch_t target = ANVIL_ARCH_X86_64;
    
    /* Parse command line for target architecture */
    if (argc > 1) {
        if (strcmp(argv[1], "x86") == 0) {
            target = ANVIL_ARCH_X86;
        } else if (strcmp(argv[1], "x86-64") == 0 || strcmp(argv[1], "x64") == 0) {
            target = ANVIL_ARCH_X86_64;
        } else if (strcmp(argv[1], "s370") == 0) {
            target = ANVIL_ARCH_S370;
        } else if (strcmp(argv[1], "s390") == 0) {
            target = ANVIL_ARCH_S390;
        } else if (strcmp(argv[1], "zarch") == 0 || strcmp(argv[1], "z") == 0) {
            target = ANVIL_ARCH_ZARCH;
        } else if (strcmp(argv[1], "ppc32") == 0 || strcmp(argv[1], "ppc") == 0) {
            target = ANVIL_ARCH_PPC32;
        } else if (strcmp(argv[1], "ppc64") == 0) {
            target = ANVIL_ARCH_PPC64;
        } else if (strcmp(argv[1], "ppc64le") == 0) {
            target = ANVIL_ARCH_PPC64LE;
        } else {
            fprintf(stderr, "Unknown target: %s\n", argv[1]);
            fprintf(stderr, "Available: x86, x86-64, s370, s390, zarch, ppc32, ppc64, ppc64le\n");
            return 1;
        }
    }
    
    /* Create context */
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Set target architecture */
    if (anvil_ctx_set_target(ctx, target) != ANVIL_OK) {
        fprintf(stderr, "Failed to set target: %s\n", anvil_ctx_get_error(ctx));
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Get architecture info */
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    printf("Target: %s (%d-bit, %s-endian, stack grows %s)\n",
           arch->name, arch->addr_bits,
           arch->endian == ANVIL_ENDIAN_LITTLE ? "little" : "big",
           arch->stack_dir == ANVIL_STACK_DOWN ? "down" : "up");
    
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
