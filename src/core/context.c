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

static anvil_layout_entry_t layout_entry(size_t size, size_t abi,
                                          size_t preferred)
{
    anvil_layout_entry_t entry = { size, abi, preferred };
    return entry;
}

static anvil_data_layout_t data_layout_for(anvil_arch_t arch, anvil_abi_t abi)
{
    const size_t ptr = (size_t)arch_info_table[arch].ptr_size;
    anvil_data_layout_t dl;
    memset(&dl, 0, sizeof(dl));
    dl.pointer = layout_entry(ptr, ptr, ptr);
    dl.i1 = layout_entry(1, 1, 1);
    dl.i8 = layout_entry(1, 1, 1);
    dl.i16 = layout_entry(2, 2, 2);
    dl.i32 = layout_entry(4, 4, 4);
    dl.f32 = layout_entry(4, 4, 4);
    dl.i64 = layout_entry(8, 8, 8);
    dl.f64 = layout_entry(8, 8, 8);
    dl.aggregate_abi_align = 1;
    dl.aggregate_preferred_align = ptr;

    /* i386 SysV permits 64-bit scalars at four-byte object alignment while
     * still preferring eight-byte alignment. PPC32 keeps the natural
     * eight-byte ABI alignment. */
    if (arch == ANVIL_ARCH_X86) {
        dl.i64.abi_align = 4;
        dl.f64.abi_align = 4;
    }
    /* The currently supported SysV, Darwin, Win64 and MVS variants share
     * scalar/object alignment for their corresponding architecture. Keep the
     * ABI dispatch explicit so future ABI-specific vector/long-double rules
     * cannot accidentally become an architecture-only global. */
    switch (abi) {
        case ANVIL_ABI_DEFAULT:
        case ANVIL_ABI_SYSV:
        case ANVIL_ABI_DARWIN:
        case ANVIL_ABI_WIN64:
        case ANVIL_ABI_MVS:
            break;
    }
    return dl;
}

anvil_ctx_t *anvil_ctx_create(void)
{
    anvil_ctx_t *ctx = calloc(1, sizeof(anvil_ctx_t));
    if (!ctx) return NULL;

    ctx->arch = ANVIL_ARCH_NONE;
    ctx->output = ANVIL_OUTPUT_ASM;
    ctx->syntax = ANVIL_SYNTAX_DEFAULT;
    ctx->abi = ANVIL_ABI_DEFAULT;
    ctx->fp_format = ANVIL_FP_UNSPECIFIED;
    ctx->cpu_model = ANVIL_CPU_GENERIC;

    anvil_init_backends();
    return ctx;
}

anvil_ctx_t *anvil_ctx_create_for_target(anvil_arch_t arch)
{
    if ((unsigned)arch >= (unsigned)ANVIL_ARCH_COUNT) return NULL;
    anvil_ctx_t *ctx = anvil_ctx_create();
    if (!ctx) return NULL;
    if (anvil_ctx_set_target(ctx, arch) != ANVIL_OK) {
        anvil_ctx_destroy(ctx);
        return NULL;
    }
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

    /* Allocation registries outlive CFG topology and module unlinking. */
    anvil_func_free_all(ctx);
    anvil_ir_free_all(ctx);
    anvil_value_free_all(ctx);

    /* Free every type registered against this context. */
    anvil_type_t *type = ctx->types;
    while (type) {
        anvil_type_t *next = type->ctx_next;
        anvil_type_free(type);
        type = next;
    }

    free(ctx->named_struct_buckets);

    free(ctx);
}

anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)arch >= (unsigned)ANVIL_ARCH_COUNT) {
        return ANVIL_ERR_INVALID_ARG;
    }

    if (ctx->target_configured && ctx->arch == arch && ctx->backend)
        return ANVIL_OK;
    if (ctx->target_frozen || ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "Target is frozen; create a new context for another target");
        return ANVIL_ERR_INVALID_OP;
    }

    /* Construct the replacement backend against a complete candidate target
     * state, but retain the old backend and restore every field on failure. */
    anvil_arch_t old_arch = ctx->arch;
    anvil_fp_format_t old_fp = ctx->fp_format;
    anvil_abi_t old_abi = ctx->abi;
    anvil_cpu_model_t old_cpu = ctx->cpu_model;
    anvil_cpu_features_t old_features = ctx->cpu_features;
    anvil_cpu_features_t old_enabled = ctx->features_enabled;
    anvil_cpu_features_t old_disabled = ctx->features_disabled;
    anvil_data_layout_t old_layout = ctx->data_layout;
    anvil_backend_t *old_backend = ctx->backend;
    anvil_type_t *old_types = ctx->types;
    anvil_type_t *old_type_void = ctx->type_void;
    anvil_type_t *old_type_i1 = ctx->type_i1;
    anvil_type_t *old_type_i8 = ctx->type_i8;
    anvil_type_t *old_type_i16 = ctx->type_i16;
    anvil_type_t *old_type_i32 = ctx->type_i32;
    anvil_type_t *old_type_i64 = ctx->type_i64;
    anvil_type_t *old_type_u8 = ctx->type_u8;
    anvil_type_t *old_type_u16 = ctx->type_u16;
    anvil_type_t *old_type_u32 = ctx->type_u32;
    anvil_type_t *old_type_u64 = ctx->type_u64;
    anvil_type_t *old_type_f32 = ctx->type_f32;
    anvil_type_t *old_type_f64 = ctx->type_f64;
    anvil_type_t *old_type_ptr_i8 = ctx->type_ptr_i8;
    anvil_type_t *old_type_ptr_void = ctx->type_ptr_void;

    ctx->arch = arch;
    ctx->fp_format = arch_info_table[arch].fp_format;
    ctx->abi = arch_info_table[arch].abi;
    ctx->data_layout = data_layout_for(arch, ctx->abi);
    ctx->cpu_model = anvil_arch_default_cpu(arch);
    ctx->features_enabled = 0;
    ctx->features_disabled = 0;
    anvil_update_cpu_features(ctx);

    anvil_ctx_clear_error(ctx);
    anvil_backend_t *new_backend = anvil_get_backend(ctx, arch);
    if (!new_backend) {
        ctx->arch = old_arch;
        ctx->fp_format = old_fp;
        ctx->abi = old_abi;
        ctx->cpu_model = old_cpu;
        ctx->cpu_features = old_features;
        ctx->features_enabled = old_enabled;
        ctx->features_disabled = old_disabled;
        ctx->data_layout = old_layout;
        ctx->backend = old_backend;
        anvil_error_t error = ctx->last_error == ANVIL_OK
                                  ? ANVIL_ERR_NO_BACKEND : ctx->last_error;
        if (error == ANVIL_ERR_NO_BACKEND)
            anvil_set_error(ctx, error,
                            "No backend available for architecture %s",
                            arch_info_table[arch].name);
        return error;
    }

    ctx->backend = new_backend;
    anvil_type_init_sizes(ctx);
    if (!ctx->type_void || !ctx->type_i1 || !ctx->type_i8 || !ctx->type_i16 ||
        !ctx->type_i32 || !ctx->type_i64 || !ctx->type_u8 ||
        !ctx->type_u16 || !ctx->type_u32 || !ctx->type_u64 ||
        !ctx->type_f32 || !ctx->type_f64 || !ctx->type_ptr_i8 ||
        !ctx->type_ptr_void) {
        if (new_backend->ops && new_backend->ops->cleanup)
            new_backend->ops->cleanup(new_backend);
        free(new_backend);
        while (ctx->types != old_types) {
            anvil_type_t *failed_type = ctx->types;
            ctx->types = failed_type->ctx_next;
            anvil_type_free(failed_type);
        }
        ctx->type_void = old_type_void;
        ctx->type_i1 = old_type_i1;
        ctx->type_i8 = old_type_i8;
        ctx->type_i16 = old_type_i16;
        ctx->type_i32 = old_type_i32;
        ctx->type_i64 = old_type_i64;
        ctx->type_u8 = old_type_u8;
        ctx->type_u16 = old_type_u16;
        ctx->type_u32 = old_type_u32;
        ctx->type_u64 = old_type_u64;
        ctx->type_f32 = old_type_f32;
        ctx->type_f64 = old_type_f64;
        ctx->type_ptr_i8 = old_type_ptr_i8;
        ctx->type_ptr_void = old_type_ptr_void;
        ctx->arch = old_arch;
        ctx->fp_format = old_fp;
        ctx->abi = old_abi;
        ctx->cpu_model = old_cpu;
        ctx->cpu_features = old_features;
        ctx->features_enabled = old_enabled;
        ctx->features_disabled = old_disabled;
        ctx->data_layout = old_layout;
        ctx->backend = old_backend;
        anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                        "Out of memory initializing target types");
        return ANVIL_ERR_NOMEM;
    }

    if (old_backend) {
        anvil_cpu_model_t new_cpu = ctx->cpu_model;
        anvil_cpu_features_t new_features = ctx->cpu_features;
        anvil_data_layout_t new_layout = ctx->data_layout;
        anvil_fp_format_t new_fp = ctx->fp_format;
        anvil_abi_t new_abi = ctx->abi;
        ctx->arch = old_arch;
        ctx->fp_format = old_fp;
        ctx->abi = old_abi;
        ctx->cpu_model = old_cpu;
        ctx->cpu_features = old_features;
        ctx->features_enabled = old_enabled;
        ctx->features_disabled = old_disabled;
        ctx->data_layout = old_layout;
        ctx->backend = old_backend;
        if (old_backend->ops && old_backend->ops->reset)
            old_backend->ops->reset(old_backend);
        if (old_backend->ops && old_backend->ops->cleanup)
            old_backend->ops->cleanup(old_backend);
        free(old_backend);
        ctx->arch = arch;
        ctx->fp_format = new_fp;
        ctx->abi = new_abi;
        ctx->cpu_model = new_cpu;
        ctx->cpu_features = new_features;
        ctx->features_enabled = 0;
        ctx->features_disabled = 0;
        ctx->data_layout = new_layout;
        ctx->backend = new_backend;
    }
    ctx->target_configured = true;
    return ANVIL_OK;
}

