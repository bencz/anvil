/*
 * MCC - Micro C Compiler
 * Common Subexpression Elimination (CSE)
 * 
 * Identifies and eliminates redundant computations within basic blocks.
 * When the same expression is computed multiple times, we can reuse
 * the result of the first computation.
 */

#include "opt_internal.h"

/* ============================================================
 * CSE Implementation
 * 
 * This is a local CSE that works within basic blocks (straight-line code).
 * It tracks expressions and replaces duplicates with references to
 * previously computed values.
 * 
 * Limitations:
 * - Only works within basic blocks (invalidates at control flow)
 * - Does not create temporary variables (would require AST modification)
 * - Only handles expressions without side effects
 * ============================================================ */

#define MAX_CSE_EXPRS 32

typedef struct {
    mcc_ast_node_t *expr;       /* The expression */
    mcc_ast_node_t *first_use;  /* First occurrence of this expression */
} cse_entry_t;

typedef struct {
    cse_entry_t entries[MAX_CSE_EXPRS];
    int num_entries;
    mcc_ast_opt_t *opt;
    int changes;
} cse_ctx_t;

/* Forward declarations */
static bool cse_exprs_equal(mcc_ast_node_t *a, mcc_ast_node_t *b);
static bool is_cse_candidate(mcc_ast_node_t *expr);
static void cse_scan_expr(cse_ctx_t *ctx, mcc_ast_node_t **expr_ptr);
static void cse_process_stmt(cse_ctx_t *ctx, mcc_ast_node_t *stmt);

/* Check if two expressions are structurally equivalent */
static bool cse_exprs_equal(mcc_ast_node_t *a, mcc_ast_node_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case AST_INT_LIT:
            return a->data.int_lit.value == b->data.int_lit.value;
            
        case AST_FLOAT_LIT:
            return a->data.float_lit.value == b->data.float_lit.value;
            
        case AST_IDENT_EXPR:
            /* Compare by symbol pointer (unique per variable) */
            if (a->data.ident_expr.symbol && b->data.ident_expr.symbol) {
                return a->data.ident_expr.symbol == b->data.ident_expr.symbol;
            }
            /* Fallback to name comparison */
            if (a->data.ident_expr.name && b->data.ident_expr.name) {
                return strcmp(a->data.ident_expr.name, b->data.ident_expr.name) == 0;
            }
            return false;
            
        case AST_BINARY_EXPR:
            if (a->data.binary_expr.op != b->data.binary_expr.op) return false;
            return cse_exprs_equal(a->data.binary_expr.lhs, b->data.binary_expr.lhs) &&
                   cse_exprs_equal(a->data.binary_expr.rhs, b->data.binary_expr.rhs);
            
        case AST_UNARY_EXPR:
            if (a->data.unary_expr.op != b->data.unary_expr.op) return false;
            return cse_exprs_equal(a->data.unary_expr.operand, b->data.unary_expr.operand);
            
        case AST_SUBSCRIPT_EXPR:
            return cse_exprs_equal(a->data.subscript_expr.array, b->data.subscript_expr.array) &&
                   cse_exprs_equal(a->data.subscript_expr.index, b->data.subscript_expr.index);
            
        case AST_MEMBER_EXPR:
            if (a->data.member_expr.is_arrow != b->data.member_expr.is_arrow) return false;
            if (!a->data.member_expr.member || !b->data.member_expr.member) return false;
            if (strcmp(a->data.member_expr.member, b->data.member_expr.member) != 0) return false;
            return cse_exprs_equal(a->data.member_expr.object, b->data.member_expr.object);
            
        case AST_CAST_EXPR:
            /* For casts, we'd need to compare types too - skip for now */
            return false;
            
        default:
            return false;
    }
}

/* Check if expression is suitable for CSE */
static bool is_cse_candidate(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        case AST_BINARY_EXPR:
            /* Skip assignments and compound assignments */
            if (expr->data.binary_expr.op >= BINOP_ASSIGN) return false;
            /* Must have no side effects */
            return !opt_has_side_effects(expr);
            
        case AST_UNARY_EXPR:
            /* Skip increment/decrement */
            switch (expr->data.unary_expr.op) {
                case UNOP_PRE_INC:
                case UNOP_PRE_DEC:
                case UNOP_POST_INC:
                case UNOP_POST_DEC:
                    return false;
                default:
                    return !opt_has_side_effects(expr);
            }
            
        case AST_SUBSCRIPT_EXPR:
        case AST_MEMBER_EXPR:
            return !opt_has_side_effects(expr);
            
        default:
            return false;
    }
}

