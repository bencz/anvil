/*
 * ANVIL - x86 (32-bit) Backend
 *
 * Thin backend_ops driver over the shared MachineIR pipeline: lower IR to
 * MachineIR, allocate registers, materialize spills, and emit x86 assembly.
 * Little-endian, stack grows downward. Generates GAS (AT&T) syntax. Supports
 * cdecl/stdcall/fastcall calling conventions and ELF, Mach-O, and Win32 COFF
 * platform decoration.
 */

#include "x86_internal.h"
#include "anvil/anvil_x86_mir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    anvil_ctx_t *ctx;
    anvil_syntax_t syntax;
} x86_backend_priv_t;

static const anvil_arch_info_t x86_arch_info = {
    .arch = ANVIL_ARCH_X86,
    .name = "x86",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 8,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t x86_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    x86_backend_priv_t *priv = calloc(1, sizeof(x86_backend_priv_t));
    if (!priv) return ANVIL_ERR_NOMEM;

    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->ctx = ctx;
    priv->syntax = ctx->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS
                                                       : ctx->syntax;

    be->priv = priv;
    return ANVIL_OK;
}

static void x86_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;

    x86_backend_priv_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
}

static void x86_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *x86_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &x86_arch_info;
}

static bool x86_is_macho(x86_backend_priv_t *priv)
{
    return priv->ctx && priv->ctx->abi == ANVIL_ABI_DARWIN;
}

static bool x86_is_coff(x86_backend_priv_t *priv)
{
    return priv->ctx && priv->ctx->abi == ANVIL_ABI_WIN64;
}

static const char *x86_symbol_prefix(x86_backend_priv_t *priv)
{
    return (x86_is_macho(priv) || x86_is_coff(priv)) ? "_" : "";
}

