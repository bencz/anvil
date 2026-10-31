/*
 * MCC - Micro C Compiler
 * Code Generator - Declaration Generation
 * 
 * This file handles code generation for declarations (functions, global variables).
 */

#include "codegen_internal.h"
#include <limits.h>

/* Generate code for function */
void codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func)
{
    if (!func->data.func_decl.is_definition) {
        /* Just a declaration, no code to generate */
        return;
    }
    
    /* Create function type */
    anvil_abi_value_plan_t result_plan;
    if (!codegen_abi_plan(cg, func->data.func_decl.func_type, true, &result_plan))
        return;

    anvil_type_t *ret_type = result_plan.transport_type;
    size_t hidden = result_plan.kind == ANVIL_ABI_VALUE_INDIRECT ? 1 : 0;
    if (!ret_type || func->data.func_decl.num_params > (size_t)INT_MAX ||
        func->data.func_decl.num_params >
            SIZE_MAX / sizeof(anvil_type_t *)) {
        if (ret_type) mcc_error(cg->mcc_ctx,
                                "function parameter table overflow");
        return;
    }
    int num_params = (int)func->data.func_decl.num_params;
    anvil_type_t **param_types = NULL;
    bool attrs_mode = codegen_abi_uses_param_attrs(cg);
    anvil_param_attr_t *attrs = NULL;
    
    if (num_params > 0 || hidden) {
        param_types = mcc_alloc_array(cg->mcc_ctx, (size_t)num_params + hidden, sizeof(*param_types));
        attrs = attrs_mode
            ? mcc_alloc_array(cg->mcc_ctx, (size_t)num_params + hidden, sizeof(*attrs))
            : NULL;
        if (!param_types || (attrs_mode && !attrs))
            return;

        if (hidden) {
            param_types[0] = ret_type;
            if (attrs_mode) {
                /* The result object is written through the SRET pointer and
                 * the target returns it as its ABI requires. */
                attrs[0] = ANVIL_PARAM_SRET;
                ret_type = anvil_type_void(cg->anvil_ctx);
            }
        }
        for (int i = 0; i < num_params; i++) {
            mcc_ast_node_t *p = func->data.func_decl.params[i];
            param_types[i + hidden] = codegen_param_type(cg, p->data.param_decl.param_type);
            if (!param_types[i + hidden])
                return;
            if (attrs_mode && codegen_type_is_record(p->data.param_decl.param_type))
                attrs[i + hidden] = ANVIL_PARAM_BYVAL;
        }
    }
    
    anvil_type_t *func_type = anvil_type_func_attrs(cg->anvil_ctx, ret_type, param_types, attrs,
                                                     (size_t)num_params + hidden,
                                                     func->data.func_decl.is_variadic, ANVIL_CC_DEFAULT);
    if (!func_type) return;
    
    /* Create function */
    anvil_linkage_t linkage = func->data.func_decl.is_static ? 
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    mcc_symbol_t *func_sym = func->data.func_decl.symbol;
    if (!func_sym)
        func_sym = mcc_symtab_lookup(cg->symtab, func->data.func_decl.name);
    cg->current_func = anvil_func_create(cg->anvil_mod,
                                         codegen_link_name(func_sym, func->data.func_decl.name),
                                         func_type, linkage);
    if (!cg->current_func) {
        mcc_error_at(cg->mcc_ctx, func->location,
                     "cannot define function '%s': %s",
                     func->data.func_decl.name,
                     anvil_ctx_get_error(cg->anvil_ctx));
        return;
    }
    anvil_func_set_fp_vectorization(cg->current_func, cg->mcc_ctx->options.fp_vectorization);
    anvil_func_set_fp_reassociation(cg->current_func, cg->mcc_ctx->options.fp_reassociation);
    anvil_func_set_fp_contraction(cg->current_func, cg->mcc_ctx->options.fp_contraction);
    cg->current_func_name = func->data.func_decl.name;  /* For __func__ (C99) */
    cg->current_return_type = func->data.func_decl.func_type;
    cg->current_result_pointer = hidden ? anvil_func_get_param(cg->current_func, 0) : NULL;
    
    /* Register function in mapping */
    if (func_sym) {
        codegen_add_func(cg, func_sym, cg->current_func);
    }
    
    /* Builder failures inside the body are reported once, below. */
    anvil_ctx_clear_error(cg->anvil_ctx);

    /* Create entry block */
    anvil_block_t *entry = anvil_func_get_entry(cg->current_func);
    codegen_set_current_block(cg, entry);
    
    /* Reset locals */
    cg->num_locals = 0;
    cg->num_labels = 0;
    cg->num_vla_lengths = 0;
    
    /* Allocate space for parameters and add to locals */
    for (int i = 0; i < num_params; i++) {
        mcc_ast_node_t *p = func->data.func_decl.params[i];
        if (p->data.param_decl.name) {
            anvil_value_t *param = anvil_func_get_param(cg->current_func, (size_t)i + hidden);
            if (codegen_type_is_record(p->data.param_decl.param_type) && attrs_mode) {
                /* A BYVAL parameter already points at the callee's copy. */
                codegen_add_local(cg, p->data.param_decl.name, param);
                continue;
            }
            if (codegen_type_is_record(p->data.param_decl.param_type)) {
                mcc_type_t *object_type = p->data.param_decl.param_type;
                anvil_abi_value_plan_t plan;
                if (!codegen_abi_plan(cg, object_type, false, &plan))
                    return;

                anvil_value_t *local = anvil_build_alloca(cg->anvil_ctx, codegen_type(cg, object_type), p->data.param_decl.name);
                if (!codegen_abi_unpack(cg, object_type, param, local, &plan))
                    return;

                codegen_add_local(cg, p->data.param_decl.name, local);
                continue;
            }
            anvil_value_t *alloca_val = anvil_build_alloca(cg->anvil_ctx, param_types[i + hidden],
                                                           p->data.param_decl.name);
            codegen_store_object(cg, p->data.param_decl.param_type, param, alloca_val);
            
            /* Add parameter to locals by name */
            codegen_add_local(cg, p->data.param_decl.name, alloca_val);
        }
    }
    
    /* Variably modified parameter types (int m[rows][cols]) evaluate their
     * dimensions on entry, after every parameter is addressable. */
    for (int i = 0; i < num_params; i++)
        codegen_vla_bind(cg, func->data.func_decl.params[i]->data.param_decl.param_type);

    /* The entry block keeps the prologue and receives storage for
     * declarations in unreachable code; it branches to the body last. */
    cg->alloca_block = entry;
    anvil_block_t *body = anvil_block_create(cg->current_func, "func.body");
    if (!body) return;
    codegen_set_current_block(cg, body);

    /* Generate body */
    codegen_stmt(cg, func->data.func_decl.body);

    /* A rejected ANVIL builder call yields NULL, and a NULL operand makes
     * the enclosing statement vanish.  Never let that pass silently. */
    if (anvil_ctx_get_last_error(cg->anvil_ctx) != ANVIL_OK) {
        mcc_error_at(cg->mcc_ctx, func->location,
                     "internal code generation error in '%s': %s",
                     func->data.func_decl.name,
                     anvil_ctx_get_error(cg->anvil_ctx));
    }
    
    /* Add implicit return if needed */
    if (!codegen_block_has_terminator(cg)) {
        if (ret_type == anvil_type_void(cg->anvil_ctx)) {
            anvil_build_ret_void(cg->anvil_ctx);
        } else if (hidden && attrs_mode) {
            anvil_build_ret_void(cg->anvil_ctx);
        } else if (hidden) {
            anvil_build_ret(cg->anvil_ctx, cg->current_result_pointer);
        } else {
            anvil_value_t *zero = NULL;
            if (anvil_type_is_integer(ret_type))
                zero = codegen_const_int_for_type(cg, ret_type, 0);
            else if (anvil_type_is_pointer(ret_type))
                zero = anvil_const_null(cg->anvil_ctx, ret_type);
            else if (anvil_type_is_floating(ret_type))
                zero = anvil_type_size(ret_type) == 4 ? anvil_const_f32(cg->anvil_ctx, 0) : anvil_const_f64(cg->anvil_ctx, 0);

            anvil_build_ret(cg->anvil_ctx, zero);
        }
    }

    codegen_set_current_block(cg, entry);
    anvil_build_br(cg->anvil_ctx, body);
    cg->alloca_block = NULL;

    /* Statements emitted after a jump (to keep nested labels reachable)
     * leave unreachable blocks behind. */
    if (anvil_func_remove_unreachable_blocks(cg->current_func) != ANVIL_OK)
        mcc_error_at(cg->mcc_ctx, func->location,
                     "cannot remove unreachable code in '%s': %s",
                     func->data.func_decl.name,
                     anvil_ctx_get_error(cg->anvil_ctx));
    
    cg->current_func = NULL;
    cg->current_block = NULL;
    cg->current_return_type = NULL;
    cg->current_result_pointer = NULL;
}

