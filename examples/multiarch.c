/*
 * ANVIL - Multi-Architecture Example
 * 
 * Demonstrates generating code for multiple architectures from the same IR.
 * Creates a factorial function and generates assembly for all supported targets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anvil/anvil.h"

/* Build factorial function IR - TRUE RECURSIVE implementation
 * 
 * int factorial(int n) {
 *     if (n <= 1) return 1;
 *     return n * factorial(n - 1);
 * }
 */
static anvil_func_t *build_factorial(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    /* Create function type: int factorial(int n) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
    
    /* Create function */
    anvil_func_t *func = anvil_func_create(mod, "factorial", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    /* Get function as a value for recursive call */
    anvil_value_t *func_val = anvil_func_get_value(func);
    
    /* Create basic blocks */
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_block_t *recurse = anvil_block_create(func, "recurse");
    anvil_block_t *base_case = anvil_block_create(func, "base_case");
    
    /* Entry block: check if n <= 1 */
    anvil_set_insert_point(ctx, entry);
    anvil_value_t *n = anvil_func_get_param(func, 0);
    anvil_value_t *one = anvil_const_i32(ctx, 1);
    anvil_value_t *cmp = anvil_build_cmp_le(ctx, n, one, "cmp");
    anvil_build_br_cond(ctx, cmp, base_case, recurse);
    
    /* Base case: return 1 */
    anvil_set_insert_point(ctx, base_case);
    anvil_build_ret(ctx, one);
    
    /* Recursive case: return n * factorial(n - 1) */
    anvil_set_insert_point(ctx, recurse);
    
    /* Compute n - 1 */
    anvil_value_t *n_minus_1 = anvil_build_sub(ctx, n, one, "n_minus_1");
    
    /* Call factorial(n - 1) recursively */
    anvil_value_t *call_args[] = { n_minus_1 };
    anvil_value_t *rec_result = anvil_build_call(ctx, i32, func_val, call_args, 1, "rec_result");
    
    /* Multiply n * factorial(n - 1) */
    anvil_value_t *product = anvil_build_mul(ctx, n, rec_result, "product");
    
    anvil_build_ret(ctx, product);
    
    return func;
}

/* Build a more complex function: sum of array */
static anvil_func_t *build_sum_array(anvil_ctx_t *ctx, anvil_module_t *mod)
{
    /* Create function type: int sum_array(int* arr, int len) */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *params[] = { ptr_i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    
    anvil_func_t *func = anvil_func_create(mod, "sum_array", func_type, ANVIL_LINK_EXTERNAL);
    if (!func) return NULL;
    
    anvil_block_t *entry = anvil_func_get_entry(func);
    
    anvil_set_insert_point(ctx, entry);
    
    /* Get parameters */
    anvil_value_t *arr = anvil_func_get_param(func, 0);
    anvil_value_t *len = anvil_func_get_param(func, 1);
    
    /* For simplicity, just load first element and return */
    /* A full implementation would have a loop */
    anvil_value_t *first = anvil_build_load(ctx, i32, arr, "first");
    
    /* Multiply by len as a simple operation */
    anvil_value_t *result = anvil_build_mul(ctx, first, len, "result");
    
    anvil_build_ret(ctx, result);
    
    return func;
}

int main(void)
{
    /* All supported architectures */
    struct {
        anvil_arch_t arch;
        const char *name;
        const char *filename;
    } targets[] = {
        // { ANVIL_ARCH_X86,     "x86 (32-bit)",         "output_x86.s" },
        // { ANVIL_ARCH_X86_64,  "x86-64 (64-bit)",      "output_x64.s" },
        { ANVIL_ARCH_S370,    "S/370 (24-bit)",       "output_s370.asm" },
        { ANVIL_ARCH_S390,    "S/390 (31-bit)",       "output_s390.asm" },
        { ANVIL_ARCH_ZARCH,   "z/Arch (64-bit)",      "output_zarch.asm" },
        // { ANVIL_ARCH_PPC32,   "PowerPC 32-bit",       "output_ppc32.s" },
        // { ANVIL_ARCH_PPC64,   "PowerPC 64-bit BE",    "output_ppc64.s" },
        // { ANVIL_ARCH_PPC64LE, "PowerPC 64-bit LE",    "output_ppc64le.s" },
        { ANVIL_ARCH_ARM64,   "ARM64 (AArch64)",      "output_arm64.s" }
    };
    
    size_t num_targets = sizeof(targets) / sizeof(targets[0]);
    
    printf("ANVIL Multi-Architecture Code Generator\n");
    printf("========================================\n\n");
    
    for (size_t i = 0; i < num_targets; i++) {
        printf("Generating for %s...\n", targets[i].name);
        
        /* Create fresh context for each target */
        anvil_ctx_t *ctx = anvil_ctx_create();
        if (!ctx) {
            fprintf(stderr, "  Failed to create context\n");
            continue;
        }
        
        /* Set target */
        if (anvil_ctx_set_target(ctx, targets[i].arch) != ANVIL_OK) {
            fprintf(stderr, "  Failed to set target: %s\n", anvil_ctx_get_error(ctx));
            anvil_ctx_destroy(ctx);
            continue;
        }
        
        /* Get and display architecture info */
        const anvil_arch_info_t *info = anvil_ctx_get_arch_info(ctx);
        printf("  Address bits: %d\n", info->addr_bits);
        printf("  Pointer size: %d bytes\n", info->ptr_size);
        printf("  Endianness: %s\n", info->endian == ANVIL_ENDIAN_LITTLE ? "little" : "big");
        printf("  Stack direction: %s\n", info->stack_dir == ANVIL_STACK_DOWN ? "down" : "up");
        printf("  GPRs: %d, FPRs: %d\n", info->num_gpr, info->num_fpr);
        
        /* Create module */
        anvil_module_t *mod = anvil_module_create(ctx, "multiarch");
        if (!mod) {
            fprintf(stderr, "  Failed to create module\n");
            anvil_ctx_destroy(ctx);
            continue;
        }
        
        /* Build functions */
        if (!build_factorial(ctx, mod)) {
            fprintf(stderr, "  Failed to build factorial function\n");
        }
        
        if (!build_sum_array(ctx, mod)) {
            fprintf(stderr, "  Failed to build sum_array function\n");
        }
        
        /* Generate and write code */
        char *output = NULL;
        size_t len = 0;
        
        if (anvil_module_codegen(mod, &output, &len) == ANVIL_OK) {
            printf("  Generated %zu bytes of assembly\n", len);
            
            /* Write to file */
            FILE *f = fopen(targets[i].filename, "w");
            if (f) {
                fwrite(output, 1, len, f);
                fclose(f);
                printf("  Written to %s\n", targets[i].filename);
            }
            
            /* Print first few lines */
            printf("  Preview:\n");
            char *line = output;
            int line_count = 0;
            while (*line && line_count < 10) {
                char *end = strchr(line, '\n');
                if (end) {
                    printf("    %.*s\n", (int)(end - line), line);
                    line = end + 1;
                } else {
                    printf("    %s\n", line);
                    break;
                }
                line_count++;
            }
            if (*line) printf("    ...\n");
            
            free(output);
        } else {
            fprintf(stderr, "  Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        }
        
        printf("\n");
        
        /* Cleanup */
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
    
    printf("Done!\n");
    return 0;
}
