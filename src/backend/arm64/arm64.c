/*
 * ANVIL - ARM64 (AArch64) Backend
 * 
 * Main backend file - lifecycle, code generation entry points.
 * 
 * Little-endian, stack grows downward
 * Generates GAS syntax (GNU Assembler)
 * Uses AAPCS64 (ARM 64-bit Procedure Call Standard)
 * Supports both Linux (ELF) and Darwin/macOS (Mach-O) ABIs
 */

#include "arm64_internal.h"
#include "anvil/anvil_arm64_mir.h"
#include "../common/gnu_data.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Architecture Info
 * ============================================================================ */

static const anvil_arch_info_t arm64_arch_info = {
    .arch = ANVIL_ARCH_ARM64,
    .name = "ARM64",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 31,
    .num_fpr = 32,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false,
};

/* ============================================================================
 * Backend Lifecycle
 * ============================================================================ */

static anvil_error_t arm64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    arm64_backend_t *priv = calloc(1, sizeof(arm64_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->ctx = ctx;

    be->priv = priv;
    return ANVIL_OK;
}

static void arm64_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    arm64_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv);
    be->priv = NULL;
}

static void arm64_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    arm64_backend_t *priv = be->priv;
    
    /* Clear string table */
    priv->num_strings = 0;
    priv->string_counter = 0;
    
    /* Reset other state */
}

static const anvil_arch_info_t *arm64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &arm64_arch_info;
}

/* ============================================================================
 * Function Emission
 * ============================================================================ */

static anvil_error_t arm64_emit_func(arm64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func) return ANVIL_ERR_INVALID_ARG;
    if (func->is_declaration) return ANVIL_OK;

    if (!arm64_backend_abi(be)) {
        if (be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                            "ARM64 MachineIR codegen supports SysV and Darwin ABIs");
        }
        return ANVIL_ERR_CODEGEN;
    }

    anvil_mir_func_t *mir = anvil_arm64_lower_func_to_mir(func);
    if (!mir) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "ARM64 MachineIR lowering failed for function %s",
                        func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    bool ok = anvil_arm64_regalloc_mir(mir);
    char *mir_text = NULL;
    size_t mir_len = 0;
    if (ok) {
        ok = anvil_arm64_emit_mir_abi(mir, be->ctx->abi, &mir_text, &mir_len);
    }
    anvil_mir_func_destroy(mir);

    if (!ok || !mir_text) {
        free(mir_text);
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "ARM64 MachineIR emission failed for function %s",
                        func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    (void)mir_len;
    anvil_strbuf_append(&be->code, mir_text);
    anvil_strbuf_append(&be->code, "\n");
    free(mir_text);
    return ANVIL_OK;
}

/* ============================================================================
 * Global Variables
 * ============================================================================ */

static size_t arm64_global_align(const anvil_type_t *type)
{
    int align = arm64_type_align((anvil_type_t *)type);
    return align > 0 ? (size_t)align : 1;
}

static bool arm64_emit_globals(arm64_backend_t *be, anvil_module_t *mod)
{
    const arm64_abi_desc_t *abi = arm64_backend_abi(be);
    if (!abi)
        return false;
    anvil_gnu_globals_config_t config = {
        .format = abi->format,
        .symbol_prefix = abi->symbol_prefix,
        .pointer_size = 8,
        .type_align = arm64_global_align,
    };
    return anvil_gnu_emit_globals(&be->data, mod, &config);
}

/* ============================================================================
 * String Constants
 * ============================================================================ */

static void arm64_emit_strings(arm64_backend_t *be)
{
    const arm64_abi_desc_t *abi = arm64_backend_abi(be);
    if (be->num_strings == 0 || !abi) return;

    anvil_strbuf_appendf(&be->data, "%s\n", abi->cstring_section);
    
    for (size_t i = 0; i < be->num_strings; i++) {
        arm64_string_entry_t *entry = &be->strings[i];
        anvil_strbuf_appendf(&be->data, "%s:\n", entry->label);
        anvil_strbuf_append(&be->data, "\t.asciz \"");
        
        for (const char *p = entry->str; *p; p++) {
            switch (*p) {
                case '\n': anvil_strbuf_append(&be->data, "\\n"); break;
                case '\r': anvil_strbuf_append(&be->data, "\\r"); break;
                case '\t': anvil_strbuf_append(&be->data, "\\t"); break;
                case '\\': anvil_strbuf_append(&be->data, "\\\\"); break;
                case '"':  anvil_strbuf_append(&be->data, "\\\""); break;
                default:   anvil_strbuf_append_char(&be->data, *p); break;
            }
        }
        anvil_strbuf_append(&be->data, "\"\n");
    }
    
    anvil_strbuf_append(&be->data, "\n");
}