static anvil_error_t x86_emit_func(x86_backend_priv_t *priv, anvil_func_t *func)
{
    if (!priv || !func) return ANVIL_ERR_INVALID_ARG;
    if (func->is_declaration) return ANVIL_OK;

    anvil_abi_t abi = priv->ctx ? priv->ctx->abi : ANVIL_ABI_DEFAULT;

    anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(func);
    if (!mir) {
        if (priv->ctx) {
            anvil_set_error(priv->ctx, ANVIL_ERR_CODEGEN,
                            "x86 MachineIR lowering failed for function %s",
                            func->name ? func->name : "<anonymous>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    bool ok = anvil_x86_regalloc_mir(mir);
    char *mir_text = NULL;
    size_t mir_len = 0;
    if (ok) {
        ok = anvil_x86_emit_mir_abi(mir, func, abi, priv->syntax,
                                    &mir_text, &mir_len);
    }
    anvil_mir_func_destroy(mir);

    if (!ok || !mir_text) {
        free(mir_text);
        if (priv->ctx) {
            anvil_set_error(priv->ctx, ANVIL_ERR_CODEGEN,
                            "x86 MachineIR emission failed for function %s",
                            func->name ? func->name : "<anonymous>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    (void)mir_len;
    anvil_strbuf_append(&priv->code, mir_text);
    anvil_strbuf_append(&priv->code, "\n");
    free(mir_text);
    return ANVIL_OK;
}

static void x86_emit_globals(x86_backend_priv_t *priv, anvil_module_t *mod)
{
    if (mod->num_globals == 0) return;

    const char *prefix = x86_symbol_prefix(priv);

    int actual_globals = 0;
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;
        actual_globals++;
    }
    if (actual_globals == 0) return;

    anvil_strbuf_append(&priv->data, "\t.data\n");

    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC) continue;

        anvil_strbuf_appendf(&priv->data, "\t.globl %s%s\n", prefix, g->value->name);

        int size = g->value->type ? x86_type_size(g->value->type) : 4;
        int align = g->value->type ? x86_type_align(g->value->type) : 4;

        if (x86_is_macho(priv)) {
            int p2align = (align <= 1) ? 0 : (align <= 2) ? 1 : (align <= 4) ? 2 : 3;
            anvil_strbuf_appendf(&priv->data, "\t.p2align %d\n", p2align);
        } else {
            anvil_strbuf_appendf(&priv->data, "\t.align %d\n", align);
        }

        anvil_strbuf_appendf(&priv->data, "%s%s:\n", prefix, g->value->name);

        if (g->value->data.global.init) {
            anvil_value_t *init = g->value->data.global.init;
            if (init->kind == ANVIL_VAL_CONST_INT) {
                switch (size) {
                    case 1: anvil_strbuf_appendf(&priv->data, "\t.byte %lld\n", (long long)init->data.i); break;
                    case 2: anvil_strbuf_appendf(&priv->data, "\t.short %lld\n", (long long)init->data.i); break;
                    case 4: anvil_strbuf_appendf(&priv->data, "\t.long %lld\n", (long long)init->data.i); break;
                    default: anvil_strbuf_appendf(&priv->data, "\t.quad %lld\n", (long long)init->data.i); break;
                }
            } else if (init->kind == ANVIL_VAL_CONST_FLOAT) {
                if (size == 4) {
                    float fval = (float)init->data.f;
                    uint32_t bits;
                    memcpy(&bits, &fval, sizeof(bits));
                    anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n", bits);
                } else {
                    double dval = init->data.f;
                    uint64_t bits;
                    memcpy(&bits, &dval, sizeof(bits));
                    anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n",
                                         (uint32_t)(bits & 0xffffffffu));
                    anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n",
                                         (uint32_t)(bits >> 32));
                }
            } else if (init->kind == ANVIL_VAL_CONST_ARRAY) {
                int elem_size = 4;
                bool is_float_array = false;
                if (g->value->type && g->value->type->kind == ANVIL_TYPE_ARRAY &&
                    g->value->type->data.array.elem) {
                    elem_size = x86_type_size(g->value->type->data.array.elem);
                    is_float_array = x86_type_is_float(g->value->type->data.array.elem);
                }
                for (size_t i = 0; i < init->data.array.num_elements; i++) {
                    anvil_value_t *elem = init->data.array.elements[i];
                    if (is_float_array && elem && elem->kind == ANVIL_VAL_CONST_FLOAT) {
                        if (elem_size == 4) {
                            float fval = (float)elem->data.f;
                            uint32_t bits;
                            memcpy(&bits, &fval, sizeof(bits));
                            anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n", bits);
                        } else {
                            double dval = elem->data.f;
                            uint64_t bits;
                            memcpy(&bits, &dval, sizeof(bits));
                            anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n",
                                                 (uint32_t)(bits & 0xffffffffu));
                            anvil_strbuf_appendf(&priv->data, "\t.long 0x%x\n",
                                                 (uint32_t)(bits >> 32));
                        }
                    } else {
                        int64_t val = 0;
                        if (elem && elem->kind == ANVIL_VAL_CONST_INT) {
                            val = elem->data.i;
                        }
                        switch (elem_size) {
                            case 1: anvil_strbuf_appendf(&priv->data, "\t.byte %lld\n", (long long)val); break;
                            case 2: anvil_strbuf_appendf(&priv->data, "\t.short %lld\n", (long long)val); break;
                            case 4: anvil_strbuf_appendf(&priv->data, "\t.long %lld\n", (long long)val); break;
                            default: anvil_strbuf_appendf(&priv->data, "\t.quad %lld\n", (long long)val); break;
                        }
                    }
                }
            } else {
                anvil_strbuf_appendf(&priv->data, "\t.zero %d\n", size);
            }
        } else {
            anvil_strbuf_appendf(&priv->data, "\t.zero %d\n", size);
        }
    }

    anvil_strbuf_append(&priv->data, "\n");
}

static anvil_error_t x86_codegen_module(anvil_backend_t *be, anvil_module_t *mod,
                                        char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;

    x86_backend_priv_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);

    if (x86_is_macho(priv)) {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for x86 - macOS\n");
        anvil_strbuf_append(&priv->code, "\t.section __TEXT,__text,regular,pure_instructions\n\n");
    } else {
        anvil_strbuf_append(&priv->code, "# Generated by ANVIL for x86 (32-bit)\n");
        anvil_strbuf_append(&priv->code, "\t.text\n\n");
    }

    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_error_t err = x86_emit_func(priv, func);
        if (err != ANVIL_OK) return err;
    }

    x86_emit_globals(priv, mod);

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

static anvil_error_t x86_codegen_func(anvil_backend_t *be, anvil_func_t *func,
                                      char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;

    x86_backend_priv_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);

    anvil_error_t err = x86_emit_func(priv, func);
    if (err != ANVIL_OK) return err;

    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

const anvil_backend_ops_t anvil_backend_x86 = {
    .name = "x86",
    .arch = ANVIL_ARCH_X86,
    .init = x86_init,
    .cleanup = x86_cleanup,
    .reset = x86_reset,
    .codegen_module = x86_codegen_module,
    .codegen_func = x86_codegen_func,
    .get_arch_info = x86_get_arch_info
};