void anvil_ctx_freeze_target(anvil_ctx_t *ctx)
{
    if (ctx && ctx->target_configured) ctx->target_frozen = true;
}

const anvil_data_layout_t *anvil_ctx_get_data_layout(const anvil_ctx_t *ctx)
{
    return ctx && ctx->target_configured ? &ctx->data_layout : NULL;
}

anvil_error_t anvil_ctx_set_output(anvil_ctx_t *ctx, anvil_output_t output)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if (output != ANVIL_OUTPUT_ASM) {
        return ANVIL_ERR_INVALID_ARG;
    }
    ctx->output = output;
    return ANVIL_OK;
}

anvil_arch_t anvil_ctx_get_target(anvil_ctx_t *ctx)
{
    return ctx ? ctx->arch : ANVIL_ARCH_NONE;
}

bool anvil_ctx_has_target(const anvil_ctx_t *ctx)
{
    return ctx && ctx->target_configured && ctx->arch != ANVIL_ARCH_NONE &&
           ctx->backend != NULL;
}

anvil_error_t anvil_ctx_set_syntax(anvil_ctx_t *ctx, anvil_syntax_t syntax)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)syntax > (unsigned)ANVIL_SYNTAX_GAS) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Unknown assembly syntax");
        return ANVIL_ERR_INVALID_ARG;
    }
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before setting assembly syntax");
        return ANVIL_ERR_NO_TARGET;
    }
    bool mainframe = ctx->arch == ANVIL_ARCH_S370 ||
                     ctx->arch == ANVIL_ARCH_S370_XA ||
                     ctx->arch == ANVIL_ARCH_S390 ||
                     ctx->arch == ANVIL_ARCH_ZARCH;
    if ((syntax == ANVIL_SYNTAX_HLASM && !mainframe) ||
        (syntax == ANVIL_SYNTAX_GAS && mainframe)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Assembly syntax is not supported by the selected target");
        return ANVIL_ERR_INVALID_ARG;
    }
    ctx->syntax = syntax;
    if (ctx->backend) {
        ctx->backend->syntax = syntax;
    }
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_abi(anvil_ctx_t *ctx, anvil_abi_t abi)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)abi > (unsigned)ANVIL_ABI_MVS) {
        return ANVIL_ERR_INVALID_ARG;
    }
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before setting the ABI");
        return ANVIL_ERR_NO_TARGET;
    }
    if (ctx->target_frozen && ctx->abi != abi) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "Target ABI is frozen by existing IR/types");
        return ANVIL_ERR_INVALID_OP;
    }
    ctx->abi = abi;
    ctx->data_layout = data_layout_for(ctx->arch, abi);
    anvil_type_init_sizes(ctx);
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
    if (fp_format != ANVIL_FP_IEEE754 && fp_format != ANVIL_FP_HFP &&
        fp_format != ANVIL_FP_HFP_IEEE) {
        return ANVIL_ERR_INVALID_ARG;
    }
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before setting the floating-point format");
        return ANVIL_ERR_NO_TARGET;
    }
    
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
    if (!ctx || !ctx->target_configured) return ANVIL_FP_UNSPECIFIED;
    return ctx->fp_format;
}

