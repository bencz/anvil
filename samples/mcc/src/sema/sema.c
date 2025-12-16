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
    (void)sema;
    (void)func_type;
    (void)args;
    (void)num_args;
    (void)loc;
    return true;
}

bool mcc_sema_check_return(mcc_sema_t *sema, mcc_type_t *expr_type, mcc_location_t loc)
{
    (void)sema;
    (void)expr_type;
    (void)loc;
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
