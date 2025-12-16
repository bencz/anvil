/*
 * ANVIL Examples - Architecture Selection Utility
 * 
 * Common header for parsing command-line arguments to select
 * target architecture and floating-point format.
 * 
 * Usage in examples:
 *   #include "arch_select.h"
 *   
 *   int main(int argc, char **argv) {
 *       arch_config_t config;
 *       if (!parse_arch_args(argc, argv, &config)) {
 *           return 1;
 *       }
 *       
 *       anvil_ctx_t *ctx = anvil_ctx_create();
 *       if (!setup_arch_context(ctx, &config)) {
 *           anvil_ctx_destroy(ctx);
 *           return 1;
 *       }
 *       // ... use ctx ...
 *   }
 */

#ifndef ARCH_SELECT_H
#define ARCH_SELECT_H

#include <anvil/anvil.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Architecture configuration */
typedef struct {
    anvil_arch_t arch;
    const char *arch_name;
    anvil_fp_format_t fp_format;
    anvil_abi_t abi;
    bool fp_format_specified;
    bool abi_specified;
} arch_config_t;

/* Architecture entry for lookup table */
typedef struct {
    const char *name;
    anvil_arch_t arch;
    const char *display_name;
    anvil_fp_format_t default_fp_format;
} arch_entry_t;

/* Supported architectures table */
static const arch_entry_t arch_table[] = {
    { "x86",        ANVIL_ARCH_X86,      "x86 (32-bit)",          ANVIL_FP_IEEE754  },
    { "x86_64",     ANVIL_ARCH_X86_64,   "x86-64 (64-bit)",       ANVIL_FP_IEEE754  },
    { "s370",       ANVIL_ARCH_S370,     "IBM S/370 (24-bit)",    ANVIL_FP_HFP      },
    { "s370_xa",    ANVIL_ARCH_S370_XA,  "IBM S/370-XA (31-bit)", ANVIL_FP_HFP      },
    { "s390",       ANVIL_ARCH_S390,     "IBM S/390 (31-bit)",    ANVIL_FP_HFP      },
    { "zarch",      ANVIL_ARCH_ZARCH,    "IBM z/Architecture",    ANVIL_FP_HFP_IEEE },
    { "ppc32",      ANVIL_ARCH_PPC32,    "PowerPC 32-bit",        ANVIL_FP_IEEE754  },
    { "ppc64",      ANVIL_ARCH_PPC64,    "PowerPC 64-bit BE",     ANVIL_FP_IEEE754  },
    { "ppc64le",    ANVIL_ARCH_PPC64LE,  "PowerPC 64-bit LE",     ANVIL_FP_IEEE754  },
    { "arm64",      ANVIL_ARCH_ARM64,    "ARM64 (AArch64/Linux)", ANVIL_FP_IEEE754  },
    { "arm64_macos", ANVIL_ARCH_ARM64,   "ARM64 (Apple Silicon)", ANVIL_FP_IEEE754  },
    { NULL, 0, NULL, 0 }
};

/* Print usage information */
static inline void print_arch_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [arch] [fp_format]\n", prog_name);
    fprintf(stderr, "\nSupported architectures:\n");
    for (const arch_entry_t *e = arch_table; e->name; e++) {
        fprintf(stderr, "  %-10s - %s\n", e->name, e->display_name);
    }
    fprintf(stderr, "\nFP formats (for s390/zarch only):\n");
    fprintf(stderr, "  hfp        - IBM Hexadecimal Floating Point\n");
    fprintf(stderr, "  ieee       - IEEE 754 Binary Floating Point\n");
}