const anvil_arch_info_t *anvil_ctx_get_arch_info(anvil_ctx_t *ctx)
{
    if (!ctx || !ctx->target_configured) return NULL;
    return &arch_info_table[ctx->arch];
}

const anvil_arch_info_t *anvil_arch_get_info(anvil_arch_t arch)
{
    if ((unsigned)arch >= (unsigned)ANVIL_ARCH_COUNT) return NULL;
    return &arch_info_table[arch];
}

const char *anvil_ctx_get_error(anvil_ctx_t *ctx)
{
    if (!ctx) return "Invalid context";
    return ctx->error_msg;
}

anvil_error_t anvil_ctx_get_last_error(anvil_ctx_t *ctx)
{
    return ctx ? ctx->last_error : ANVIL_ERR_INVALID_ARG;
}

void anvil_ctx_clear_error(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->last_error = ANVIL_OK;
    ctx->error_msg[0] = '\0';
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

bool anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block)
{
    if (!ctx) return false;
    if (!block) {
        ctx->insert_block = NULL;
        ctx->insert_point = NULL;
        return true;
    }
    if (!block->owner_module || block->owner_module->ctx != ctx ||
        !block->parent || block->parent->parent != block->owner_module) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                        "Insertion block is destroyed or belongs to another context");
        return false;
    }
    bool live = false;
    for (anvil_block_t *candidate = block->parent->blocks; candidate;
         candidate = candidate->next) {
        if (candidate == block) { live = true; break; }
    }
    if (!live) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "Insertion block is no longer part of its function");
        return false;
    }
    ctx->insert_block = block;
    ctx->insert_point = block->last;
    return true;
}

anvil_block_t *anvil_get_insert_block(anvil_ctx_t *ctx)
{
    return ctx ? ctx->insert_block : NULL;
}

static bool alloc_should_fail(anvil_ctx_t *ctx)
{
    if (!ctx || !ctx->alloc_fail_enabled) return false;
    if (ctx->alloc_fail_after == 0) return true;
    ctx->alloc_fail_after--;
    return false;
}

void *anvil_ctx_malloc(anvil_ctx_t *ctx, size_t size)
{
    if (alloc_should_fail(ctx)) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Injected allocation failure");
        return NULL;
    }
    void *ptr = malloc(size);
    if (!ptr && size != 0) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Out of memory");
    }
    return ptr;
}

void *anvil_ctx_calloc(anvil_ctx_t *ctx, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Allocation size overflow");
        return NULL;
    }
    if (alloc_should_fail(ctx)) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Injected allocation failure");
        return NULL;
    }
    void *ptr = calloc(count, size);
    if (!ptr && count != 0 && size != 0) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Out of memory");
    }
    return ptr;
}

void *anvil_ctx_realloc(anvil_ctx_t *ctx, void *ptr, size_t size)
{
    if (alloc_should_fail(ctx)) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Injected allocation failure");
        return NULL;
    }
    void *resized = realloc(ptr, size);
    if (!resized && size != 0) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Out of memory");
    }
    return resized;
}

char *anvil_ctx_strdup(anvil_ctx_t *ctx, const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    if (len == SIZE_MAX) {
        anvil_set_error(ctx, ANVIL_ERR_NOMEM, "String size overflow");
        return NULL;
    }
    char *copy = anvil_ctx_malloc(ctx, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

void anvil_test_fail_alloc_after(anvil_ctx_t *ctx, size_t successes)
{
    if (!ctx) return;
    ctx->alloc_fail_enabled = true;
    ctx->alloc_fail_after = successes;
}

void anvil_test_disable_alloc_fail(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->alloc_fail_enabled = false;
    ctx->alloc_fail_after = 0;
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
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before setting the CPU model");
        return ANVIL_ERR_NO_TARGET;
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
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before enabling CPU features");
        return ANVIL_ERR_NO_TARGET;
    }
    
    ctx->features_enabled |= feature;
    ctx->features_disabled &= ~feature;  /* Remove from disabled if present */
    anvil_update_cpu_features(ctx);
    
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if (!ctx->target_configured) {
        anvil_set_error(ctx, ANVIL_ERR_NO_TARGET,
                        "Select a target before disabling CPU features");
        return ANVIL_ERR_NO_TARGET;
    }
    
    ctx->features_disabled |= feature;
    ctx->features_enabled &= ~feature;  /* Remove from enabled if present */
    anvil_update_cpu_features(ctx);
    
    return ANVIL_OK;
}
