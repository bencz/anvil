/*
 * MCC - Micro C Compiler
 * Function Inlining Optimization Passes
 * 
 * Implements tail call optimization and function inlining.
 */

#include "opt_internal.h"

/* ============================================================
 * Tail Call Optimization (O2)
 * 
 * Detects and marks tail calls for optimization.
 * A tail call is a function call that is the last action before
 * returning from a function.
 * 
 * Pattern: return f(args);
 * 
 * At AST level, we can detect tail calls but the actual optimization
 * (converting to a jump) happens at code generation or IR level.
 * Here we mark them for the backend to optimize.
 * ============================================================ */

typedef struct {
    mcc_ast_opt_t *opt;
    const char *current_func;   /* Name of current function */
    int tail_calls_found;
} tail_call_ctx_t;

/* Check if a return statement contains a tail call */
static bool is_tail_call(mcc_ast_node_t *ret_stmt, const char *func_name)
{
    if (!ret_stmt || ret_stmt->kind != AST_RETURN_STMT) return false;
    
    mcc_ast_node_t *expr = ret_stmt->data.return_stmt.expr;
    if (!expr) return false;
    
    /* Check if return expression is a function call */
    if (expr->kind != AST_CALL_EXPR) return false;
    
    /* Check if it's a recursive call (self-tail-call) */
    mcc_ast_node_t *func = expr->data.call_expr.func;
    if (func && func->kind == AST_IDENT_EXPR) {
        if (func_name && func->data.ident_expr.name &&
            strcmp(func->data.ident_expr.name, func_name) == 0) {
            return true;  /* Recursive tail call */
        }
    }
    
    /* Any tail call (not just recursive) can be optimized */
    return true;
}

/* Process a statement looking for tail calls */
static void find_tail_calls_stmt(tail_call_ctx_t *ctx, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            /* Only the last statement can be a tail call */
            if (stmt->data.compound_stmt.num_stmts > 0) {
                size_t last = stmt->data.compound_stmt.num_stmts - 1;
                find_tail_calls_stmt(ctx, stmt->data.compound_stmt.stmts[last]);
            }
            break;
            
        case AST_RETURN_STMT:
            if (is_tail_call(stmt, ctx->current_func)) {
                ctx->tail_calls_found++;
                /* Mark the call as a tail call (could add a flag to AST) */
                /* For now, just count them */
            }
            break;
            
        case AST_IF_STMT:
            /* Both branches could end with tail calls */
            find_tail_calls_stmt(ctx, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                find_tail_calls_stmt(ctx, stmt->data.if_stmt.else_stmt);
            }
            break;
            
        case AST_SWITCH_STMT:
            find_tail_calls_stmt(ctx, stmt->data.switch_stmt.body);
            break;
            
        case AST_CASE_STMT:
            find_tail_calls_stmt(ctx, stmt->data.case_stmt.stmt);
            break;
            
        case AST_DEFAULT_STMT:
            find_tail_calls_stmt(ctx, stmt->data.default_stmt.stmt);
            break;
            
        default:
            break;
    }
}

int opt_pass_tail_call(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    tail_call_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.tail_calls_found = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                ctx.current_func = decl->data.func_decl.name;
                find_tail_calls_stmt(&ctx, decl->data.func_decl.body);
            }
        }
    }
    
    /* Note: This pass currently only detects tail calls.
     * The actual optimization would require:
     * 1. Adding a flag to AST_CALL_EXPR to mark tail calls
     * 2. Code generator using this flag to emit jumps instead of calls
     * 
     * For now, return 0 since we don't actually transform the AST. */
    return 0;
}

/* ============================================================
 * Small Function Inlining (O2)
 * 
 * Inlines small functions at call sites.
 * A function is considered "small" if:
 * - It has a simple body (few statements)
 * - It doesn't have complex control flow
 * - It's not recursive
 * 
 * At AST level, inlining requires:
 * 1. Finding the function definition
 * 2. Substituting parameters with arguments
 * 3. Handling return values
 * 4. Renaming local variables to avoid conflicts
 * 
 * This is complex and error-prone at AST level.
 * ============================================================ */

#define MAX_INLINE_STMTS 5      /* Max statements for inlining */
#define MAX_INLINE_PARAMS 4     /* Max parameters for inlining */

typedef struct {
    const char *name;
    mcc_ast_node_t *decl;
    int num_stmts;
    int num_params;
    bool is_recursive;
    bool is_simple;
} inline_candidate_t;

typedef struct {
    mcc_ast_opt_t *opt;
    inline_candidate_t *candidates;
    int num_candidates;
    int max_candidates;
    int changes;
} inline_ctx_t;

/* Count statements in a function body */
static int count_stmts(mcc_ast_node_t *stmt)
{
    if (!stmt) return 0;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT: {
            int count = 0;
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                count += count_stmts(stmt->data.compound_stmt.stmts[i]);
            }
            return count;
        }
        
        case AST_IF_STMT:
            return 1 + count_stmts(stmt->data.if_stmt.then_stmt) +
                   (stmt->data.if_stmt.else_stmt ? count_stmts(stmt->data.if_stmt.else_stmt) : 0);
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            return 1 + count_stmts(stmt->data.while_stmt.body);
            
        case AST_FOR_STMT:
            return 1 + count_stmts(stmt->data.for_stmt.body);
            
        case AST_SWITCH_STMT:
            return 1 + count_stmts(stmt->data.switch_stmt.body);
            
        default:
            return 1;
    }
}

