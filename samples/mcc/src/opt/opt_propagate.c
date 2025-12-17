/*
 * MCC - Micro C Compiler
 * Propagation Optimization Passes
 * 
 * Implements constant propagation and copy propagation.
 * Uses symbol pointers for unique identification (handles shadowing correctly).
 */

#include "opt_internal.h"

/* ============================================================
 * Value Tracking for Propagation
 * 
 * We track known values of variables by their symbol pointer,
 * which is unique even for variables with the same name in
 * different scopes (shadowing).
 * ============================================================ */

#define MAX_TRACKED_VARS 64

typedef enum {
    VAL_UNKNOWN,        /* Value is unknown */
    VAL_CONST_INT,      /* Known integer constant */
    VAL_COPY            /* Copy of another variable */
} tracked_val_kind_t;

typedef struct {
    void *sym;                  /* Symbol pointer (unique identifier) */
    tracked_val_kind_t kind;    /* What we know about it */
    union {
        int64_t int_val;        /* For VAL_CONST_INT */
        void *copy_of;          /* For VAL_COPY - symbol of source variable */
    } data;
} tracked_var_t;

typedef struct {
    tracked_var_t vars[MAX_TRACKED_VARS];
    int num_vars;
    mcc_ast_opt_t *opt;
    int changes;
} propagate_ctx_t;

/* Find a tracked variable by symbol pointer */
static tracked_var_t *find_var(propagate_ctx_t *ctx, void *sym)
{
    if (!sym) return NULL;
    for (int i = 0; i < ctx->num_vars; i++) {
        if (ctx->vars[i].sym == sym) {
            return &ctx->vars[i];
        }
    }
    return NULL;
}

/* Add or update a tracked variable */
static tracked_var_t *get_or_add_var(propagate_ctx_t *ctx, void *sym)
{
    if (!sym) return NULL;
    
    tracked_var_t *var = find_var(ctx, sym);
    if (var) return var;
    
    if (ctx->num_vars >= MAX_TRACKED_VARS) return NULL;
    
    var = &ctx->vars[ctx->num_vars++];
    var->sym = sym;
    var->kind = VAL_UNKNOWN;
    return var;
}

/* Set a variable to a known constant value */
static void set_const_int(propagate_ctx_t *ctx, void *sym, int64_t value)
{
    tracked_var_t *var = get_or_add_var(ctx, sym);
    if (var) {
        var->kind = VAL_CONST_INT;
        var->data.int_val = value;
    }
}

/* Set a variable as a copy of another */
static void set_copy(propagate_ctx_t *ctx, void *sym, void *copy_of)
{
    tracked_var_t *var = get_or_add_var(ctx, sym);
    if (var) {
        var->kind = VAL_COPY;
        var->data.copy_of = copy_of;
    }
}

/* Invalidate a variable */
static void invalidate(propagate_ctx_t *ctx, void *sym)
{
    if (!sym) return;
    
    tracked_var_t *var = find_var(ctx, sym);
    if (var) {
        var->kind = VAL_UNKNOWN;
    }
    
    /* Also invalidate any variables that are copies of this one */
    for (int i = 0; i < ctx->num_vars; i++) {
        if (ctx->vars[i].kind == VAL_COPY && ctx->vars[i].data.copy_of == sym) {
            ctx->vars[i].kind = VAL_UNKNOWN;
        }
    }
}

/* Invalidate all variables */
static void invalidate_all(propagate_ctx_t *ctx)
{
    for (int i = 0; i < ctx->num_vars; i++) {
        ctx->vars[i].kind = VAL_UNKNOWN;
    }
}

/* Get the symbol from an identifier expression */
static void *get_ident_sym(mcc_ast_node_t *expr)
{
    if (!expr || expr->kind != AST_IDENT_EXPR) return NULL;
    return expr->data.ident_expr.symbol;
}

/* ============================================================
 * Constant Propagation (O1)
 * 
 * Replaces variable references with their known constant values.
 * Example:
 *   int x = 5;
 *   int y = x + 3;  // becomes: int y = 5 + 3;
 * ============================================================ */

