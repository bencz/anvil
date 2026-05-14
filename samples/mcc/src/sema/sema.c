/*
 * MCC - Micro C Compiler
 * Semantic Analysis Main Module
 * 
 * This file contains the public API and core operations for semantic
 * analysis. The actual analysis logic is split into:
 * - sema_expr.c  - Expression analysis
 * - sema_stmt.c  - Statement analysis
 * - sema_decl.c  - Declaration analysis
 * - sema_type.c  - Type checking utilities
 * - sema_const.c - Constant expression evaluation
 */

#include "sema_internal.h"

/* ============================================================
 * Sema Creation/Destruction
 * ============================================================ */

mcc_sema_t *mcc_sema_create(mcc_context_t *ctx)
{
    mcc_sema_t *sema = mcc_alloc(ctx, sizeof(mcc_sema_t));
    sema->ctx = ctx;
    sema->types = mcc_type_context_create(ctx);
    sema->symtab = mcc_symtab_create(ctx, sema->types);
    sema->current_func = NULL;
    sema->current_return_type = NULL;
    sema->loop_depth = 0;
    sema->switch_depth = 0;
    return sema;
}

void mcc_sema_destroy(mcc_sema_t *sema)
{
    (void)sema; /* Arena allocated */
}

/* ============================================================
 * Main Analysis Entry Points
 * ============================================================ */

bool mcc_sema_analyze(mcc_sema_t *sema, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        sema_analyze_decl(sema, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(sema->ctx);
}

bool mcc_sema_analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    return sema_analyze_decl(sema, decl);
}

bool mcc_sema_analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    return sema_analyze_stmt(sema, stmt);
}

mcc_type_t *mcc_sema_analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    return sema_analyze_expr(sema, expr);
}

/* ============================================================
 * Public Type Checking Utilities
 * ============================================================ */

bool mcc_sema_check_assignment(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs,
                                mcc_location_t loc)
{
    return sema_check_assignment_compat(sema, lhs, rhs, loc);
}

bool mcc_sema_check_call(mcc_sema_t *sema, mcc_type_t *func_type,
                          mcc_ast_node_t **args, size_t num_args, mcc_location_t loc)
{
    if (!func_type) return false;

    /* Peel any pointer-to-function indirection. */
    mcc_type_t *ft = func_type;
    if (ft->kind == TYPE_POINTER && ft->data.pointer.pointee &&
        ft->data.pointer.pointee->kind == TYPE_FUNCTION) {
        ft = ft->data.pointer.pointee;
    }
    if (ft->kind != TYPE_FUNCTION) {
        mcc_error_at(sema->ctx, loc, "called object is not a function");
        return false;
    }

    size_t expected = ft->data.function.num_params;
    bool variadic = ft->data.function.is_variadic;

    /* Old-style (no prototype) declaration — expected==0 and !variadic — is
     * treated as "any number of args"; the parser records this via num_params=0. */
    if (expected == 0 && !variadic) {
        return true;
    }

    if (num_args < expected || (!variadic && num_args > expected)) {
        mcc_error_at(sema->ctx, loc,
            "call has %zu argument(s); expected %s%zu",
            num_args, variadic ? "at least " : "", expected);
        return false;
    }

    /* Check each argument is assignment-compatible with its parameter. */
    mcc_func_param_t *param = ft->data.function.params;
    for (size_t i = 0; i < expected && param; i++, param = param->next) {
        mcc_ast_node_t *arg = args[i];
        if (!arg || !arg->type) continue;
        if (!sema_check_assignment_compat(sema, param->type, arg->type, loc)) {
            return false;
        }
    }
    return true;
}

bool mcc_sema_check_return(mcc_sema_t *sema, mcc_type_t *expr_type, mcc_location_t loc)
{
    mcc_type_t *want = sema->current_return_type;
    if (!want) return true; /* no enclosing function context */

    if (!expr_type) {
        /* bare `return;` */
        if (want->kind != TYPE_VOID) {
            mcc_error_at(sema->ctx, loc,
                "non-void function must return a value");
            return false;
        }
        return true;
    }

    if (want->kind == TYPE_VOID) {
        mcc_error_at(sema->ctx, loc, "void function should not return a value");
        return false;
    }

    if (!mcc_type_is_compatible(want, expr_type) &&
        !(mcc_type_is_arithmetic(want) && mcc_type_is_arithmetic(expr_type))) {
        mcc_warning_at(sema->ctx, loc,
            "incompatible return type: '%s' vs '%s'",
            mcc_type_to_string(expr_type), mcc_type_to_string(want));
    }
    return true;
}

/* ============================================================
 * Implicit Cast Insertion
 * ============================================================ */

mcc_ast_node_t *mcc_sema_implicit_cast(mcc_sema_t *sema, mcc_ast_node_t *expr,
                                        mcc_type_t *target)
{
    if (!expr || !target) return expr;
    if (mcc_type_is_same(expr->type, target)) return expr;
    
    mcc_ast_node_t *cast = mcc_ast_create(sema->ctx, AST_CAST_EXPR, expr->location);
    cast->data.cast_expr.target_type = target;
    cast->data.cast_expr.expr = expr;
    cast->type = target;
    return cast;
}

/* ============================================================
 * Constant Expression Evaluation (Public API)
 * ============================================================ */

bool mcc_sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr, int64_t *result)
{
    return sema_eval_const_expr(sema, expr, result);
}
