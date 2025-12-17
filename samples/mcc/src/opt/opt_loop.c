/*
 * MCC - Micro C Compiler
 * Loop Optimization Passes
 * 
 * Implements loop simplification and related optimizations.
 */

#include "opt_internal.h"

/* ============================================================
 * Loop Simplification (O2)
 * 
 * Simplifies loop structures:
 * - Remove loops with constant false condition: while(0) { } -> nothing
 * - Convert do-while(0) to single execution
 * - Simplify for loops with constant bounds
 * ============================================================ */

typedef struct {
    mcc_ast_opt_t *opt;
    int changes;
} loop_ctx_t;

/* Check if expression is constant zero */
static bool is_const_zero(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    int64_t val;
    if (opt_eval_const_int(expr, &val)) {
        return val == 0;
    }
    return false;
}

/* Process a statement for loop simplification */
static bool loop_simp_stmt(loop_ctx_t *ctx, mcc_ast_node_t **stmt_ptr);

static bool loop_simp_stmt(loop_ctx_t *ctx, mcc_ast_node_t **stmt_ptr)
{
    if (!stmt_ptr || !*stmt_ptr) return false;
    
    mcc_ast_node_t *stmt = *stmt_ptr;
    bool changed = false;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                if (loop_simp_stmt(ctx, &stmt->data.compound_stmt.stmts[i])) {
                    changed = true;
                }
            }
            break;
            
        case AST_WHILE_STMT:
            /* while(0) { body } -> empty */
            if (is_const_zero(stmt->data.while_stmt.cond)) {
                /* Replace with null statement */
                stmt->kind = AST_NULL_STMT;
                ctx->changes++;
                changed = true;
            } else {
                /* Process body */
                loop_simp_stmt(ctx, &stmt->data.while_stmt.body);
            }
            break;
            
        case AST_DO_STMT:
            /* do { body } while(0) -> body (single execution) */
            if (is_const_zero(stmt->data.do_stmt.cond)) {
                /* Replace do-while with just the body */
                mcc_ast_node_t *body = stmt->data.do_stmt.body;
                if (body) {
                    *stmt_ptr = body;
                    ctx->changes++;
                    changed = true;
                    /* Process the body recursively */
                    loop_simp_stmt(ctx, stmt_ptr);
                }
            } else {
                /* Process body */
                loop_simp_stmt(ctx, &stmt->data.do_stmt.body);
            }
            break;
            
        case AST_FOR_STMT:
            /* for(init; 0; incr) { body } -> init (condition always false) */
            if (stmt->data.for_stmt.cond && is_const_zero(stmt->data.for_stmt.cond)) {
                /* Replace with just the init if present, otherwise null */
                if (stmt->data.for_stmt.init) {
                    *stmt_ptr = stmt->data.for_stmt.init;
                    ctx->changes++;
                    changed = true;
                } else {
                    stmt->kind = AST_NULL_STMT;
                    ctx->changes++;
                    changed = true;
                }
            } else {
                /* Process init and body */
                if (stmt->data.for_stmt.init) {
                    loop_simp_stmt(ctx, &stmt->data.for_stmt.init);
                }
                loop_simp_stmt(ctx, &stmt->data.for_stmt.body);
            }
            break;
            
        case AST_IF_STMT:
            loop_simp_stmt(ctx, &stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                loop_simp_stmt(ctx, &stmt->data.if_stmt.else_stmt);
            }
            break;
            
        case AST_SWITCH_STMT:
            loop_simp_stmt(ctx, &stmt->data.switch_stmt.body);
            break;
            
        default:
            break;
    }
    
    return changed;
}

int opt_pass_loop_simp(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    loop_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                loop_simp_stmt(&ctx, &decl->data.func_decl.body);
            }
        }
    }
    
    return ctx.changes;
}

/* ============================================================
 * Loop-Invariant Code Motion (O2)
 * 
 * Moves computations that don't change within a loop to outside
 * the loop. This is complex at AST level and requires:
 * - Identifying loop-invariant expressions
 * - Creating temporary variables
 * - Moving code before the loop
 * 
 * For now, this is a stub - full implementation would require
 * significant AST manipulation capabilities.
 * ============================================================ */

int opt_pass_licm(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    /* LICM is complex at AST level - would need to:
     * 1. Identify loop-invariant expressions
     * 2. Create temporary variables to hold results
     * 3. Move computations before the loop
     * 
     * This is better done at IR level where we have SSA form.
     */
    (void)opt;
    (void)ast;
    return 0;
}

