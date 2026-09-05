#include "systemz_internal.h"

static bool systemz_validate_config(anvil_backend_t *be, const anvil_mainframe_target_desc_t *desc)
{
    if (!be->ctx)
        return true;

    if (be->ctx->arch != desc->arch || (be->ctx->abi != ANVIL_ABI_DEFAULT && be->ctx->abi != desc->abi_ops->abi) ||
        (be->ctx->syntax != ANVIL_SYNTAX_DEFAULT && be->ctx->syntax != desc->asm_ops->syntax)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "%s currently implements %s linkage with %s syntax", desc->name, desc->abi_ops->name, desc->asm_ops->name);
        return false;
    }

    return true;
}

anvil_error_t anvil_mainframe_codegen_func(anvil_backend_t *be, anvil_func_t *func, anvil_mainframe_variant_t variant, char **output, size_t *len)
{
    if (!be || !func || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);

    if (!desc)
        return ANVIL_ERR_INVALID_ARG;

    if (!systemz_validate_config(be, desc))
        return ANVIL_ERR_CODEGEN;

    if (func->is_declaration) {
        *output = calloc(1, 1);
        return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
    }

    anvil_mir_func_t *mir = anvil_mainframe_lower_func_to_mir(func, variant);
    if (!mir) {
        if (be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "mainframe MachineIR lowering failed for function %s", func->name ? func->name : "<anon>");
        }
        return ANVIL_ERR_CODEGEN;
    }

    char legal_error[256] = {0};
    bool ok = anvil_mainframe_verify_mir_legal(mir, variant, legal_error, sizeof(legal_error));
    if (!ok && be->ctx) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "mainframe MachineIR legalization failed for function %s: %s", func->name ? func->name : "<anon>",
                        legal_error[0] ? legal_error : "unknown legalizer error");
    }
    if (ok)
        ok = anvil_mainframe_regalloc_mir(mir, variant);
    if (!ok && be->ctx && !legal_error[0]) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "mainframe MachineIR register allocation failed for function %s", func->name ? func->name : "<anon>");
    }
    if (ok) {
        anvil_fp_format_t fp_format = be->ctx ? be->ctx->fp_format : anvil_mainframe_get_target_desc(variant)->fp_format;
        ok = desc->asm_ops->emit_mir(mir, variant, fp_format, output, len);
        if (!ok && be->ctx) {
            anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "mainframe HLASM emission failed for function %s", func->name ? func->name : "<anon>");
        }
    }
    anvil_mir_func_destroy(mir);
    return ok ? ANVIL_OK : ANVIL_ERR_CODEGEN;
}

anvil_error_t anvil_mainframe_codegen_module(anvil_backend_t *be, anvil_module_t *mod, anvil_mainframe_variant_t variant, char **output, size_t *len)
{
    if (!be || !mod || !output)
        return ANVIL_ERR_INVALID_ARG;
    *output = NULL;
    if (len)
        *len = 0;

    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc)
        return ANVIL_ERR_INVALID_ARG;

    if (!systemz_validate_config(be, desc))
        return ANVIL_ERR_CODEGEN;

    anvil_fp_format_t fp_format = be->ctx ? be->ctx->fp_format : desc->fp_format;
    anvil_strbuf_t out;
    anvil_strbuf_init(&out);

    bool ok = true;
    for (anvil_func_t *func = mod->funcs; ok && func; func = func->next) {
        if (func->is_declaration)
            continue;
        char *func_text = NULL;
        size_t func_len = 0;
        anvil_error_t err = anvil_mainframe_codegen_func(be, func, variant, &func_text, &func_len);
        if (err != ANVIL_OK) {
            ok = false;
            break;
        }
        anvil_strbuf_append(&out, func_text);
        free(func_text);
    }
    if (ok)
        ok = desc->asm_ops->emit_globals(&out, mod, fp_format, (size_t)desc->ptr_size);
    if (ok)
        desc->asm_ops->emit_module_end(&out);

    if (!ok || out.failed) {
        anvil_strbuf_destroy(&out);
        return ok ? ANVIL_ERR_NOMEM : ANVIL_ERR_CODEGEN;
    }
    *output = anvil_strbuf_detach(&out, len);
    return *output ? ANVIL_OK : ANVIL_ERR_NOMEM;
}

bool anvil_mainframe_emit_mir(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, char **output, size_t *len)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc)
        return false;
    return desc->asm_ops->emit_mir(mir, variant, desc->fp_format, output, len);
}
