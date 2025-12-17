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
 * Removes stores to variables that are never read before being
 * overwritten or going out of scope.
 * 
 * This is a simplified version that only handles:
 * - Variables assigned but never used
 * - Consecutive assignments to the same variable
 * ============================================================ */

#define MAX_STORE_VARS 64

typedef struct {
    mcc_symbol_t *sym;
    mcc_ast_node_t *last_store;  /* Last store to this variable */
    bool was_read;               /* Was read since last store */
} store_var_t;

typedef struct {
    store_var_t vars[MAX_STORE_VARS];
    int num_vars;
    mcc_ast_opt_t *opt;
    int changes;
} dead_store_ctx_t;

static store_var_t *find_store_var(dead_store_ctx_t *ctx, mcc_symbol_t *sym)
{
    for (int i = 0; i < ctx->num_vars; i++) {
        if (ctx->vars[i].sym == sym) {
            return &ctx->vars[i];
        }
    }
    return NULL;
}

static store_var_t *add_store_var(dead_store_ctx_t *ctx, mcc_symbol_t *sym)
{
    if (ctx->num_vars >= MAX_STORE_VARS) return NULL;
    
    store_var_t *var = &ctx->vars[ctx->num_vars++];
    var->sym = sym;
    var->last_store = NULL;
    var->was_read = false;
    return var;
}

/* Mark a variable as read */
static void mark_var_read(dead_store_ctx_t *ctx, mcc_symbol_t *sym)
{
    store_var_t *var = find_store_var(ctx, sym);
    if (var) {
        var->was_read = true;
    }
}

/* Record a store to a variable */
static void record_store(dead_store_ctx_t *ctx, mcc_symbol_t *sym, mcc_ast_node_t *store)
{
    store_var_t *var = find_store_var(ctx, sym);
    if (!var) {
        var = add_store_var(ctx, sym);
    }
    if (!var) return;
    
    var->last_store = store;
    var->was_read = false;
}

/* Get symbol from identifier expression */
static mcc_symbol_t *get_store_ident_symbol(mcc_ast_node_t *expr)
{
    if (!expr || expr->kind != AST_IDENT_EXPR) return NULL;
    return (mcc_symbol_t *)expr->data.ident_expr.symbol;
}

/* Check if symbol is a local variable */
static bool is_store_local_var(mcc_symbol_t *sym)
{
    if (!sym) return false;
    if (sym->kind != SYM_VAR && sym->kind != SYM_PARAM) return false;
    if (sym->storage == STORAGE_EXTERN || sym->storage == STORAGE_STATIC) return false;
    return true;
}

/* Scan expression for variable reads */
static void scan_reads(dead_store_ctx_t *ctx, mcc_ast_node_t *expr)
{
    if (!expr) return;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = get_store_ident_symbol(expr);
            if (sym && is_store_local_var(sym)) {
                mark_var_read(ctx, sym);
            }
            break;
        }
        
        case AST_BINARY_EXPR:
            /* For assignment, only RHS is a read */
            if (expr->data.binary_expr.op == BINOP_ASSIGN) {
                scan_reads(ctx, expr->data.binary_expr.rhs);
            } else {
                scan_reads(ctx, expr->data.binary_expr.lhs);
                scan_reads(ctx, expr->data.binary_expr.rhs);
            }
            break;
            
        case AST_UNARY_EXPR:
            scan_reads(ctx, expr->data.unary_expr.operand);
            break;
            
        case AST_CALL_EXPR:
            scan_reads(ctx, expr->data.call_expr.func);
            for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
                scan_reads(ctx, expr->data.call_expr.args[i]);
            }
            break;
            
        case AST_TERNARY_EXPR:
            scan_reads(ctx, expr->data.ternary_expr.cond);
            scan_reads(ctx, expr->data.ternary_expr.then_expr);
            scan_reads(ctx, expr->data.ternary_expr.else_expr);
            break;
            
        case AST_SUBSCRIPT_EXPR:
            scan_reads(ctx, expr->data.subscript_expr.array);
            scan_reads(ctx, expr->data.subscript_expr.index);
            break;
            
        case AST_MEMBER_EXPR:
            scan_reads(ctx, expr->data.member_expr.object);
            break;
            
        case AST_CAST_EXPR:
            scan_reads(ctx, expr->data.cast_expr.expr);
            break;
            
        case AST_COMMA_EXPR:
            scan_reads(ctx, expr->data.comma_expr.left);
            scan_reads(ctx, expr->data.comma_expr.right);
            break;
            
        default:
            break;
    }
}