/* ============================================================================
 * Code Generation Entry Points
 * ============================================================================ */

static anvil_error_t arm64_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                          char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;

    arm64_backend_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->num_strings = 0;
    
    const arm64_abi_desc_t *abi = arm64_backend_abi(priv);
    if (!abi) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "ARM64 supports the Linux (SysV) and Darwin ABIs");
        return ANVIL_ERR_CODEGEN;
    }
    anvil_strbuf_append(&priv->code, abi->module_header);
    
    /* Emit functions (prepare_ir already called by anvil_module_codegen) */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_error_t err = arm64_emit_func(priv, func);
        if (err != ANVIL_OK) return err;
    }
    
    /* Emit globals and strings */
    if (!arm64_emit_globals(priv, mod)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN,
                        "ARM64 global initializer is not representable");
        return ANVIL_ERR_CODEGEN;
    }
    arm64_emit_strings(priv);
    if (priv->code.failed || priv->data.failed) return ANVIL_ERR_NOMEM;
    
    /* Combine output */
    return anvil_obj_finish_module(&priv->code, &priv->data, abi->format, output, len);
}

static anvil_error_t arm64_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                        char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len) *len = 0;
    
    arm64_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    anvil_error_t err = arm64_emit_func(priv, func);
    if (err != ANVIL_OK) return err;
    if (priv->code.failed) return ANVIL_ERR_NOMEM;
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}

/* ============================================================================
 * Backend Registration
 * ============================================================================ */

/* va_arg / va_copy follow the context's ABI. */
static const arm64_abi_desc_t *arm64_va_abi(anvil_backend_t *be)
{
    const arm64_abi_desc_t *abi = be->ctx ? arm64_get_abi_desc(be->ctx->abi) : NULL;
    if (!abi && be->ctx)
        anvil_set_error(be->ctx, ANVIL_ERR_INVALID_TYPE, "ARM64 variadic arguments need the Linux or Darwin ABI");
    return abi;
}

static anvil_value_t *arm64_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    const arm64_abi_desc_t *abi = arm64_va_abi(be);
    return abi ? abi->build_va_arg(be, storage, type, name) : NULL;
}

static anvil_value_t *arm64_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    const arm64_abi_desc_t *abi = arm64_va_abi(be);
    return abi ? abi->build_va_copy(be, cursor, name) : NULL;
}

static bool arm64_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    const arm64_abi_desc_t *abi = arm64_va_abi(be);
    return abi && abi->build_va_copy_into(be, destination, cursor);
}

static bool arm64_atomic_is_lock_free(anvil_backend_t *be, anvil_type_t *type)
{
    (void)be;
    return anvil_atomic_type_valid(type);
}

static bool arm64_reloc_model_supported(anvil_backend_t *be, anvil_reloc_model_t model)
{
    const arm64_abi_desc_t *abi = arm64_backend_abi(be->priv);
    return abi && (abi->reloc_models & (1u << model));
}

static anvil_reloc_model_t arm64_default_reloc_model(anvil_backend_t *be)
{
    const arm64_abi_desc_t *abi = arm64_backend_abi(be->priv);
    return abi ? abi->default_reloc : ANVIL_RELOC_STATIC;
}

const anvil_backend_ops_t anvil_backend_arm64 = {
    .name = "ARM64",
    .arch = ANVIL_ARCH_ARM64,
    .init = arm64_init,
    .cleanup = arm64_cleanup,
    .reset = arm64_reset,
    .codegen_module = arm64_codegen_module,
    .codegen_func = arm64_codegen_func,
    .get_arch_info = arm64_get_arch_info,
    .atomic_is_lock_free = arm64_atomic_is_lock_free,
    .build_va_arg = arm64_build_va_arg,
    .build_va_copy = arm64_build_va_copy,
    .build_va_copy_into = arm64_build_va_copy_into,
    .param_attr_support = (1u << ANVIL_PARAM_BYVAL) | (1u << ANVIL_PARAM_SRET),
    .abi_support = (1u << ANVIL_ABI_SYSV) | (1u << ANVIL_ABI_DARWIN),
    .reloc_model_supported = arm64_reloc_model_supported,
    .vector_operation_cost = arm64_vector_operation_cost,
    .default_reloc_model = arm64_default_reloc_model
};
