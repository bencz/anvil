/*
 * MCC - Micro C Compiler
 * Constant Optimization Passes
 * 
 * Implements constant folding, trivial constant simplification,
 * and identity operation removal.
 */

#include "opt_internal.h"

/* ============================================================
 * Trivial Constant Simplification (O0)
 * 
 * Simplifies expressions with trivial constants that don't
 * require full constant folding:
 * - 1 * x -> x
 * - x * 1 -> x
 * - 0 + x -> x
 * - x + 0 -> x
 * - x - 0 -> x
 * - x / 1 -> x
 * - 0 / x -> 0 (if x != 0)
 * ============================================================ */

static bool is_int_zero(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    if (expr->kind == AST_INT_LIT) {
        return expr->data.int_lit.value == 0;
    }
    return false;
}

static bool is_int_one(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    if (expr->kind == AST_INT_LIT) {
        return expr->data.int_lit.value == 1;
    }
    return false;
}

static bool is_all_ones(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    if (expr->kind == AST_INT_LIT) {
        return expr->data.int_lit.value == (int64_t)-1;
    }
    return false;
}

typedef struct {
    int changes;
} trivial_const_data_t;

static bool trivial_const_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    trivial_const_data_t *tcd = (trivial_const_data_t *)data;
    
    if (node->kind != AST_BINARY_EXPR) return true;
    
    mcc_ast_node_t *lhs = node->data.binary_expr.lhs;
    mcc_ast_node_t *rhs = node->data.binary_expr.rhs;
    mcc_binop_t op = node->data.binary_expr.op;
    
    mcc_ast_node_t *replacement = NULL;
    
    switch (op) {
        case BINOP_ADD:
            /* 0 + x -> x */
            if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = rhs;
            }
            /* x + 0 -> x */
            else if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            break;
            
        case BINOP_SUB:
            /* x - 0 -> x */
            if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            break;
            
        case BINOP_MUL:
            /* 1 * x -> x */
            if (is_int_one(lhs) && !opt_has_side_effects(rhs)) {
                replacement = rhs;
            }
            /* x * 1 -> x */
            else if (is_int_one(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* 0 * x -> 0 (if x has no side effects) */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = lhs;
            }
            /* x * 0 -> 0 (if x has no side effects) */
            else if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = rhs;
            }
            break;
            
        case BINOP_DIV:
            /* x / 1 -> x */
            if (is_int_one(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* 0 / x -> 0 (if x has no side effects and x != 0) */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs) && !is_int_zero(rhs)) {
                replacement = lhs;
            }
            break;
            
        case BINOP_MOD:
            /* x % 1 -> 0 */
            if (is_int_one(rhs) && !opt_has_side_effects(lhs)) {
                replacement = opt_make_int_lit(opt, 0, node->location);
            }
            break;
            
        case BINOP_BIT_OR:
            /* x | 0 -> x */
            if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* 0 | x -> x */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = rhs;
            }
            break;
            
        case BINOP_BIT_AND:
            /* x & ~0 -> x (all ones) */
            if (is_all_ones(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* ~0 & x -> x */
            else if (is_all_ones(lhs) && !opt_has_side_effects(rhs)) {
                replacement = rhs;
            }
            /* x & 0 -> 0 */
            else if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = rhs;
            }
            /* 0 & x -> 0 */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = lhs;
            }
            break;
            
        case BINOP_BIT_XOR:
            /* x ^ 0 -> x */
            if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* 0 ^ x -> x */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = rhs;
            }
            break;
            
        case BINOP_LSHIFT:
        case BINOP_RSHIFT:
            /* x << 0 -> x, x >> 0 -> x */
            if (is_int_zero(rhs) && !opt_has_side_effects(lhs)) {
                replacement = lhs;
            }
            /* 0 << x -> 0, 0 >> x -> 0 */
            else if (is_int_zero(lhs) && !opt_has_side_effects(rhs)) {
                replacement = lhs;
            }
            break;
            
        default:
            break;
    }
    
    if (replacement) {
        /* Replace the binary expression with the simplified form */
        /* Copy the replacement data into the current node */
        node->kind = replacement->kind;
        node->data = replacement->data;
        node->type = replacement->type;
        tcd->changes++;
    }
    
    return true;
}

