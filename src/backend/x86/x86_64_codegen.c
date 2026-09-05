#include "x86_64_internal.h"
#include "../common/gnu_data.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool x64_abi_supported(anvil_abi_t abi)
{
    return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_SYSV || abi == ANVIL_ABI_DARWIN || abi == ANVIL_ABI_WIN64;
}

static anvil_abi_t x64_resolve_abi(anvil_ctx_t *ctx, anvil_func_t *func)
{
    anvil_abi_t abi = ctx ? ctx->abi : ANVIL_ABI_DEFAULT;
    if (func && func->type && func->type->kind == ANVIL_TYPE_FUNC) {
        if (func->type->data.func.cc == ANVIL_CC_WIN64)
            abi = ANVIL_ABI_WIN64;
        else if (func->type->data.func.cc == ANVIL_CC_SYSV && abi != ANVIL_ABI_DARWIN)
            abi = ANVIL_ABI_SYSV;
    }
    if (abi == ANVIL_ABI_DEFAULT)
        abi = ANVIL_ABI_SYSV;
    return abi;
}

static bool x64_is_darwin(x64_backend_t *be)
{
    return be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
}

static const char *x64_symbol_prefix(x64_backend_t *be)
{
    return x64_is_darwin(be) ? "_" : "";
}

static anvil_error_t x64_emit_func(x64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func)
        return ANVIL_ERR_INVALID_ARG;
    if (func->is_declaration)
        return ANVIL_OK;

    if (!be->ctx || !x64_abi_supported(be->ctx->abi)) {
        if (be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "x86-64 codegen supports SysV, Darwin, and Win64 ABIs");
        }
        return ANVIL_ERR_CODEGEN;
    }

    anvil_abi_t abi = x64_resolve_abi(be->ctx, func);

    anvil_mir_func_t *mir = anvil_x86_64_lower_func_to_mir(func);
    if (!mir) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "x86-64 MachineIR lowering failed for function %s", func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    bool ok = anvil_x86_64_regalloc_mir_abi(mir, abi);
    char *mir_text = NULL;
    size_t mir_len = 0;
    if (ok) {
        ok = anvil_x86_64_emit_mir_abi(mir, abi, be->syntax, &mir_text, &mir_len);
    }
    anvil_mir_func_destroy(mir);

    if (!ok || !mir_text) {
        free(mir_text);
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "x86-64 MachineIR emission failed for function %s", func->name ? func->name : "<anonymous>");
        return ANVIL_ERR_CODEGEN;
    }

    (void)mir_len;
    anvil_strbuf_append(&be->code, mir_text);
    anvil_strbuf_append(&be->code, "\n");
    free(mir_text);
    return ANVIL_OK;
}

static bool x64_emit_globals(x64_backend_t *be, anvil_module_t *mod)
{
    if (mod->num_globals == 0)
        return true;

    const char *prefix = x64_symbol_prefix(be);

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

    anvil_strbuf_append(&be->data, "\t.data\n");

    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (g->value->type && g->value->type->kind == ANVIL_TYPE_FUNC)
            continue;

        anvil_value_t *global = g->value;
        if (!global || !global->name || !global->type) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
        if (global->data.global.is_declaration) {
            anvil_strbuf_appendf(&be->data, "\t.extern %s%s\n", prefix, global->name);
            continue;
        }
        if (global->data.global.linkage == ANVIL_LINK_EXTERNAL || global->data.global.linkage == ANVIL_LINK_COMMON) {
            anvil_strbuf_appendf(&be->data, "\t.globl %s%s\n", prefix, global->name);
        } else if (global->data.global.linkage == ANVIL_LINK_WEAK) {
            anvil_strbuf_appendf(&be->data, "\t.weak %s%s\n", prefix, global->name);
        }

        int align = g->value->type ? x64_type_align(g->value->type) : 8;

        if (x64_is_darwin(be)) {
            int p2align = (align <= 1) ? 0 : (align <= 2) ? 1 : (align <= 4) ? 2 : 3;
            anvil_strbuf_appendf(&be->data, "\t.p2align %d\n", p2align);
        } else {
            anvil_strbuf_appendf(&be->data, "\t.align %d\n", align);
        }

        anvil_strbuf_appendf(&be->data, "%s%s:\n", prefix, g->value->name);

        if (!anvil_gnu_emit_constant(&be->data, global->type, global->data.global.init, 8, prefix, &strings, NULL, NULL)) {
            anvil_gnu_string_pool_destroy(&strings);
            return false;
        }
    }

    anvil_strbuf_append(&be->data, "\n");
    const char *string_section = x64_is_darwin(be) ? "\t.section __TEXT,__cstring,cstring_literals" : (be->ctx && be->ctx->abi == ANVIL_ABI_WIN64 ? "\t.section .rdata,\"dr\"" : "\t.section .rodata");
    bool ok = anvil_gnu_string_pool_emit(&be->data, &strings, string_section);
    anvil_gnu_string_pool_destroy(&strings);
    return ok && !be->data.failed;
}

anvil_error_t x64_codegen_module(anvil_backend_t *be, anvil_module_t *mod, char **output, size_t *len)
{
    if (!be || !mod || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

    x64_backend_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->num_strings = 0;

    if (x64_is_darwin(priv)) {
        anvil_strbuf_append(&priv->code, "// Generated by ANVIL for x86-64 - macOS\n");
        anvil_strbuf_append(&priv->code, "\t.section __TEXT,__text,regular,pure_instructions\n\n");
    } else {
        anvil_strbuf_append(&priv->code, priv->ctx && priv->ctx->abi == ANVIL_ABI_WIN64 ? "# Generated by ANVIL for x86-64 - Windows\n" : "# Generated by ANVIL for x86-64 - Linux\n");
        anvil_strbuf_append(&priv->code, "\t.text\n\n");
    }

    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_error_t err = x64_emit_func(priv, func);
        if (err != ANVIL_OK)
            return err;
    }

    if (!x64_emit_globals(priv, mod)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "x86-64 global initializer is not representable");
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

anvil_error_t x64_codegen_func(anvil_backend_t *be, anvil_func_t *func, char **output, size_t *len)
{
    if (!be || !func || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

    x64_backend_t *priv = be->priv;

    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);

    anvil_error_t err = x64_emit_func(priv, func);
    if (err != ANVIL_OK)
        return err;
    if (priv->code.failed)
        return ANVIL_ERR_NOMEM;

    *output = anvil_strbuf_detach(&priv->code, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}