/* ============================================================
 * Loop Unrolling (O3)
 * 
 * Unrolls loops with known iteration counts to reduce loop overhead.
 * 
 * Patterns:
 * - for (i = 0; i < N; i++) { body } where N is small constant
 * - while loops with known bounds
 * 
 * Benefits:
 * - Reduces loop overhead (compare, branch)
 * - Enables more optimizations on unrolled body
 * - Better instruction cache utilization for small loops
 * 
 * Limitations:
 * - Only unrolls loops with constant bounds
 * - Maximum unroll factor to avoid code bloat
 * ============================================================ */

#define MAX_UNROLL_ITERATIONS 8     /* Maximum iterations to fully unroll */
#define MAX_UNROLL_BODY_STMTS 10    /* Maximum body statements for unrolling */

typedef struct {
    mcc_ast_opt_t *opt;
    int changes;
} unroll_ctx_t;

/* Check if expression is a simple loop variable comparison */
static bool is_simple_loop_cond(mcc_ast_node_t *cond, const char **var_name, int64_t *limit)
{
    if (!cond || cond->kind != AST_BINARY_EXPR) return false;
    
    mcc_binop_t op = cond->data.binary_expr.op;
    mcc_ast_node_t *lhs = cond->data.binary_expr.lhs;
    mcc_ast_node_t *rhs = cond->data.binary_expr.rhs;
    
    /* Check for i < N or i <= N */
    if (op != BINOP_LT && op != BINOP_LE) return false;
    
    /* LHS should be identifier */
    if (!lhs || lhs->kind != AST_IDENT_EXPR) return false;
    
    /* RHS should be constant */
    if (!rhs) return false;
    int64_t val;
    if (!opt_eval_const_int(rhs, &val)) return false;
    
    *var_name = lhs->data.ident_expr.name;
    *limit = (op == BINOP_LE) ? val + 1 : val;
    
    return true;
}

/* Check if init is i = 0 */
static bool is_zero_init(mcc_ast_node_t *init, const char *var_name)
{
    if (!init) return false;
    
    /* Could be VAR_DECL or EXPR_STMT with assignment */
    if (init->kind == AST_VAR_DECL) {
        if (!init->data.var_decl.name || !var_name) return false;
        if (strcmp(init->data.var_decl.name, var_name) != 0) return false;
        
        mcc_ast_node_t *init_expr = init->data.var_decl.init;
        if (!init_expr) return false;
        
        int64_t val;
        if (opt_eval_const_int(init_expr, &val) && val == 0) {
            return true;
        }
    } else if (init->kind == AST_EXPR_STMT) {
        mcc_ast_node_t *expr = init->data.expr_stmt.expr;
        if (!expr || expr->kind != AST_BINARY_EXPR) return false;
        if (expr->data.binary_expr.op != BINOP_ASSIGN) return false;
        
        mcc_ast_node_t *lhs = expr->data.binary_expr.lhs;
        mcc_ast_node_t *rhs = expr->data.binary_expr.rhs;
        
        if (!lhs || lhs->kind != AST_IDENT_EXPR) return false;
        if (!lhs->data.ident_expr.name || !var_name) return false;
        if (strcmp(lhs->data.ident_expr.name, var_name) != 0) return false;
        
        int64_t val;
        if (opt_eval_const_int(rhs, &val) && val == 0) {
            return true;
        }
    }
    
    return false;
}

/* Check if incr is i++ or i += 1 */
static bool is_unit_increment(mcc_ast_node_t *incr, const char *var_name)
{
    if (!incr) return false;
    
    /* Check for i++ */
    if (incr->kind == AST_UNARY_EXPR) {
        if (incr->data.unary_expr.op != UNOP_POST_INC &&
            incr->data.unary_expr.op != UNOP_PRE_INC) {
            return false;
        }
        mcc_ast_node_t *operand = incr->data.unary_expr.operand;
        if (!operand || operand->kind != AST_IDENT_EXPR) return false;
        if (!operand->data.ident_expr.name || !var_name) return false;
        return strcmp(operand->data.ident_expr.name, var_name) == 0;
    }
    
    /* Check for i += 1 */
    if (incr->kind == AST_BINARY_EXPR) {
        if (incr->data.binary_expr.op != BINOP_ADD_ASSIGN) return false;
        
        mcc_ast_node_t *lhs = incr->data.binary_expr.lhs;
        mcc_ast_node_t *rhs = incr->data.binary_expr.rhs;
        
        if (!lhs || lhs->kind != AST_IDENT_EXPR) return false;
        if (!lhs->data.ident_expr.name || !var_name) return false;
        if (strcmp(lhs->data.ident_expr.name, var_name) != 0) return false;
        
        int64_t val;
        if (opt_eval_const_int(rhs, &val) && val == 1) {
            return true;
        }
    }
    
    return false;
}