int opt_pass_trivial_const(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    trivial_const_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, trivial_const_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Identity Operations (O0)
 * 
 * Similar to trivial_const but focuses on identity patterns:
 * - x + 0, x - 0, x * 1, x / 1, x | 0, x ^ 0, x << 0, x >> 0
 * ============================================================ */

int opt_pass_identity_ops(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    /* This is largely covered by trivial_const, but kept separate
     * for clarity and potential future expansion */
    (void)opt;
    (void)ast;
    return 0;
}

/* ============================================================
 * Double Negation Removal (O0)
 * 
 * Removes double negations:
 * - --x -> x (for arithmetic)
 * - ~~x -> x (for bitwise)
 * - !!x -> x (for boolean context, or x != 0)
 * ============================================================ */

typedef struct {
    int changes;
} double_neg_data_t;

static bool double_neg_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    double_neg_data_t *dnd = (double_neg_data_t *)data;
    (void)opt;
    
    if (node->kind != AST_UNARY_EXPR) return true;
    
    mcc_ast_node_t *operand = node->data.unary_expr.operand;
    mcc_unop_t op = node->data.unary_expr.op;
    
    /* Check for double unary */
    if (operand->kind == AST_UNARY_EXPR) {
        mcc_unop_t inner_op = operand->data.unary_expr.op;
        mcc_ast_node_t *inner_operand = operand->data.unary_expr.operand;
        
        /* --x -> x (unary minus) */
        if (op == UNOP_NEG && inner_op == UNOP_NEG) {
            node->kind = inner_operand->kind;
            node->data = inner_operand->data;
            node->type = inner_operand->type;
            dnd->changes++;
            return true;
        }
        
        /* ~~x -> x (bitwise not) */
        if (op == UNOP_BIT_NOT && inner_op == UNOP_BIT_NOT) {
            node->kind = inner_operand->kind;
            node->data = inner_operand->data;
            node->type = inner_operand->type;
            dnd->changes++;
            return true;
        }
        
        /* !!x -> x (logical not) - only in boolean context */
        /* For now, we don't do this as it changes the type */
    }
    
    return true;
}

int opt_pass_double_neg(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    double_neg_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, double_neg_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Constant Folding (O1)
 * 
 * Evaluates constant expressions at compile time:
 * - 3 + 5 -> 8
 * - 10 * 2 -> 20
 * - etc.
 * ============================================================ */

typedef struct {
    int changes;
} const_fold_data_t;

static bool const_fold_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    const_fold_data_t *cfd = (const_fold_data_t *)data;
    
    if (node->kind != AST_BINARY_EXPR && node->kind != AST_UNARY_EXPR) {
        return true;
    }
    
    /* Try to evaluate as integer constant */
    int64_t int_result;
    if (opt_eval_const_int(node, &int_result)) {
        /* Replace with integer literal */
        mcc_ast_node_t *lit = opt_make_int_lit(opt, int_result, node->location);
        if (lit) {
            node->kind = lit->kind;
            node->data = lit->data;
            /* Preserve type if set */
            cfd->changes++;
        }
        return true;
    }
    
    /* Try to evaluate as float constant */
    double float_result;
    if (opt_eval_const_float(node, &float_result)) {
        /* Replace with float literal */
        mcc_ast_node_t *lit = opt_make_float_lit(opt, float_result, node->location);
        if (lit) {
            node->kind = lit->kind;
            node->data = lit->data;
            cfd->changes++;
        }
        return true;
    }
    
    return true;
}

int opt_pass_const_fold(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    const_fold_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, const_fold_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Constant Propagation (O1)
 * 
 * Propagates known constant values through the code.
 * Requires semantic analysis for symbol tracking.
 * ============================================================ */

int opt_pass_const_prop(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    /* TODO: Implement constant propagation */
    /* This requires tracking variable values through the code */
    (void)opt;
    (void)ast;
    return 0;
}
