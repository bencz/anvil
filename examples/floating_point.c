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
 *   arch: s370, s370_xa, s390, zarch, x86, x86_64, ppc32, ppc64, ppc64le
 *   fp_format: hfp, ieee (only for s390 and zarch)
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_fp_format_info(anvil_fp_format_t fp_format)
{
    switch (fp_format) {
        case ANVIL_FP_IEEE754:
            printf("  FP Format: IEEE 754 (binary floating-point)\n");
            break;
        case ANVIL_FP_HFP:
            printf("  FP Format: IBM HFP (hexadecimal floating-point)\n");
            break;
        case ANVIL_FP_HFP_IEEE:
            printf("  FP Format: HFP + IEEE 754 (both supported)\n");
            break;
    }
}

int main(int argc, char **argv)
{
    anvil_arch_t arch = ANVIL_ARCH_ZARCH;
    const char *arch_name = "z/Architecture";
    anvil_fp_format_t fp_format = ANVIL_FP_HFP_IEEE;  /* Default for zarch */
    bool fp_format_specified = false;
    
    /* Parse architecture */
    if (argc > 1) {
        if (strcmp(argv[1], "s370") == 0) {
            arch = ANVIL_ARCH_S370;
            arch_name = "S/370";
            fp_format = ANVIL_FP_HFP;
        } else if (strcmp(argv[1], "s370_xa") == 0) {
            arch = ANVIL_ARCH_S370_XA;
            arch_name = "S/370-XA";
            fp_format = ANVIL_FP_HFP;
        } else if (strcmp(argv[1], "s390") == 0) {
            arch = ANVIL_ARCH_S390;
            arch_name = "S/390";
            fp_format = ANVIL_FP_HFP;  /* Default, but can be changed */
        } else if (strcmp(argv[1], "zarch") == 0) {
            arch = ANVIL_ARCH_ZARCH;
            arch_name = "z/Architecture";
            fp_format = ANVIL_FP_HFP_IEEE;
        } else if (strcmp(argv[1], "x86") == 0) {
            arch = ANVIL_ARCH_X86;
            arch_name = "x86";
            fp_format = ANVIL_FP_IEEE754;
        } else if (strcmp(argv[1], "x86_64") == 0) {
            arch = ANVIL_ARCH_X86_64;
            arch_name = "x86-64";
            fp_format = ANVIL_FP_IEEE754;
        } else if (strcmp(argv[1], "ppc32") == 0) {
            arch = ANVIL_ARCH_PPC32;
            arch_name = "PowerPC 32-bit";
            fp_format = ANVIL_FP_IEEE754;
        } else if (strcmp(argv[1], "ppc64") == 0) {
            arch = ANVIL_ARCH_PPC64;
            arch_name = "PowerPC 64-bit";
            fp_format = ANVIL_FP_IEEE754;
        } else if (strcmp(argv[1], "ppc64le") == 0) {
            arch = ANVIL_ARCH_PPC64LE;
            arch_name = "PowerPC 64-bit LE";
            fp_format = ANVIL_FP_IEEE754;
        } else {
            fprintf(stderr, "Unknown architecture: %s\n", argv[1]);
            fprintf(stderr, "Available: s370, s370_xa, s390, zarch, x86, x86_64, ppc32, ppc64, ppc64le\n");
            return 1;
        }
    }
    
    /* Parse FP format (optional, only for s390 and zarch) */
    if (argc > 2) {
        fp_format_specified = true;
        if (strcmp(argv[2], "hfp") == 0) {
            fp_format = ANVIL_FP_HFP;
        } else if (strcmp(argv[2], "ieee") == 0) {
            fp_format = ANVIL_FP_IEEE754;
        } else {
            fprintf(stderr, "Unknown FP format: %s\n", argv[2]);
            fprintf(stderr, "Available: hfp, ieee\n");
            return 1;
        }
    }
    
    printf("=== ANVIL Floating-Point Example ===\n");
    printf("Target: %s\n", arch_name);
    
    /* Create context */
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Set target architecture */
    if (anvil_ctx_set_target(ctx, arch) != ANVIL_OK) {
        fprintf(stderr, "Failed to set target: %s\n", anvil_ctx_get_error(ctx));
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Set FP format if specified */
    if (fp_format_specified) {
        anvil_error_t err = anvil_ctx_set_fp_format(ctx, fp_format);
        if (err != ANVIL_OK) {
            fprintf(stderr, "Failed to set FP format: %s\n", anvil_ctx_get_error(ctx));
            anvil_ctx_destroy(ctx);
            return 1;
        }
    }
    
    /* Get and display architecture info */
    const anvil_arch_info_t *info = anvil_ctx_get_arch_info(ctx);
    printf("  Address bits: %d\n", info->addr_bits);
    printf("  Endianness: %s\n", info->endian == ANVIL_ENDIAN_LITTLE ? "little" : "big");
    printf("  FPRs: %d\n", info->num_fpr);
    print_fp_format_info(anvil_ctx_get_fp_format(ctx));
    printf("\n");
    
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