/* Process an expression for constant propagation - returns true if modified */
static bool const_prop_expr(propagate_ctx_t *ctx, mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    bool modified = false;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR: {
            void *sym = get_ident_sym(expr);
            if (!sym) break;
            
            tracked_var_t *var = find_var(ctx, sym);
            if (var && var->kind == VAL_CONST_INT) {
                /* Replace identifier with constant */
                expr->kind = AST_INT_LIT;
                expr->data.int_lit.value = (uint64_t)var->data.int_val;
                expr->data.int_lit.suffix = INT_SUFFIX_NONE;
                ctx->changes++;
                modified = true;
            }
            break;
        }
        
        case AST_BINARY_EXPR:
            modified |= const_prop_expr(ctx, expr->data.binary_expr.lhs);
            modified |= const_prop_expr(ctx, expr->data.binary_expr.rhs);
            break;
            
        case AST_UNARY_EXPR:
            /* Don't propagate into address-of operations */
            if (expr->data.unary_expr.op != UNOP_ADDR) {
                modified |= const_prop_expr(ctx, expr->data.unary_expr.operand);
            }
            break;
            
        case AST_CALL_EXPR:
            modified |= const_prop_expr(ctx, expr->data.call_expr.func);
            for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
                modified |= const_prop_expr(ctx, expr->data.call_expr.args[i]);
            }
            break;
            
        case AST_TERNARY_EXPR:
            modified |= const_prop_expr(ctx, expr->data.ternary_expr.cond);
            modified |= const_prop_expr(ctx, expr->data.ternary_expr.then_expr);
            modified |= const_prop_expr(ctx, expr->data.ternary_expr.else_expr);
            break;
            
        case AST_SUBSCRIPT_EXPR:
            modified |= const_prop_expr(ctx, expr->data.subscript_expr.array);
            modified |= const_prop_expr(ctx, expr->data.subscript_expr.index);
            break;
            
        case AST_MEMBER_EXPR:
            modified |= const_prop_expr(ctx, expr->data.member_expr.object);
            break;
            
        case AST_CAST_EXPR:
            modified |= const_prop_expr(ctx, expr->data.cast_expr.expr);
            break;
            
        case AST_COMMA_EXPR:
            modified |= const_prop_expr(ctx, expr->data.comma_expr.left);
            modified |= const_prop_expr(ctx, expr->data.comma_expr.right);
            break;
            
        default:
            break;
    }
    
    return modified;
}