/* Check if function body is simple enough for inlining */
static bool is_simple_body(mcc_ast_node_t *body)
{
    if (!body) return false;
    
    /* Must be a compound statement */
    if (body->kind != AST_COMPOUND_STMT) return false;
    
    /* Check each statement */
    for (size_t i = 0; i < body->data.compound_stmt.num_stmts; i++) {
        mcc_ast_node_t *stmt = body->data.compound_stmt.stmts[i];
        
        /* No loops allowed in simple functions */
        if (stmt->kind == AST_WHILE_STMT ||
            stmt->kind == AST_DO_STMT ||
            stmt->kind == AST_FOR_STMT) {
            return false;
        }
        
        /* No goto/labels */
        if (stmt->kind == AST_GOTO_STMT ||
            stmt->kind == AST_LABEL_STMT) {
            return false;
        }
    }
    
    return true;
}

/* Check if function calls itself (recursive) */
static bool check_recursive(mcc_ast_node_t *node, const char *func_name)
{
    if (!node || !func_name) return false;
    
    switch (node->kind) {
        case AST_CALL_EXPR: {
            mcc_ast_node_t *func = node->data.call_expr.func;
            if (func && func->kind == AST_IDENT_EXPR &&
                func->data.ident_expr.name &&
                strcmp(func->data.ident_expr.name, func_name) == 0) {
                return true;
            }
            /* Check arguments */
            for (size_t i = 0; i < node->data.call_expr.num_args; i++) {
                if (check_recursive(node->data.call_expr.args[i], func_name)) {
                    return true;
                }
            }
            break;
        }
        
        case AST_BINARY_EXPR:
            return check_recursive(node->data.binary_expr.lhs, func_name) ||
                   check_recursive(node->data.binary_expr.rhs, func_name);
            
        case AST_UNARY_EXPR:
            return check_recursive(node->data.unary_expr.operand, func_name);
            
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
                if (check_recursive(node->data.compound_stmt.stmts[i], func_name)) {
                    return true;
                }
            }
            break;
            
        case AST_EXPR_STMT:
            return check_recursive(node->data.expr_stmt.expr, func_name);
            
        case AST_RETURN_STMT:
            return check_recursive(node->data.return_stmt.expr, func_name);
            
        case AST_IF_STMT:
            return check_recursive(node->data.if_stmt.cond, func_name) ||
                   check_recursive(node->data.if_stmt.then_stmt, func_name) ||
                   check_recursive(node->data.if_stmt.else_stmt, func_name);
            
        case AST_VAR_DECL:
            return check_recursive(node->data.var_decl.init, func_name);
            
        default:
            break;
    }
    
    return false;
}

/* Count function parameters */
static int count_params(mcc_ast_node_t *func_decl)
{
    if (!func_decl || func_decl->kind != AST_FUNC_DECL) return 0;
    return (int)func_decl->data.func_decl.num_params;
}

int opt_pass_inline_small(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    if (ast->kind != AST_TRANSLATION_UNIT) return 0;
    
    /* Phase 1: Find inline candidates */
    inline_candidate_t candidates[32];
    int num_candidates = 0;
    
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
        if (decl->kind != AST_FUNC_DECL || !decl->data.func_decl.body) continue;
        if (num_candidates >= 32) break;
        
        const char *name = decl->data.func_decl.name;
        mcc_ast_node_t *body = decl->data.func_decl.body;
        
        int num_stmts = count_stmts(body);
        int num_params = count_params(decl);
        bool is_recursive = check_recursive(body, name);
        bool is_simple = is_simple_body(body);
        
        /* Check if function is small enough */
        if (num_stmts <= MAX_INLINE_STMTS &&
            num_params <= MAX_INLINE_PARAMS &&
            !is_recursive &&
            is_simple) {
            
            candidates[num_candidates].name = name;
            candidates[num_candidates].decl = decl;
            candidates[num_candidates].num_stmts = num_stmts;
            candidates[num_candidates].num_params = num_params;
            candidates[num_candidates].is_recursive = is_recursive;
            candidates[num_candidates].is_simple = is_simple;
            num_candidates++;
        }
    }
    
    /* Phase 2: Inline at call sites
     * Note: Actually performing inlining at AST level is complex.
     * It requires:
     * - Cloning the function body
     * - Substituting parameters with arguments
     * - Renaming local variables
     * - Handling return statements
     * 
     * For now, we just identify candidates. Full implementation
     * would require significant AST manipulation infrastructure.
     */
    
    (void)opt;
    (void)candidates;
    
    return 0;  /* No actual changes yet */
}

/* ============================================================
 * Aggressive Inlining (O3)
 * 
 * More aggressive version of inlining that:
 * - Inlines larger functions
 * - May inline recursive functions (with depth limit)
 * - Considers call frequency hints
 * ============================================================ */

#define MAX_AGGR_INLINE_STMTS 15
#define MAX_AGGR_INLINE_PARAMS 8

int opt_pass_inline_aggr(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    /* Similar to inline_small but with higher thresholds */
    /* For now, just call the small inliner */
    (void)opt;
    (void)ast;
    return 0;
}