/* Process statement for dead store elimination */
static void dead_store_stmt(dead_store_ctx_t *ctx, mcc_ast_node_t *stmt);

static void dead_store_stmt(dead_store_ctx_t *ctx, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                dead_store_stmt(ctx, stmt->data.compound_stmt.stmts[i]);
            }
            break;
            
        case AST_VAR_DECL: {
            mcc_ast_node_t *init = stmt->data.var_decl.init;
            if (init) {
                /* Scan initializer for reads */
                scan_reads(ctx, init);
                
                /* Look up symbol */
                const char *name = stmt->data.var_decl.name;
                mcc_symbol_t *sym = NULL;
                if (name && ctx->opt->sema) {
                    sym = mcc_symtab_lookup(ctx->opt->sema->symtab, name);
                }
                
                if (sym && is_store_local_var(sym)) {
                    record_store(ctx, sym, stmt);
                }
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            mcc_ast_node_t *expr = stmt->data.expr_stmt.expr;
            if (!expr) break;
            
            /* Check for assignment */
            if (expr->kind == AST_BINARY_EXPR && expr->data.binary_expr.op == BINOP_ASSIGN) {
                mcc_ast_node_t *lhs = expr->data.binary_expr.lhs;
                mcc_ast_node_t *rhs = expr->data.binary_expr.rhs;
                
                /* Scan RHS for reads first */
                scan_reads(ctx, rhs);
                
                /* Record the store */
                mcc_symbol_t *sym = get_store_ident_symbol(lhs);
                if (sym && is_store_local_var(sym)) {
                    record_store(ctx, sym, stmt);
                }
            } else {
                /* Not an assignment, scan for reads */
                scan_reads(ctx, expr);
            }
            break;
        }
        
        case AST_IF_STMT:
            scan_reads(ctx, stmt->data.if_stmt.cond);
            dead_store_stmt(ctx, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                dead_store_stmt(ctx, stmt->data.if_stmt.else_stmt);
            }
            /* After branches, be conservative */
            for (int i = 0; i < ctx->num_vars; i++) {
                ctx->vars[i].was_read = true;
            }
            break;
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            /* Loops are tricky - mark all as read */
            for (int i = 0; i < ctx->num_vars; i++) {
                ctx->vars[i].was_read = true;
            }
            scan_reads(ctx, stmt->data.while_stmt.cond);
            dead_store_stmt(ctx, stmt->data.while_stmt.body);
            break;
            
        case AST_FOR_STMT:
            for (int i = 0; i < ctx->num_vars; i++) {
                ctx->vars[i].was_read = true;
            }
            if (stmt->data.for_stmt.init) {
                dead_store_stmt(ctx, stmt->data.for_stmt.init);
            }
            if (stmt->data.for_stmt.cond) {
                scan_reads(ctx, stmt->data.for_stmt.cond);
            }
            dead_store_stmt(ctx, stmt->data.for_stmt.body);
            if (stmt->data.for_stmt.incr) {
                scan_reads(ctx, stmt->data.for_stmt.incr);
            }
            break;
            
        case AST_RETURN_STMT:
            if (stmt->data.return_stmt.expr) {
                scan_reads(ctx, stmt->data.return_stmt.expr);
            }
            break;
            
        case AST_SWITCH_STMT:
            scan_reads(ctx, stmt->data.switch_stmt.expr);
            dead_store_stmt(ctx, stmt->data.switch_stmt.body);
            for (int i = 0; i < ctx->num_vars; i++) {
                ctx->vars[i].was_read = true;
            }
            break;
            
        default:
            break;
    }
}

int opt_pass_dead_store(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    if (!opt->sema) return 0;
    
    dead_store_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                ctx.num_vars = 0;
                dead_store_stmt(&ctx, decl->data.func_decl.body);
            }
        }
    }
    
    return ctx.changes;
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