/* Check if expression is "complex enough" to be worth CSE */
static bool is_worth_cse(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        case AST_BINARY_EXPR:
            /* Arithmetic operations are worth CSE */
            switch (expr->data.binary_expr.op) {
                case BINOP_MUL:
                case BINOP_DIV:
                case BINOP_MOD:
                    return true;  /* These are expensive */
                case BINOP_ADD:
                case BINOP_SUB:
                    /* Only if operands are complex */
                    return is_worth_cse(expr->data.binary_expr.lhs) ||
                           is_worth_cse(expr->data.binary_expr.rhs);
                default:
                    return false;
            }
            
        case AST_SUBSCRIPT_EXPR:
            /* Array access with complex index */
            return expr->data.subscript_expr.index->kind != AST_INT_LIT;
            
        case AST_MEMBER_EXPR:
            /* Member access through pointer */
            return expr->data.member_expr.is_arrow;
            
        default:
            return false;
    }
}

/* Find a matching expression in the CSE table */
static cse_entry_t *find_cse_entry(cse_ctx_t *ctx, mcc_ast_node_t *expr)
{
    for (int i = 0; i < ctx->num_entries; i++) {
        if (cse_exprs_equal(ctx->entries[i].expr, expr)) {
            return &ctx->entries[i];
        }
    }
    return NULL;
}

/* Add an expression to the CSE table */
static void add_cse_entry(cse_ctx_t *ctx, mcc_ast_node_t *expr)
{
    if (ctx->num_entries >= MAX_CSE_EXPRS) return;
    if (!is_cse_candidate(expr)) return;
    if (!is_worth_cse(expr)) return;
    
    cse_entry_t *entry = &ctx->entries[ctx->num_entries++];
    entry->expr = expr;
    entry->first_use = expr;
}

/* Invalidate entries that use a given variable */
static void invalidate_var(cse_ctx_t *ctx, void *sym)
{
    if (!sym) return;
    
    /* Remove all entries - conservative but safe */
    /* A more sophisticated implementation would only remove entries
     * that actually use the modified variable */
    ctx->num_entries = 0;
}

/* Invalidate all entries (e.g., after function call) */
static void invalidate_all(cse_ctx_t *ctx)
{
    ctx->num_entries = 0;
}

/* Get symbol from identifier expression */
static void *get_cse_ident_sym(mcc_ast_node_t *expr)
{
    if (!expr || expr->kind != AST_IDENT_EXPR) return NULL;
    return expr->data.ident_expr.symbol;
}

/* Scan and process an expression for CSE */
static void cse_scan_expr(cse_ctx_t *ctx, mcc_ast_node_t **expr_ptr)
{
    if (!expr_ptr || !*expr_ptr) return;
    
    mcc_ast_node_t *expr = *expr_ptr;
    
    switch (expr->kind) {
        case AST_BINARY_EXPR:
            /* Process children first (bottom-up) */
            cse_scan_expr(ctx, &expr->data.binary_expr.lhs);
            cse_scan_expr(ctx, &expr->data.binary_expr.rhs);
            
            /* Check if this expression is in the CSE table */
            if (is_cse_candidate(expr)) {
                cse_entry_t *found = find_cse_entry(ctx, expr);
                if (found && found->first_use != expr) {
                    /* Found a common subexpression! */
                    /* Note: We can't easily replace it without creating temps */
                    /* For now, just count it for statistics */
                    ctx->changes++;
                } else if (!found) {
                    /* Add to table for future matches */
                    add_cse_entry(ctx, expr);
                }
            }
            break;
            
        case AST_UNARY_EXPR:
            cse_scan_expr(ctx, &expr->data.unary_expr.operand);
            if (is_cse_candidate(expr)) {
                cse_entry_t *found = find_cse_entry(ctx, expr);
                if (found && found->first_use != expr) {
                    ctx->changes++;
                } else if (!found) {
                    add_cse_entry(ctx, expr);
                }
            }
            break;
            
        case AST_CALL_EXPR:
            /* Function calls invalidate all CSE entries */
            invalidate_all(ctx);
            /* Still scan arguments */
            cse_scan_expr(ctx, &expr->data.call_expr.func);
            for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
                cse_scan_expr(ctx, &expr->data.call_expr.args[i]);
            }
            break;
            
        case AST_TERNARY_EXPR:
            cse_scan_expr(ctx, &expr->data.ternary_expr.cond);
            /* Branches invalidate CSE */
            invalidate_all(ctx);
            cse_scan_expr(ctx, &expr->data.ternary_expr.then_expr);
            invalidate_all(ctx);
            cse_scan_expr(ctx, &expr->data.ternary_expr.else_expr);
            break;
            
        case AST_SUBSCRIPT_EXPR:
            cse_scan_expr(ctx, &expr->data.subscript_expr.array);
            cse_scan_expr(ctx, &expr->data.subscript_expr.index);
            if (is_cse_candidate(expr)) {
                cse_entry_t *found = find_cse_entry(ctx, expr);
                if (found && found->first_use != expr) {
                    ctx->changes++;
                } else if (!found) {
                    add_cse_entry(ctx, expr);
                }
            }
            break;
            
        case AST_MEMBER_EXPR:
            cse_scan_expr(ctx, &expr->data.member_expr.object);
            if (is_cse_candidate(expr)) {
                cse_entry_t *found = find_cse_entry(ctx, expr);
                if (found && found->first_use != expr) {
                    ctx->changes++;
                } else if (!found) {
                    add_cse_entry(ctx, expr);
                }
            }
            break;
            
        case AST_CAST_EXPR:
            cse_scan_expr(ctx, &expr->data.cast_expr.expr);
            break;
            
        case AST_COMMA_EXPR:
            cse_scan_expr(ctx, &expr->data.comma_expr.left);
            cse_scan_expr(ctx, &expr->data.comma_expr.right);
            break;
            
        default:
            break;
    }
}