/* Find architecture by name */
static inline const arch_entry_t *find_arch(const char *name)
{
    for (const arch_entry_t *e = arch_table; e->name; e++) {
        if (strcmp(name, e->name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Parse command-line arguments for architecture selection
 * Returns true on success, false on error */
static inline bool parse_arch_args(int argc, char **argv, arch_config_t *config)
{
    /* Default to z/Architecture */
    config->arch = ANVIL_ARCH_ZARCH;
    config->arch_name = "IBM z/Architecture";
    config->fp_format = ANVIL_FP_HFP_IEEE;
    config->abi = ANVIL_ABI_DEFAULT;
    config->fp_format_specified = false;
    config->abi_specified = false;
    
    /* Parse architecture */
    if (argc > 1) {
        const arch_entry_t *entry = find_arch(argv[1]);
        if (!entry) {
            fprintf(stderr, "Unknown architecture: %s\n\n", argv[1]);
            print_arch_usage(argv[0]);
            return false;
        }
        config->arch = entry->arch;
        config->arch_name = entry->display_name;
        config->fp_format = entry->default_fp_format;
        
        /* Check for macOS ARM64 variant */
        if (strcmp(argv[1], "arm64_macos") == 0) {
            config->abi = ANVIL_ABI_DARWIN;
            config->abi_specified = true;
        }
    }
    
    /* Parse FP format (optional) */
    if (argc > 2) {
        config->fp_format_specified = true;
        if (strcmp(argv[2], "hfp") == 0) {
            config->fp_format = ANVIL_FP_HFP;
        } else if (strcmp(argv[2], "ieee") == 0) {
            config->fp_format = ANVIL_FP_IEEE754;
        } else {
            fprintf(stderr, "Unknown FP format: %s\n", argv[2]);
            fprintf(stderr, "Available: hfp, ieee\n");
            return false;
        }
    }
    
    return true;
}

/* Set up context with architecture configuration
 * Returns true on success, false on error */
static inline bool setup_arch_context(anvil_ctx_t *ctx, const arch_config_t *config)
{
    if (!ctx) return false;
    
    /* Set target architecture */
    if (anvil_ctx_set_target(ctx, config->arch) != ANVIL_OK) {
        fprintf(stderr, "Failed to set target: %s\n", anvil_ctx_get_error(ctx));
        return false;
    }
    
    /* Set ABI if specified (e.g., Darwin for macOS ARM64) */
    if (config->abi_specified) {
        anvil_error_t err = anvil_ctx_set_abi(ctx, config->abi);
        if (err != ANVIL_OK) {
            fprintf(stderr, "Failed to set ABI: %s\n", anvil_ctx_get_error(ctx));
            return false;
        }
    }
    
    /* Set FP format if specified */
    if (config->fp_format_specified) {
        anvil_error_t err = anvil_ctx_set_fp_format(ctx, config->fp_format);
        if (err != ANVIL_OK) {
            fprintf(stderr, "Failed to set FP format: %s\n", anvil_ctx_get_error(ctx));
            return false;
        }
    }
    
    return true;
}

/* Get appropriate file extension for architecture */
static inline const char *get_file_extension(anvil_arch_t arch)
{
    switch (arch) {
        case ANVIL_ARCH_S370:
        case ANVIL_ARCH_S370_XA:
        case ANVIL_ARCH_S390:
        case ANVIL_ARCH_ZARCH:
            return ".hlasm";  /* HLASM */
        default:
            return ".s";      /* GAS */
    }
}

/* Print FP format information */
static inline void print_fp_format(anvil_fp_format_t fp_format)
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

/* Print architecture information */
static inline void print_arch_info(anvil_ctx_t *ctx, const arch_config_t *config)
{
    const anvil_arch_info_t *info = anvil_ctx_get_arch_info(ctx);
    
    printf("Target: %s\n", config->arch_name);
    printf("  Address bits: %d\n", info->addr_bits);
    printf("  Pointer size: %d bytes\n", info->ptr_size);
    printf("  Endianness: %s\n", info->endian == ANVIL_ENDIAN_LITTLE ? "little" : "big");
    printf("  Stack direction: %s\n", info->stack_dir == ANVIL_STACK_DOWN ? "down" : "up");
    printf("  GPRs: %d, FPRs: %d\n", info->num_gpr, info->num_fpr);
    print_fp_format(anvil_ctx_get_fp_format(ctx));
}

/* Convenience macro for common example setup */
#define EXAMPLE_SETUP(argc, argv, ctx, config, title) \
    do { \
        printf("=== %s ===\n", title); \
        if (!parse_arch_args(argc, argv, &config)) { \
            return 1; \
        } \
        ctx = anvil_ctx_create(); \
        if (!ctx) { \
            fprintf(stderr, "Failed to create context\n"); \
            return 1; \
        } \
        if (!setup_arch_context(ctx, &config)) { \
            anvil_ctx_destroy(ctx); \
            return 1; \
        } \
        print_arch_info(ctx, &config); \
        printf("\n"); \
    } while (0)

#endif /* ARCH_SELECT_H */
