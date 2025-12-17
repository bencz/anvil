/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Statement Analysis
 * 
 * This file handles semantic analysis of statements.
 */

#include "sema_internal.h"

/* ============================================================
 * Compound Statement Analysis
 * ============================================================ */

bool sema_analyze_compound_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_symtab_push_scope(sema->symtab);
    
    for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
        mcc_ast_node_t *s = stmt->data.compound_stmt.stmts[i];
        if (s->kind == AST_VAR_DECL || s->kind == AST_FUNC_DECL || s->kind == AST_DECL_LIST) {
            sema_analyze_decl(sema, s);
        } else {
            sema_analyze_stmt(sema, s);
        }
    }
    
    mcc_symtab_pop_scope(sema->symtab);
    return true;
}

/* ============================================================
 * If Statement Analysis
 * ============================================================ */

bool sema_analyze_if_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_type_t *cond_type = sema_analyze_expr(sema, stmt->data.if_stmt.cond);
    
    if (cond_type && !sema_check_scalar(sema, cond_type, stmt->location, "if condition")) {
        /* Continue analysis despite error */
    }
    
    sema_analyze_stmt(sema, stmt->data.if_stmt.then_stmt);
    
    if (stmt->data.if_stmt.else_stmt) {
        sema_analyze_stmt(sema, stmt->data.if_stmt.else_stmt);
    }
    
    return true;
}

/* ============================================================
 * While Statement Analysis
 * ============================================================ */

bool sema_analyze_while_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_type_t *cond_type = sema_analyze_expr(sema, stmt->data.while_stmt.cond);
    
    if (cond_type && !sema_check_scalar(sema, cond_type, stmt->location, "while condition")) {
        /* Continue analysis despite error */
    }
    
    sema->loop_depth++;
    sema_analyze_stmt(sema, stmt->data.while_stmt.body);
    sema->loop_depth--;
    
    return true;
}

/* ============================================================
 * Do-While Statement Analysis
 * ============================================================ */

bool sema_analyze_do_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    sema->loop_depth++;
    sema_analyze_stmt(sema, stmt->data.do_stmt.body);
    sema->loop_depth--;
    
    mcc_type_t *cond_type = sema_analyze_expr(sema, stmt->data.do_stmt.cond);
    
    if (cond_type && !sema_check_scalar(sema, cond_type, stmt->location, "do-while condition")) {
        /* Continue analysis despite error */
    }
    
    return true;
}

/* ============================================================
 * For Statement Analysis
 * ============================================================ */

bool sema_analyze_for_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_symtab_push_scope(sema->symtab);
    
    /* C99: for loop can have declaration in init */
    if (stmt->data.for_stmt.init_decl) {
        /* C99 feature: declaration in for loop */
        if (!sema_has_feature(sema, MCC_FEAT_FOR_DECL)) {
            mcc_warning_at(sema->ctx, stmt->location,
                           "declaration in for loop is a C99 extension");
        }
        sema_analyze_decl(sema, stmt->data.for_stmt.init_decl);
    } else if (stmt->data.for_stmt.init) {
        if (stmt->data.for_stmt.init->kind == AST_VAR_DECL) {
            /* C99 feature: declaration in for loop */
            if (!sema_has_feature(sema, MCC_FEAT_FOR_DECL)) {
                mcc_warning_at(sema->ctx, stmt->location,
                               "declaration in for loop is a C99 extension");
            }
            sema_analyze_decl(sema, stmt->data.for_stmt.init);
        } else {
            sema_analyze_expr(sema, stmt->data.for_stmt.init);
        }
    }
    
    if (stmt->data.for_stmt.cond) {
        mcc_type_t *cond_type = sema_analyze_expr(sema, stmt->data.for_stmt.cond);
        if (cond_type && !sema_check_scalar(sema, cond_type, stmt->location, "for condition")) {
            /* Continue analysis despite error */
        }
    }
    
    if (stmt->data.for_stmt.incr) {
        sema_analyze_expr(sema, stmt->data.for_stmt.incr);
    }
    
    sema->loop_depth++;
    sema_analyze_stmt(sema, stmt->data.for_stmt.body);
    sema->loop_depth--;
    
    mcc_symtab_pop_scope(sema->symtab);
    return true;
}

/* ============================================================
 * Switch Statement Analysis
 * ============================================================ */

bool sema_analyze_switch_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_type_t *expr_type = sema_analyze_expr(sema, stmt->data.switch_stmt.expr);
    
    if (expr_type && !sema_check_integer(sema, expr_type, stmt->location, "switch expression")) {
        /* Continue analysis despite error */
    }
    
    sema->switch_depth++;
    sema_analyze_stmt(sema, stmt->data.switch_stmt.body);
    sema->switch_depth--;
    
    return true;
}

