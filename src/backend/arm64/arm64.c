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
#include "opt/arm64_opt.h"
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

static void arm64_emit_globals(arm64_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return;
    
    const char *prefix = arm64_symbol_prefix(be);
    
    /* Count actual globals (skip function declarations) */
    int actual_globals = 0;
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;
        actual_globals++;
    }
    if (actual_globals == 0) return;
    
    anvil_strbuf_append(&be->data, "\t.data\n");
    
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;
        
        anvil_strbuf_appendf(&be->data, "\t.globl %s%s\n", prefix, g->value->name);
        
        int size = g->value->type ? arm64_type_size(g->value->type) : 8;
        int align = g->value->type ? arm64_type_align(g->value->type) : 8;
        
        if (arm64_is_darwin(be)) {
            int p2align = (align <= 1) ? 0 : (align <= 2) ? 1 : (align <= 4) ? 2 : 3;
            anvil_strbuf_appendf(&be->data, "\t.p2align %d\n", p2align);
        } else {
            anvil_strbuf_appendf(&be->data, "\t.align %d\n", align);
        }
        
        anvil_strbuf_appendf(&be->data, "%s%s:\n", prefix, g->value->name);
        
        if (g->value->data.global.init) {
            anvil_value_t *init = g->value->data.global.init;
            if (init->kind == ANVIL_VAL_CONST_INT) {
                switch (size) {
                    case 1: anvil_strbuf_appendf(&be->data, "\t.byte %lld\n", (long long)init->data.i); break;
                    case 2: anvil_strbuf_appendf(&be->data, "\t.short %lld\n", (long long)init->data.i); break;
                    case 4: anvil_strbuf_appendf(&be->data, "\t.long %lld\n", (long long)init->data.i); break;
                    default: anvil_strbuf_appendf(&be->data, "\t.quad %lld\n", (long long)init->data.i); break;
                }
            } else if (init->kind == ANVIL_VAL_CONST_FLOAT) {
                /* Emit floating-point initializer using bit representation */
                if (size == 4) {
                    /* float - use single precision bit pattern */
                    float fval = (float)init->data.f;
                    uint32_t bits;
                    memcpy(&bits, &fval, sizeof(bits));
                    anvil_strbuf_appendf(&be->data, "\t.long 0x%x\n", bits);
                } else {
                    /* double - use double precision bit pattern */
                    double dval = init->data.f;
                    uint64_t bits;
                    memcpy(&bits, &dval, sizeof(bits));
                    anvil_strbuf_appendf(&be->data, "\t.quad 0x%llx\n", (unsigned long long)bits);
                }
            } else if (init->kind == ANVIL_VAL_CONST_ARRAY) {
                /* Emit array initializer */
                int elem_size = 8;
                bool is_float_array = false;
                if (g->value->type && g->value->type->kind == ANVIL_TYPE_ARRAY &&
                    g->value->type->data.array.elem) {
                    elem_size = arm64_type_size(g->value->type->data.array.elem);
                    is_float_array = arm64_type_is_float(g->value->type->data.array.elem);
                }
                for (size_t i = 0; i < init->data.array.num_elements; i++) {
                    anvil_value_t *elem = init->data.array.elements[i];
                    if (is_float_array && elem && elem->kind == ANVIL_VAL_CONST_FLOAT) {
                        if (elem_size == 4) {
                            float fval = (float)elem->data.f;
                            uint32_t bits;
                            memcpy(&bits, &fval, sizeof(bits));
                            anvil_strbuf_appendf(&be->data, "\t.long 0x%x\n", bits);
                        } else {
                            double dval = elem->data.f;
                            uint64_t bits;
                            memcpy(&bits, &dval, sizeof(bits));
                            anvil_strbuf_appendf(&be->data, "\t.quad 0x%llx\n", (unsigned long long)bits);
                        }
                    } else {
                        int64_t val = 0;
                        if (elem && elem->kind == ANVIL_VAL_CONST_INT) {
                            val = elem->data.i;
                        }
                        switch (elem_size) {
                            case 1: anvil_strbuf_appendf(&be->data, "\t.byte %lld\n", (long long)val); break;
                            case 2: anvil_strbuf_appendf(&be->data, "\t.short %lld\n", (long long)val); break;
                            case 4: anvil_strbuf_appendf(&be->data, "\t.long %lld\n", (long long)val); break;
                            default: anvil_strbuf_appendf(&be->data, "\t.quad %lld\n", (long long)val); break;
                        }
                    }
                }
            } else {
                anvil_strbuf_appendf(&be->data, "\t.zero %d\n", size);
            }
        } else {
            anvil_strbuf_appendf(&be->data, "\t.zero %d\n", size);
        }
    }
    
    anvil_strbuf_append(&be->data, "\n");
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
    arm64_emit_globals(priv, mod);
    arm64_emit_strings(priv);
    
    /* Combine output */
    anvil_strbuf_t result;
    anvil_strbuf_init(&result);
    char *code_str = anvil_strbuf_detach(&priv->code, NULL);
    char *data_str = anvil_strbuf_detach(&priv->data, NULL);
    if (code_str) {
        anvil_strbuf_append(&result, code_str);
        free(code_str);
    }
    if (data_str) {
        anvil_strbuf_append(&result, data_str);
        free(data_str);
    }
    
    *output = anvil_strbuf_detach(&result, len);
    return ANVIL_OK;
}

static anvil_error_t arm64_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                        char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    arm64_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    anvil_error_t err = arm64_emit_func(priv, func);
    if (err != ANVIL_OK) return err;
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

/* ============================================================================
 * IR Preparation/Lowering
 * ============================================================================ */

static anvil_error_t arm64_prepare_ir(anvil_backend_t *be, anvil_module_t *mod)
{
    if (!be || !mod) return ANVIL_ERR_INVALID_ARG;
    
    arm64_backend_t *priv = be->priv;
    
    /* Run ARM64-specific optimizations */
    arm64_opt_module(priv, mod);
    
    return ANVIL_OK;
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
    .prepare_ir = arm64_prepare_ir,
    .codegen_module = arm64_codegen_module,
    .codegen_func = arm64_codegen_func,
    .get_arch_info = arm64_get_arch_info
};
