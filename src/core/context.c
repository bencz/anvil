/*
 * ANVIL - Context implementation
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


/* Architecture information table */
static const anvil_arch_info_t arch_info_table[ANVIL_ARCH_COUNT] = {
    [ANVIL_ARCH_X86] = {
        .arch = ANVIL_ARCH_X86,
        .name = "x86",
        .ptr_size = 4,
        .addr_bits = 32,
        .word_size = 4,
        .num_gpr = 8,
        .num_fpr = 8,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_X86_64] = {
        .arch = ANVIL_ARCH_X86_64,
        .name = "x86-64",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S370] = {
        .arch = ANVIL_ARCH_S370,
        .name = "S/370",
        .ptr_size = 4,
        .addr_bits = 24,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 4,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* IBM Hexadecimal FP only */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S370_XA] = {
        .arch = ANVIL_ARCH_S370_XA,
        .name = "S/370-XA",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 4,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* IBM Hexadecimal FP only */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S390] = {
        .arch = ANVIL_ARCH_S390,
        .name = "S/390",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* HFP default, some models have IEEE */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_ZARCH] = {
        .arch = ANVIL_ARCH_ZARCH,
        .name = "z/Architecture",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP_IEEE,      /* Both HFP and IEEE 754 supported */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC32] = {
        .arch = ANVIL_ARCH_PPC32,
        .name = "PowerPC 32-bit",
        .ptr_size = 4,
        .addr_bits = 32,
        .word_size = 4,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC64] = {
        .arch = ANVIL_ARCH_PPC64,
        .name = "PowerPC 64-bit",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC64LE] = {
        .arch = ANVIL_ARCH_PPC64LE,
        .name = "PowerPC 64-bit LE",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_ARM64] = {
        .arch = ANVIL_ARCH_ARM64,
        .name = "ARM64",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 31,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,               /* Default to Linux, can be changed to DARWIN */
        .has_condition_codes = true,
        .has_delay_slots = false
    }
};

anvil_ctx_t *anvil_ctx_create(void)
{
    anvil_ctx_t *ctx = calloc(1, sizeof(anvil_ctx_t));
    if (!ctx) return NULL;
    
    /* Default to x86-64 */
    ctx->arch = ANVIL_ARCH_X86_64;
    ctx->output = ANVIL_OUTPUT_ASM;
    ctx->syntax = ANVIL_SYNTAX_DEFAULT;
    
    /* Initialize type cache */
    anvil_type_init_sizes(ctx);
    
    /* Initialize backends */
    anvil_init_backends();
    
    return ctx;
}

void anvil_ctx_destroy(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    
    /* Reset backend state FIRST (while IR values are still valid)
     * This clears any cached pointers to anvil_value_t in stack_slots/strings */
    if (ctx->backend) {
        if (ctx->backend->ops && ctx->backend->ops->reset) {
            ctx->backend->ops->reset(ctx->backend);
        }
    }
    
    /* Destroy all modules (frees all IR values) */
    anvil_module_t *mod = ctx->modules;
    while (mod) {
        anvil_module_t *next = mod->next;
        anvil_module_destroy(mod);
        mod = next;
    }
    
    /* Cleanup backend (now safe - no dangling pointers) */
    if (ctx->backend) {
        if (ctx->backend->ops && ctx->backend->ops->cleanup) {
            ctx->backend->ops->cleanup(ctx->backend);
        }
        free(ctx->backend);
    }
    
    /* Cleanup pass manager */
    if (ctx->pass_manager) {
        anvil_pass_manager_destroy(ctx->pass_manager);
    }

    /* Free every type registered against this context. */
    anvil_type_t *type = ctx->types;
    while (type) {
        anvil_type_t *next = type->ctx_next;
        anvil_type_free(type);
        type = next;
    }

    free(ctx);
}

anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if (arch >= ANVIL_ARCH_COUNT) return ANVIL_ERR_INVALID_ARG;

    if (ctx->backend) {
        if (ctx->backend->ops && ctx->backend->ops->reset) {
            ctx->backend->ops->reset(ctx->backend);
        }
        if (ctx->backend->ops && ctx->backend->ops->cleanup) {
            ctx->backend->ops->cleanup(ctx->backend);
        }
        free(ctx->backend);
        ctx->backend = NULL;
    }
    
    ctx->arch = arch;
    
    /* Set default FP format based on architecture */
    ctx->fp_format = arch_info_table[arch].fp_format;
    
    /* Set default ABI based on architecture */
    ctx->abi = arch_info_table[arch].abi;
    
    /* Set default CPU model for this architecture */
    ctx->cpu_model = anvil_arch_default_cpu(arch);
    ctx->features_enabled = 0;
    ctx->features_disabled = 0;
    anvil_update_cpu_features(ctx);
    
    /* Update type sizes for new architecture */
    anvil_type_init_sizes(ctx);
    
    /* Get backend for this architecture */
    ctx->backend = anvil_get_backend(ctx, arch);
    if (!ctx->backend) {
        anvil_set_error(ctx, ANVIL_ERR_NO_BACKEND, 
                        "No backend available for architecture %s",
                        arch_info_table[arch].name);
        return ANVIL_ERR_NO_BACKEND;
    }
    
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_output(anvil_ctx_t *ctx, anvil_output_t output)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->output = output;
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_syntax(anvil_ctx_t *ctx, anvil_syntax_t syntax)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->syntax = syntax;
    if (ctx->backend) {
        ctx->backend->syntax = syntax;
    }
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_abi(anvil_ctx_t *ctx, anvil_abi_t abi)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->abi = abi;
    return ANVIL_OK;
}