/* Process a statement for CSE */
static void cse_process_stmt(cse_ctx_t *ctx, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                cse_process_stmt(ctx, stmt->data.compound_stmt.stmts[i]);
            }
            break;
            
        case AST_VAR_DECL:
            if (stmt->data.var_decl.init) {
                cse_scan_expr(ctx, &stmt->data.var_decl.init);
            }
            break;
            
        case AST_EXPR_STMT: {
            mcc_ast_node_t *expr = stmt->data.expr_stmt.expr;
            if (!expr) break;
            
            /* Check for assignment */
            if (expr->kind == AST_BINARY_EXPR && expr->data.binary_expr.op >= BINOP_ASSIGN) {
                /* Scan RHS for CSE */
                cse_scan_expr(ctx, &expr->data.binary_expr.rhs);
                /* Invalidate entries using the assigned variable */
                void *sym = get_cse_ident_sym(expr->data.binary_expr.lhs);
                if (sym) {
                    invalidate_var(ctx, sym);
                }
            } else {
                cse_scan_expr(ctx, &stmt->data.expr_stmt.expr);
            }
            break;
        }
        
        case AST_IF_STMT:
            cse_scan_expr(ctx, &stmt->data.if_stmt.cond);
            /* Branches invalidate CSE */
            invalidate_all(ctx);
            cse_process_stmt(ctx, stmt->data.if_stmt.then_stmt);
            invalidate_all(ctx);
            if (stmt->data.if_stmt.else_stmt) {
                cse_process_stmt(ctx, stmt->data.if_stmt.else_stmt);
            }
            invalidate_all(ctx);
            break;
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            invalidate_all(ctx);
            cse_scan_expr(ctx, &stmt->data.while_stmt.cond);
            cse_process_stmt(ctx, stmt->data.while_stmt.body);
            invalidate_all(ctx);
            break;
            
        case AST_FOR_STMT:
            invalidate_all(ctx);
            if (stmt->data.for_stmt.init) {
                cse_process_stmt(ctx, stmt->data.for_stmt.init);
            }
            if (stmt->data.for_stmt.cond) {
                cse_scan_expr(ctx, &stmt->data.for_stmt.cond);
            }
            cse_process_stmt(ctx, stmt->data.for_stmt.body);
            if (stmt->data.for_stmt.incr) {
                cse_scan_expr(ctx, &stmt->data.for_stmt.incr);
            }
            invalidate_all(ctx);
            break;
            
        case AST_RETURN_STMT:
            if (stmt->data.return_stmt.expr) {
                cse_scan_expr(ctx, &stmt->data.return_stmt.expr);
            }
            break;
            
        case AST_SWITCH_STMT:
            cse_scan_expr(ctx, &stmt->data.switch_stmt.expr);
            invalidate_all(ctx);
            cse_process_stmt(ctx, stmt->data.switch_stmt.body);
            invalidate_all(ctx);
            break;
            
        default:
            break;
    }
}

/* Main CSE pass entry point */
int opt_pass_cse(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    cse_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                ctx.num_entries = 0;
                cse_process_stmt(&ctx, decl->data.func_decl.body);
            }
        }
    }
    
    /* Note: This pass currently only detects CSE opportunities.
     * Full CSE would require creating temporary variables to store
     * the result of the first computation, which is complex at AST level.
     * The detection is still useful for analysis and future implementation. */
    return 0;  /* Return 0 since we don't actually transform yet */
}
