#include "x86_32_internal.h"
#include "../common/gnu_data.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool x86_format_data_symbol(char *buffer, size_t capacity, const anvil_value_t *symbol, const char *default_prefix, void *user)
{
    x86_backend_priv_t *priv = user;
    if (!buffer || capacity == 0 || !symbol || !symbol->name || !priv)
        return false;

    int length;
    anvil_type_t *func_type = symbol->kind == ANVIL_VAL_FUNC && symbol->data.func ? symbol->data.func->type : NULL;
    if (!x86_is_coff(priv) || !func_type || func_type->kind != ANVIL_TYPE_FUNC) {
        length = snprintf(buffer, capacity, "%s%s", default_prefix ? default_prefix : "", symbol->name);
    } else {
        size_t argument_bytes = 0;
        for (size_t i = 0; i < func_type->data.func.num_params; i++) {
            anvil_type_t *param = func_type->data.func.params[i];
            size_t size = param && param->size ? param->size : 4;
            if (size > SIZE_MAX - 3)
                return false;
            size = (size + 3) & ~(size_t)3;
            if (argument_bytes > SIZE_MAX - size)
                return false;
            argument_bytes += size;
        }
        switch (func_type->data.func.cc) {
        case ANVIL_CC_CDECL:
            length = snprintf(buffer, capacity, "_%s", symbol->name);
            break;
        case ANVIL_CC_STDCALL:
            length = snprintf(buffer, capacity, "_%s@%zu", symbol->name, argument_bytes);
            break;
        case ANVIL_CC_FASTCALL:
            length = snprintf(buffer, capacity, "@%s@%zu", symbol->name, argument_bytes);
            break;
        default:
            return false;
        }
    }
    return length >= 0 && (size_t)length < capacity;
}

static anvil_error_t x86_emit_func(x86_backend_priv_t *priv, anvil_func_t *func)
{
    if (!priv || !func)
        return ANVIL_ERR_INVALID_ARG;
    if (func->is_declaration)
        return ANVIL_OK;

    anvil_abi_t abi = priv->ctx ? priv->ctx->abi : ANVIL_ABI_DEFAULT;

    anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(func);
    if (!mir) {
        if (priv->ctx) {
            anvil_set_error(priv->ctx, ANVIL_ERR_CODEGEN, "x86 MachineIR lowering failed for function %s", func->name ? func->name : "<anonymous>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    bool ok = anvil_x86_regalloc_mir(mir);
    char *mir_text = NULL;
    size_t mir_len = 0;
    if (ok) {
        ok = anvil_x86_emit_mir_abi(mir, func, abi, priv->syntax, &mir_text, &mir_len);
    }
    anvil_mir_func_destroy(mir);

    if (!ok || !mir_text) {
        free(mir_text);
        if (priv->ctx) {
            anvil_set_error(priv->ctx, ANVIL_ERR_CODEGEN, "x86 MachineIR emission failed for function %s", func->name ? func->name : "<anonymous>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    (void)mir_len;
    anvil_strbuf_append(&priv->code, mir_text);
    anvil_strbuf_append(&priv->code, "\n");
    free(mir_text);
    return ANVIL_OK;
}

static bool x86_emit_globals(x86_backend_priv_t *priv, anvil_module_t *mod)
{
    if (mod->num_globals == 0)
        return true;

    const char *prefix = x86_symbol_prefix(priv);

    int actual_globals = 0;
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC)
            continue;
        actual_globals++;
    }
    if (actual_globals == 0)
        return true;

    anvil_gnu_string_pool_t strings;
    anvil_gnu_string_pool_init(&strings, ".Lanvil_global_string_");

    anvil_strbuf_append(&priv->data, "\t.data\n");

    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC)
            continue;

        anvil_value_t *global = g->value;
        if (!global || !global->name || !global->type) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
        if (global->data.global.is_declaration) {
            anvil_strbuf_appendf(&priv->data, "\t.extern %s%s\n", prefix, global->name);
            continue;
        }
        if (global->data.global.linkage == ANVIL_LINK_EXTERNAL || global->data.global.linkage == ANVIL_LINK_COMMON) {
            anvil_strbuf_appendf(&priv->data, "\t.globl %s%s\n", prefix, global->name);
        } else if (global->data.global.linkage == ANVIL_LINK_WEAK) {
            anvil_strbuf_appendf(&priv->data, "\t.weak %s%s\n", prefix, global->name);
        }

        int align = g->value->type ? x86_type_align(g->value->type) : 4;

        if (x86_is_macho(priv)) {
            int p2align = (align <= 1) ? 0 : (align <= 2) ? 1 : (align <= 4) ? 2 : 3;
            anvil_strbuf_appendf(&priv->data, "\t.p2align %d\n", p2align);
        } else {
            anvil_strbuf_appendf(&priv->data, "\t.align %d\n", align);
        }

        anvil_strbuf_appendf(&priv->data, "%s%s:\n", prefix, g->value->name);

        if (!anvil_gnu_emit_constant(&priv->data, global->type, global->data.global.init, 4, prefix, &strings, x86_format_data_symbol, priv)) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
    }

    anvil_strbuf_append(&priv->data, "\n");
    const char *string_section = x86_is_macho(priv) ? "\t.section __TEXT,__cstring,cstring_literals" : (x86_is_coff(priv) ? "\t.section .rdata,\"dr\"" : "\t.section .rodata");
    bool ok = anvil_gnu_string_pool_emit(&priv->data, &strings, string_section);
    anvil_gnu_string_pool_destroy(&strings);
    return ok && !priv->data.failed;
}

anvil_error_t x86_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len)
{
    if (!be || !mod || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

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
        if (err != ANVIL_OK)
            return err;
    }

    if (!x86_emit_globals(priv, mod)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "x86 global initializer is not representable");
        return ANVIL_ERR_CODEGEN;
    }

    if (priv->code.failed || priv->data.failed)
        return ANVIL_ERR_NOMEM;

    anvil_strbuf_t result;
    anvil_strbuf_init(&result);
    if (result.failed)
        return ANVIL_ERR_NOMEM;
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

anvil_error_t x86_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len)
{
    if (!be || !func || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

    x86_backend_priv_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);

    anvil_error_t err = x86_emit_func(priv, func);
    if (err != ANVIL_OK)
        return err;
    if (priv->code.failed)
        return ANVIL_ERR_NOMEM;

    *output = anvil_strbuf_detach(&priv->code, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