/* Process a statement for constant propagation */
static void const_prop_stmt(propagate_ctx_t *ctx, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                const_prop_stmt(ctx, stmt->data.compound_stmt.stmts[i]);
            }
            break;
            
        case AST_VAR_DECL: {
            /* For VAR_DECL, we need to find the symbol from a later use.
             * Skip tracking here - we'll track when we see assignments. */
            mcc_ast_node_t *init = stmt->data.var_decl.init;
            if (init) {
                /* Propagate constants into the initializer */
                const_prop_expr(ctx, init);
            }
            /* Note: We can't track VAR_DECL directly because it doesn't
             * have a symbol pointer. The symbol is only in IDENT_EXPR. */
            break;
        }
        
        case AST_EXPR_STMT: {
            mcc_ast_node_t *expr = stmt->data.expr_stmt.expr;
            if (!expr) break;
            
            /* Check for assignment */
            if (expr->kind == AST_BINARY_EXPR && expr->data.binary_expr.op == BINOP_ASSIGN) {
                mcc_ast_node_t *lhs = expr->data.binary_expr.lhs;
                mcc_ast_node_t *rhs = expr->data.binary_expr.rhs;
                
                /* Propagate into RHS first */
                const_prop_expr(ctx, rhs);
                
                /* Track the assignment */
                void *sym = get_ident_sym(lhs);
                if (sym) {
                    int64_t val;
                    if (opt_eval_const_int(rhs, &val)) {
                        set_const_int(ctx, sym, val);
                    } else {
                        invalidate(ctx, sym);
                    }
                }
            } else {
                const_prop_expr(ctx, expr);
            }
            break;
        }
        
        case AST_IF_STMT:
            const_prop_expr(ctx, stmt->data.if_stmt.cond);
            const_prop_stmt(ctx, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                const_prop_stmt(ctx, stmt->data.if_stmt.else_stmt);
            }
            /* After if, we don't know which branch was taken */
            invalidate_all(ctx);
            break;
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            /* Loops are tricky - invalidate everything */
            invalidate_all(ctx);
            const_prop_expr(ctx, stmt->data.while_stmt.cond);
            const_prop_stmt(ctx, stmt->data.while_stmt.body);
            invalidate_all(ctx);
            break;
            
        case AST_FOR_STMT:
            invalidate_all(ctx);
            if (stmt->data.for_stmt.init) {
                const_prop_stmt(ctx, stmt->data.for_stmt.init);
            }
            if (stmt->data.for_stmt.cond) {
                const_prop_expr(ctx, stmt->data.for_stmt.cond);
            }
            const_prop_stmt(ctx, stmt->data.for_stmt.body);
            if (stmt->data.for_stmt.incr) {
                const_prop_expr(ctx, stmt->data.for_stmt.incr);
            }
            invalidate_all(ctx);
            break;
            
        case AST_RETURN_STMT:
            if (stmt->data.return_stmt.expr) {
                const_prop_expr(ctx, stmt->data.return_stmt.expr);
            }
            break;
            
        case AST_SWITCH_STMT:
            const_prop_expr(ctx, stmt->data.switch_stmt.expr);
            const_prop_stmt(ctx, stmt->data.switch_stmt.body);
            invalidate_all(ctx);
            break;
            
        default:
            break;
    }
}

int opt_pass_const_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    propagate_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                /* Reset tracking for each function */
                ctx.num_vars = 0;
                const_prop_stmt(&ctx, decl->data.func_decl.body);
            }
        }
    }
    
    return ctx.changes;
}

/* ============================================================
 * Copy Propagation (Og)
 * 
 * Replaces variable references with the variable they were
 * copied from.
 * Example:
 *   int x = a;
 *   int y = x + 1;  // becomes: int y = a + 1;
 * ============================================================ */

/* Process an expression for copy propagation - returns true if modified */
static bool copy_prop_expr(propagate_ctx_t *ctx, mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    bool modified = false;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR: {
            void *sym = get_ident_sym(expr);
            if (!sym) break;
            
            tracked_var_t *var = find_var(ctx, sym);
            if (var && var->kind == VAL_COPY && var->data.copy_of) {
                /* Replace with the original variable's symbol */
                mcc_symbol_t *orig_sym = (mcc_symbol_t *)var->data.copy_of;
                expr->data.ident_expr.name = orig_sym->name;
                expr->data.ident_expr.symbol = orig_sym;
                ctx->changes++;
                modified = true;
            }
            break;
        }
        
        case AST_BINARY_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.binary_expr.lhs);
            modified |= copy_prop_expr(ctx, expr->data.binary_expr.rhs);
            break;
            
        case AST_UNARY_EXPR:
            if (expr->data.unary_expr.op != UNOP_ADDR) {
                modified |= copy_prop_expr(ctx, expr->data.unary_expr.operand);
            }
            break;
            
        case AST_CALL_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.call_expr.func);
            for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
                modified |= copy_prop_expr(ctx, expr->data.call_expr.args[i]);
            }
            break;
            
        case AST_TERNARY_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.ternary_expr.cond);
            modified |= copy_prop_expr(ctx, expr->data.ternary_expr.then_expr);
            modified |= copy_prop_expr(ctx, expr->data.ternary_expr.else_expr);
            break;
            
        case AST_SUBSCRIPT_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.subscript_expr.array);
            modified |= copy_prop_expr(ctx, expr->data.subscript_expr.index);
            break;
            
        case AST_MEMBER_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.member_expr.object);
            break;
            
        case AST_CAST_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.cast_expr.expr);
            break;
            
        case AST_COMMA_EXPR:
            modified |= copy_prop_expr(ctx, expr->data.comma_expr.left);
            modified |= copy_prop_expr(ctx, expr->data.comma_expr.right);
            break;
            
        default:
            break;
    }
    
    return modified;
}