anvil_abi_t anvil_ctx_get_abi(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_ABI_DEFAULT;
    return ctx->abi;
}

anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    /* Validate FP format for the current architecture */
    const anvil_arch_info_t *arch_info = &arch_info_table[ctx->arch];
    
    switch (ctx->arch) {
        case ANVIL_ARCH_S370:
        case ANVIL_ARCH_S370_XA:
            /* S/370 and S/370-XA only support HFP */
            if (fp_format != ANVIL_FP_HFP) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s only supports HFP floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        case ANVIL_ARCH_S390:
            /* S/390 supports HFP (default) and IEEE on some models */
            if (fp_format != ANVIL_FP_HFP && fp_format != ANVIL_FP_IEEE754) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s supports HFP or IEEE754 floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        case ANVIL_ARCH_ZARCH:
            /* z/Architecture supports both HFP and IEEE */
            if (fp_format != ANVIL_FP_HFP && fp_format != ANVIL_FP_IEEE754 && 
                fp_format != ANVIL_FP_HFP_IEEE) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s supports HFP, IEEE754, or HFP_IEEE floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        default:
            /* Other architectures only support IEEE 754 */
            if (fp_format != ANVIL_FP_IEEE754) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s only supports IEEE754 floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
    }
    
    ctx->fp_format = fp_format;
    return ANVIL_OK;
}

anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_FP_IEEE754;
    return ctx->fp_format;
}

const anvil_arch_info_t *anvil_ctx_get_arch_info(anvil_ctx_t *ctx)
{
    if (!ctx) return NULL;
    return &arch_info_table[ctx->arch];
}

const anvil_arch_info_t *anvil_arch_get_info(anvil_arch_t arch)
{
    if (arch >= ANVIL_ARCH_COUNT) return NULL;
    return &arch_info_table[arch];
}

const char *anvil_ctx_get_error(anvil_ctx_t *ctx)
{
    if (!ctx) return "Invalid context";
    return ctx->error_msg;
}

void anvil_set_error(anvil_ctx_t *ctx, anvil_error_t err, const char *fmt, ...)
{
    if (!ctx) return;
    
    ctx->last_error = err;
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, args);
    va_end(args);
}

void anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block)
{
    if (!ctx) return;
    ctx->insert_block = block;
    ctx->insert_point = block ? block->last : NULL;
}

/* ============================================================================
 * CPU Model API Implementation
 * ============================================================================ */

anvil_error_t anvil_ctx_set_cpu(anvil_ctx_t *ctx, anvil_cpu_model_t cpu)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;

    /* Validate CPU model exists (via the cpu_table catalogue). */
    const void *info = anvil_cpu_table_find(cpu);
    if (!info && cpu != ANVIL_CPU_GENERIC) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
            "Unknown CPU model: %d", cpu);
        return ANVIL_ERR_INVALID_ARG;
    }

    /* Validate CPU model is compatible with current architecture. */
    anvil_arch_t model_arch = anvil_cpu_table_info_arch(info);
    if (info && model_arch != ANVIL_ARCH_COUNT && model_arch != ctx->arch) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
            "CPU model '%s' is not compatible with current architecture",
            anvil_cpu_table_info_name(info));
        return ANVIL_ERR_INVALID_ARG;
    }

    ctx->cpu_model = cpu;
    anvil_update_cpu_features(ctx);

    return ANVIL_OK;
}

anvil_cpu_model_t anvil_ctx_get_cpu(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_CPU_GENERIC;
    return ctx->cpu_model;
}

anvil_cpu_features_t anvil_ctx_get_cpu_features(anvil_ctx_t *ctx)
{
    if (!ctx) return 0;
    return ctx->cpu_features;
}

bool anvil_ctx_has_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return false;
    return (ctx->cpu_features & feature) == feature;
}

anvil_error_t anvil_ctx_enable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    ctx->features_enabled |= feature;
    ctx->features_disabled &= ~feature;  /* Remove from disabled if present */
    anvil_update_cpu_features(ctx);
    
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    ctx->features_disabled |= feature;
    ctx->features_enabled &= ~feature;  /* Remove from enabled if present */
    anvil_update_cpu_features(ctx);
    
    return ANVIL_OK;
}
