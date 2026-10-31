#include "systemz_internal.h"
#include "../common/va_cursor.h"

static bool systemz_validate_config(anvil_backend_t *be, const anvil_mainframe_target_desc_t *desc)
{
    if (!be->ctx)
        return true;

    if (be->ctx->arch != desc->arch || (be->ctx->abi != ANVIL_ABI_DEFAULT && be->ctx->abi != desc->abi_ops->abi) ||
        (be->ctx->syntax != ANVIL_SYNTAX_DEFAULT && be->ctx->syntax != desc->asm_ops->syntax)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "%s currently implements %s linkage with %s syntax", desc->name, desc->abi_ops->name, desc->asm_ops->name);
        return false;
    }

    /* IEEE (BFP) arithmetic and constants need the BFP facility, which
     * S/390 CPUs before the G5 lack. */
    if (be->ctx->fp_format != ANVIL_FP_HFP && !anvil_ctx_has_feature(be->ctx, ANVIL_FEATURE_ZARCH_BFP)) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "IEEE floating point needs the BFP facility; the CPU model '%s' has only HFP",
                        anvil_cpu_model_name(be->ctx->cpu_model));
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
        ok = anvil_mainframe_regalloc_mir_features(mir, variant, be->ctx ? be->ctx->cpu_features
                                                                         : anvil_cpu_model_features(anvil_arch_default_cpu(desc->arch)));
    if (!ok && be->ctx && !legal_error[0]) {
        anvil_set_error(be->ctx, ANVIL_ERR_CODEGEN, "mainframe MachineIR register allocation failed for function %s", func->name ? func->name : "<anon>");
    }
    if (ok) {
        anvil_fp_format_t fp_format = be->ctx ? be->ctx->fp_format : anvil_mainframe_get_target_desc(variant)->fp_format;
        anvil_cpu_features_t features = be->ctx ? be->ctx->cpu_features
                                                : anvil_cpu_model_features(anvil_arch_default_cpu(desc->arch));
        ok = desc->asm_ops->emit_mir(mir, variant, fp_format, features, output, len);
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
    desc->asm_ops->emit_module_begin(&out, mod, desc);
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
    return desc->asm_ops->emit_mir(mir, variant, desc->fp_format,
                                   anvil_cpu_model_features(anvil_arch_default_cpu(desc->arch)), output, len);
}

bool anvil_mainframe_emit_mir_features(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant,
                                       anvil_cpu_features_t features, char **output, size_t *len)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc)
        return false;
    return desc->asm_ops->emit_mir(mir, variant, desc->fp_format, features, output, len);
}

/* MVS parameter lists: va_list is a cursor over the list of argument
 * addresses (the last carries the end-of-list bit). A 64-bit integer on
 * the 31-bit targets takes two entries; an aggregate's entry addresses
 * the copy made for the call. */
anvil_value_t *anvil_systemz_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx), *pointer = anvil_type_ptr(ctx, byte);
    if (!anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)) || !anvil_va_arg_type_ok(type)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "MVS va_arg requires an i8** cursor and a scalar or a sized aggregate");
        return NULL;
    }
    size_t word = anvil_type_size(pointer);
    bool wide = word == 8;
    anvil_type_t *integer = wide ? anvil_type_u64(ctx) : anvil_type_u32(ctx);
    bool pair = !wide && type->size == 8 && anvil_type_is_integer(type);

    anvil_value_t *cursor = anvil_build_load(ctx, pointer, storage, "va.cursor");
    anvil_value_t *step = wide ? anvil_const_i64(ctx, (int64_t)(word * (pair ? 2 : 1))) : anvil_const_i32(ctx, (int32_t)(word * (pair ? 2 : 1)));
    anvil_value_t *next = anvil_build_gep(ctx, byte, cursor, &step, 1, "va.cursor.next");
    if (!next || !anvil_build_store(ctx, next, storage))
        return NULL;
    anvil_value_t *entry = anvil_build_load(ctx, integer, anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, integer), "va.entry.pointer"), "va.entry");
    anvil_value_t *mask = wide ? anvil_const_u64(ctx, UINT64_MAX >> 1) : anvil_const_u32(ctx, UINT32_MAX >> 1);
    anvil_value_t *address = anvil_build_inttoptr(ctx, anvil_build_and(ctx, entry, mask, "va.address.bits"), anvil_type_ptr(ctx, type), name);
    return address;
}

anvil_value_t *anvil_systemz_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    return anvil_va_cursor_copy(be->ctx, cursor, "MVS", name);
}

bool anvil_systemz_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    return anvil_va_cursor_copy_into(be->ctx, destination, cursor, "MVS");
}