/* Process a statement for copy propagation */
static void copy_prop_stmt(propagate_ctx_t *ctx, mcc_ast_node_t *stmt)
{
    if (!stmt) return;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                copy_prop_stmt(ctx, stmt->data.compound_stmt.stmts[i]);
            }
            break;
            
        case AST_VAR_DECL: {
            /* For VAR_DECL, we can't track directly - no symbol pointer */
            mcc_ast_node_t *init = stmt->data.var_decl.init;
            if (init) {
                copy_prop_expr(ctx, init);
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            mcc_ast_node_t *expr = stmt->data.expr_stmt.expr;
            if (!expr) break;
            
            if (expr->kind == AST_BINARY_EXPR && expr->data.binary_expr.op == BINOP_ASSIGN) {
                mcc_ast_node_t *lhs = expr->data.binary_expr.lhs;
                mcc_ast_node_t *rhs = expr->data.binary_expr.rhs;
                
                /* Propagate into RHS first */
                copy_prop_expr(ctx, rhs);
                
                /* Track the assignment */
                void *sym = get_ident_sym(lhs);
                if (sym) {
                    void *src = get_ident_sym(rhs);
                    if (src) {
                        set_copy(ctx, sym, src);
                    } else {
                        invalidate(ctx, sym);
                    }
                }
            } else {
                copy_prop_expr(ctx, expr);
            }
            break;
        }
        
        case AST_IF_STMT:
            copy_prop_expr(ctx, stmt->data.if_stmt.cond);
            copy_prop_stmt(ctx, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                copy_prop_stmt(ctx, stmt->data.if_stmt.else_stmt);
            }
            invalidate_all(ctx);
            break;
            
        case AST_WHILE_STMT:
        case AST_DO_STMT:
            invalidate_all(ctx);
            copy_prop_expr(ctx, stmt->data.while_stmt.cond);
            copy_prop_stmt(ctx, stmt->data.while_stmt.body);
            invalidate_all(ctx);
            break;
            
        case AST_FOR_STMT:
            invalidate_all(ctx);
            if (stmt->data.for_stmt.init) {
                copy_prop_stmt(ctx, stmt->data.for_stmt.init);
            }
            if (stmt->data.for_stmt.cond) {
                copy_prop_expr(ctx, stmt->data.for_stmt.cond);
            }
            copy_prop_stmt(ctx, stmt->data.for_stmt.body);
            if (stmt->data.for_stmt.incr) {
                copy_prop_expr(ctx, stmt->data.for_stmt.incr);
            }
            invalidate_all(ctx);
            break;
            
        case AST_RETURN_STMT:
            if (stmt->data.return_stmt.expr) {
                copy_prop_expr(ctx, stmt->data.return_stmt.expr);
            }
            break;
            
        case AST_SWITCH_STMT:
            copy_prop_expr(ctx, stmt->data.switch_stmt.expr);
            copy_prop_stmt(ctx, stmt->data.switch_stmt.body);
            invalidate_all(ctx);
            break;
            
        default:
            break;
    }
}

int opt_pass_copy_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    propagate_ctx_t ctx = {0};
    ctx.opt = opt;
    ctx.changes = 0;
    
    /* Process each function */
    if (ast->kind == AST_TRANSLATION_UNIT) {
        for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
            mcc_ast_node_t *decl = ast->data.translation_unit.decls[i];
            if (decl->kind == AST_FUNC_DECL && decl->data.func_decl.body) {
                ctx.num_vars = 0;
                copy_prop_stmt(&ctx, decl->data.func_decl.body);
            }
        }
    }
    
    return ctx.changes;
}

/* ============================================================
 * Store-Load Propagation (Og)
 * 
 * This is largely covered by const_prop and copy_prop.
 * ============================================================ */

int opt_pass_store_load_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    (void)opt;
    (void)ast;
    return 0;
}