/* Count statements in loop body */
static int count_body_stmts(mcc_ast_node_t *body)
{
    if (!body) return 0;
    
    if (body->kind == AST_COMPOUND_STMT) {
        int count = 0;
        for (size_t i = 0; i < body->data.compound_stmt.num_stmts; i++) {
            count += count_body_stmts(body->data.compound_stmt.stmts[i]);
        }
        return count;
    }
    
    return 1;
}

/* Check if loop body uses break/continue */
static bool has_break_continue(mcc_ast_node_t *node)
{
    if (!node) return false;
    
    switch (node->kind) {
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            return true;
            
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < node->data.compound_stmt.num_stmts; i++) {
                if (has_break_continue(node->data.compound_stmt.stmts[i])) {
                    return true;
                }
            }
            break;
            
        case AST_IF_STMT:
            return has_break_continue(node->data.if_stmt.then_stmt) ||
                   has_break_continue(node->data.if_stmt.else_stmt);
            
        /* Don't recurse into nested loops - break/continue there is OK */
        case AST_WHILE_STMT:
        case AST_DO_STMT:
        case AST_FOR_STMT:
            return false;
            
        default:
            break;
    }
    
    return false;
}

/* Process for loops for unrolling */
static bool unroll_for_loop(unroll_ctx_t *ctx, mcc_ast_node_t **stmt_ptr)
{
    mcc_ast_node_t *stmt = *stmt_ptr;
    if (!stmt || stmt->kind != AST_FOR_STMT) return false;
    
    mcc_ast_node_t *init = stmt->data.for_stmt.init;
    mcc_ast_node_t *cond = stmt->data.for_stmt.cond;
    mcc_ast_node_t *incr = stmt->data.for_stmt.incr;
    mcc_ast_node_t *body = stmt->data.for_stmt.body;
    
    /* Check if this is a simple countable loop */
    const char *var_name = NULL;
    int64_t limit = 0;
    
    if (!is_simple_loop_cond(cond, &var_name, &limit)) return false;
    if (!is_zero_init(init, var_name)) return false;
    if (!is_unit_increment(incr, var_name)) return false;
    
    /* Check iteration count */
    if (limit <= 0 || limit > MAX_UNROLL_ITERATIONS) return false;
    
    /* Check body size */
    int body_stmts = count_body_stmts(body);
    if (body_stmts > MAX_UNROLL_BODY_STMTS) return false;
    
    /* Check for break/continue */
    if (has_break_continue(body)) return false;
    
    /* This loop is a candidate for unrolling!
     * 
     * Full unrolling would require:
     * 1. Clone the body 'limit' times
     * 2. Replace loop variable references with constants
     * 3. Replace the for loop with the unrolled statements
     * 
     * This is complex at AST level. For now, we just detect
     * unrollable loops. The actual unrolling could be done
     * at IR level more easily.
     */
    
    ctx->changes++;
    return true;
}

/* Process statements for loop unrolling */
static void unroll_stmt(unroll_ctx_t *ctx, mcc_ast_node_t **stmt_ptr)
{
    if (!stmt_ptr || !*stmt_ptr) return;
    
    mcc_ast_node_t *stmt = *stmt_ptr;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                unroll_stmt(ctx, &stmt->data.compound_stmt.stmts[i]);
            }
            break;
            
        case AST_FOR_STMT:
            /* Try to unroll this loop */
            if (!unroll_for_loop(ctx, stmt_ptr)) {
                /* If not unrollable, process body */
                unroll_stmt(ctx, &stmt->data.for_stmt.body);
            }
            break;
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            unroll_stmt(ctx, &stmt->data.while_stmt.body);
            break;
            
        case AST_IF_STMT:
            unroll_stmt(ctx, &stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                unroll_stmt(ctx, &stmt->data.if_stmt.else_stmt);
            }
            break;
            
        case AST_SWITCH_STMT:
            unroll_stmt(ctx, &stmt->data.switch_stmt.body);
            break;
            
        default:
            break;
    }
}

int opt_pass_loop_unroll(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    unroll_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                unroll_stmt(&ctx, &decl->data.func_decl.body);
            }
        }
    }
    
    /* Note: This pass currently only detects unrollable loops.
     * Full unrolling would require AST cloning and variable substitution.
     * Return 0 since we don't actually transform the AST yet. */
    return 0;
}