/* A const, non-volatile object (or array of them) is never written after
 * initialization, so the target may place it in read-only storage. */
static bool codegen_object_is_readonly(const mcc_type_t *type)
{
    while (type && type->kind == TYPE_ARRAY && !(type->qualifiers & QUAL_CONST))
        type = type->data.array.element;
    return type && (type->qualifiers & QUAL_CONST) && !(type->qualifiers & QUAL_VOLATILE);
}

/* Generate code for global variable */
void codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var)
{
    anvil_type_t *type = codegen_type(cg, var->data.var_decl.var_type);
    mcc_symbol_t *sym = var->data.var_decl.symbol;
    anvil_linkage_t linkage = var->data.var_decl.is_static ||
        (sym && sym->storage == STORAGE_STATIC)
        ? ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    const char *name = codegen_link_name(sym, var->data.var_decl.name);
    
    /* Use cache to avoid duplicate global definitions */
    anvil_value_t *global = codegen_get_or_add_global(cg, name, type, linkage);
    if (!global)
        return;
    
    mcc_ast_node_t *init = var->data.var_decl.init;
    if (!var->data.var_decl.is_extern || init)
    {
        for (size_t index = 0; index < cg->num_globals; index++)
        {
            if (cg->globals[index].value != global || cg->globals[index].is_defined)
                continue;

            if (!anvil_module_add_global(cg->anvil_mod, name, type, linkage))
                return;

            cg->globals[index].is_defined = true;
            break;
        }
    }

    if (init) {
        anvil_value_t *constant = codegen_const_initializer(
            cg, var->data.var_decl.var_type, init);
        if (!constant) {
            mcc_error_at(cg->mcc_ctx, init->location,
                         "unsupported or invalid constant initializer for global '%s'",
                         var->data.var_decl.name);
            return;
        }
        anvil_global_set_initializer(global, constant);
    }
    if (codegen_object_is_readonly(var->data.var_decl.var_type) && !var->data.var_decl.is_extern)
        anvil_global_set_constant(global, true);
}

/* Generate code for any declaration */
void codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl)
{
    if (!decl) return;
    
    switch (decl->kind) {
        case AST_FUNC_DECL:
            codegen_func(cg, decl);
            break;
        case AST_VAR_DECL:
            codegen_global_var(cg, decl);
            break;
        case AST_DECL_LIST:
            /* Multiple declarations: int a, b, c; */
            for (size_t i = 0; i < decl->data.decl_list.num_decls; i++) {
                codegen_decl(cg, decl->data.decl_list.decls[i]);
            }
            break;
        default:
            break;
    }
}
