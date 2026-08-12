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
#include "../gnu_data.h"
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
    .has_delay_slots = false
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

    if (!be->ctx ||
        (be->ctx->abi != ANVIL_ABI_SYSV && be->ctx->abi != ANVIL_ABI_DARWIN)) {
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

static bool arm64_emit_globals(arm64_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return true;
    
    const char *prefix = arm64_symbol_prefix(be);
    
    /* Count actual globals (skip function declarations) */
    int actual_globals = 0;
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;
        actual_globals++;
    }
    if (actual_globals == 0) return true;

    anvil_gnu_string_pool_t strings;
    anvil_gnu_string_pool_init(&strings, ".Lanvil_global_string_");
    
    anvil_strbuf_append(&be->data, "\t.data\n");
    
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;
        
        anvil_value_t *global = g->value;
        if (!global || !global->name || !global->type) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
        if (global->data.global.is_declaration) {
            anvil_strbuf_appendf(&be->data, "\t.extern %s%s\n", prefix,
                                 global->name);
            continue;
        }
        if (global->data.global.linkage == ANVIL_LINK_EXTERNAL ||
            global->data.global.linkage == ANVIL_LINK_COMMON) {
            anvil_strbuf_appendf(&be->data, "\t.globl %s%s\n", prefix,
                                 global->name);
        } else if (global->data.global.linkage == ANVIL_LINK_WEAK) {
            anvil_strbuf_appendf(&be->data, "\t.weak %s%s\n", prefix,
                                 global->name);
        }
        
        int align = g->value->type ? arm64_type_align(g->value->type) : 8;
        
        if (arm64_is_darwin(be)) {
            int p2align = (align <= 1) ? 0 : (align <= 2) ? 1 : (align <= 4) ? 2 : 3;
            anvil_strbuf_appendf(&be->data, "\t.p2align %d\n", p2align);
        } else {
            anvil_strbuf_appendf(&be->data, "\t.align %d\n", align);
        }
        
        anvil_strbuf_appendf(&be->data, "%s%s:\n", prefix, g->value->name);
        
        if (!anvil_gnu_emit_constant(&be->data, global->type,
                                     global->data.global.init, 8, prefix,
                                     &strings, NULL, NULL)) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
    }
    
    anvil_strbuf_append(&be->data, "\n");
    const char *string_section = arm64_is_darwin(be)
        ? "\t.section __TEXT,__cstring,cstring_literals"
        : "\t.section .rodata";
    bool ok = anvil_gnu_string_pool_emit(&be->data, &strings,
                                          string_section);
    anvil_gnu_string_pool_destroy(&strings);
    return ok && !be->data.failed;
}

/* ============================================================================
 * String Constants
 * ============================================================================ */

static void arm64_emit_strings(arm64_backend_t *be)
{
    if (be->num_strings == 0) return;
    
    if (arm64_is_darwin(be)) {
        anvil_strbuf_append(&be->data, "\t.section __TEXT,__cstring,cstring_literals\n");
    } else {
        anvil_strbuf_append(&be->data, "\t.section .rodata\n");
    }
    
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
    
    /* Emit header */
    if (arm64_is_darwin(priv)) {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for ARM64 (AArch64) - macOS\n");
        anvil_strbuf_append(&priv->code, "\t.build_version macos, 11, 0\n");
        anvil_strbuf_append(&priv->code, "\t.section __TEXT,__text,regular,pure_instructions\n\n");
    } else {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for ARM64 (AArch64) - Linux\n");
        anvil_strbuf_append(&priv->code, "\t.arch armv8-a\n");
        anvil_strbuf_append(&priv->code, "\t.text\n\n");
    }
    
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
    anvil_strbuf_t result;
    anvil_strbuf_init(&result);
    if (result.failed) return ANVIL_ERR_NOMEM;
    char *code_str = anvil_strbuf_detach(&priv->code, NULL);
    char *data_str = anvil_strbuf_detach(&priv->data, NULL);
    if (!code_str || !data_str) {
        free(code_str);
        free(data_str);
        anvil_strbuf_destroy(&result);
        return ANVIL_ERR_NOMEM;
    }
    anvil_strbuf_append(&result, code_str);
    anvil_strbuf_append(&result, data_str);
    free(code_str);
    free(data_str);
    if (result.failed) {
        anvil_strbuf_destroy(&result);
        return ANVIL_ERR_NOMEM;
    }
    
    *output = anvil_strbuf_detach(&result, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
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

const anvil_backend_ops_t anvil_backend_arm64 = {
    .name = "ARM64",
    .arch = ANVIL_ARCH_ARM64,
    .init = arm64_init,
    .cleanup = arm64_cleanup,
    .reset = arm64_reset,
    .codegen_module = arm64_codegen_module,
    .codegen_func = arm64_codegen_func,
    .get_arch_info = arm64_get_arch_info
};
