/*
 * MCC - Micro C Compiler
 * Dead Code Elimination Passes
 * 
 * Implements dead code elimination and related passes.
 */

#include "opt_internal.h"

/* ============================================================
 * Unreachable Code After Return (Og)
 * 
 * Removes statements that appear after a return statement
 * in the same block.
 * ============================================================ */

typedef struct {
    int changes;
} unreachable_data_t;

static bool is_terminating_stmt(mcc_ast_node_t *stmt)
{
    if (!stmt) return false;
    
    switch (stmt->kind) {
        case AST_RETURN_STMT:
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_GOTO_STMT:
            return true;
            
        case AST_IF_STMT:
            /* If both branches terminate, the if terminates */
            if (stmt->data.if_stmt.else_stmt) {
                return is_terminating_stmt(stmt->data.if_stmt.then_stmt) &&
                       is_terminating_stmt(stmt->data.if_stmt.else_stmt);
            }
            return false;
            
        case AST_COMPOUND_STMT:
            /* A compound statement terminates if any statement in it terminates */
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                if (is_terminating_stmt(stmt->data.compound_stmt.stmts[i])) {
                    return true;
                }
            }
            return false;
            
        default:
            return false;
    }
}

static bool unreachable_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    unreachable_data_t *ud = (unreachable_data_t *)data;
    (void)opt;
    
    if (node->kind != AST_COMPOUND_STMT) return true;
    
    /* Find the first terminating statement */
    size_t term_idx = node->data.compound_stmt.num_stmts;
    for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
        if (is_terminating_stmt(node->data.compound_stmt.stmts[i])) {
            term_idx = i;
            break;
        }
    }
    
    /* Remove statements after the terminating statement */
    if (term_idx < node->data.compound_stmt.num_stmts - 1) {
        size_t removed = node->data.compound_stmt.num_stmts - term_idx - 1;
        node->data.compound_stmt.num_stmts = term_idx + 1;
        ud->changes += (int)removed;
    }
    
    return true;
}

int opt_pass_unreachable_after_return(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    unreachable_data_t data = { .changes = 0 };
    opt_visit_preorder(opt, ast, unreachable_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Dead Code Elimination (O1)
 * 
 * Removes code that has no effect:
 * - Statements with no side effects
 * - Unreachable code (after unconditional jumps)
 * - Empty blocks
 * ============================================================ */

typedef struct {
    int changes;
} dce_data_t;

static bool is_dead_stmt(mcc_ast_node_t *stmt)
{
    if (!stmt) return true;
    
    switch (stmt->kind) {
        case AST_NULL_STMT:
            return true;
            
        case AST_EXPR_STMT:
            /* Expression statement is dead if expression has no side effects */
            if (!stmt->data.expr_stmt.expr) return true;
            return !opt_has_side_effects(stmt->data.expr_stmt.expr);
            
        case AST_COMPOUND_STMT:
            /* Empty compound statement is dead */
            return stmt->data.compound_stmt.num_stmts == 0;
            
        default:
            return false;
    }
}

static bool dce_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    dce_data_t *dd = (dce_data_t *)data;
    (void)opt;
    
    if (node->kind != AST_COMPOUND_STMT) return true;
    
    /* Remove dead statements from compound statement */
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < node->data.compound_stmt.num_stmts; read_idx++) {
        mcc_ast_node_t *stmt = node->data.compound_stmt.stmts[read_idx];
        
        if (!is_dead_stmt(stmt)) {
            node->data.compound_stmt.stmts[write_idx++] = stmt;
        } else {
            dd->changes++;
        }
    }
    
    node->data.compound_stmt.num_stmts = write_idx;
    
    return true;
}

int opt_pass_dce(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    dce_data_t data = { .changes = 0 };
    opt_visit_preorder(opt, ast, dce_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Dead Store Elimination (O1)
 * 
 * Removes stores to variables that are never read.
 * Requires semantic analysis for variable tracking.
 * ============================================================ */

int opt_pass_dead_store(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    /* TODO: Implement dead store elimination */
    /* This requires tracking variable uses through the code */
    (void)opt;
    (void)ast;
    return 0;
}

/* ============================================================
 * Branch Simplification (O1)
 * 
 * Simplifies branches with constant conditions:
 * - if (1) { A } else { B } -> A
 * - if (0) { A } else { B } -> B
 * - while (0) { A } -> (nothing)
 * ============================================================ */

typedef struct {
    int changes;
} branch_simp_data_t;

static bool branch_simp_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    branch_simp_data_t *bsd = (branch_simp_data_t *)data;
    (void)opt;
    
    switch (node->kind) {
        case AST_IF_STMT: {
            int64_t cond_val;
            if (opt_eval_const_int(node->data.if_stmt.cond, &cond_val)) {
                mcc_ast_node_t *replacement;
                
                if (cond_val) {
                    /* if (true) -> then branch */
                    replacement = node->data.if_stmt.then_stmt;
                } else {
                    /* if (false) -> else branch (or empty) */
                    replacement = node->data.if_stmt.else_stmt;
                }
                
                if (replacement) {
                    node->kind = replacement->kind;
                    node->data = replacement->data;
                } else {
                    /* No else branch, make it an empty statement */
                    node->kind = AST_NULL_STMT;
                }
                bsd->changes++;
            }
            break;
        }
        
        case AST_WHILE_STMT: {
            int64_t cond_val;
            if (opt_eval_const_int(node->data.while_stmt.cond, &cond_val)) {
                if (!cond_val) {
                    /* while (false) -> empty */
                    node->kind = AST_NULL_STMT;
                    bsd->changes++;
                }
                /* Note: while (true) is an infinite loop, don't remove */
            }
            break;
        }
        
        case AST_TERNARY_EXPR: {
            int64_t cond_val;
            if (opt_eval_const_int(node->data.ternary_expr.cond, &cond_val)) {
                mcc_ast_node_t *replacement;
                
                if (cond_val) {
                    replacement = node->data.ternary_expr.then_expr;
                } else {
                    replacement = node->data.ternary_expr.else_expr;
                }
                
                if (replacement) {
                    node->kind = replacement->kind;
                    node->data = replacement->data;
                    node->type = replacement->type;
                    bsd->changes++;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return true;
}

int opt_pass_branch_simp(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    branch_simp_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, branch_simp_visitor, &data);
    return data.changes;
}