/* ============================================================
 * Return Statement Analysis
 * ============================================================ */

bool sema_analyze_return_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_type_t *expr_type = NULL;
    
    if (stmt->data.return_stmt.expr) {
        expr_type = sema_analyze_expr(sema, stmt->data.return_stmt.expr);
    }
    
    if (sema->current_return_type) {
        if (mcc_type_is_void(sema->current_return_type)) {
            if (expr_type) {
                mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_VOID_RETURN);
            }
        } else {
            if (!expr_type) {
                mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_NONVOID_RETURN);
            } else {
                sema_check_assignment_compat(sema, sema->current_return_type, expr_type, stmt->location);
            }
        }
    }
    
    return true;
}

/* ============================================================
 * Case/Default Statement Analysis
 * ============================================================ */

static bool analyze_case_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (sema->switch_depth == 0) {
        mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_CASE_OUTSIDE);
    }
    
    /* Analyze case expression to resolve symbols (e.g., enum constants) */
    sema_analyze_expr(sema, stmt->data.case_stmt.expr);
    
    /* Case expression must be constant */
    int64_t case_val;
    if (!sema_eval_const_expr(sema, stmt->data.case_stmt.expr, &case_val)) {
        mcc_error_at(sema->ctx, stmt->location,
                     "case expression is not a constant");
    }
    
    sema_analyze_stmt(sema, stmt->data.case_stmt.stmt);
    return true;
}

static bool analyze_default_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (sema->switch_depth == 0) {
        mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_DEFAULT_OUTSIDE);
    }
    
    sema_analyze_stmt(sema, stmt->data.default_stmt.stmt);
    return true;
}

/* ============================================================
 * Break/Continue Statement Analysis
 * ============================================================ */

static bool analyze_break_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (sema->loop_depth == 0 && sema->switch_depth == 0) {
        mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_BREAK_OUTSIDE);
    }
    return true;
}

static bool analyze_continue_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (sema->loop_depth == 0) {
        mcc_error_at(sema->ctx, stmt->location, SEMA_ERR_CONTINUE_OUTSIDE);
    }
    return true;
}

/* ============================================================
 * Goto/Label Statement Analysis
 * ============================================================ */

static bool analyze_goto_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_symbol_t *label = mcc_symtab_lookup_label(sema->symtab, stmt->data.goto_stmt.label);
    if (!label) {
        /* Forward reference - will be checked at end of function */
        /* TODO: Track forward references and verify at function end */
    }
    return true;
}

static bool analyze_label_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    mcc_symtab_define_label(sema->symtab, stmt->data.label_stmt.label, stmt->location);
    sema_analyze_stmt(sema, stmt->data.label_stmt.stmt);
    return true;
}

/* ============================================================
 * Expression Statement Analysis
 * ============================================================ */

static bool analyze_expr_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (stmt->data.expr_stmt.expr) {
        sema_analyze_expr(sema, stmt->data.expr_stmt.expr);
    }
    return true;
}

/* ============================================================
 * Main Statement Analysis Entry Point
 * ============================================================ */

bool sema_analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (!stmt) return true;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            return sema_analyze_compound_stmt(sema, stmt);
            
        case AST_EXPR_STMT:
            return analyze_expr_stmt(sema, stmt);
            
        case AST_IF_STMT:
            return sema_analyze_if_stmt(sema, stmt);
            
        case AST_WHILE_STMT:
            return sema_analyze_while_stmt(sema, stmt);
            
        case AST_DO_STMT:
            return sema_analyze_do_stmt(sema, stmt);
            
        case AST_FOR_STMT:
            return sema_analyze_for_stmt(sema, stmt);
            
        case AST_SWITCH_STMT:
            return sema_analyze_switch_stmt(sema, stmt);
            
        case AST_CASE_STMT:
            return analyze_case_stmt(sema, stmt);
            
        case AST_DEFAULT_STMT:
            return analyze_default_stmt(sema, stmt);
            
        case AST_BREAK_STMT:
            return analyze_break_stmt(sema, stmt);
            
        case AST_CONTINUE_STMT:
            return analyze_continue_stmt(sema, stmt);
            
        case AST_RETURN_STMT:
            return sema_analyze_return_stmt(sema, stmt);
            
        case AST_GOTO_STMT:
            return analyze_goto_stmt(sema, stmt);
            
        case AST_LABEL_STMT:
            return analyze_label_stmt(sema, stmt);
            
        case AST_NULL_STMT:
            return true;
            
        default:
            return true;
    }
}
