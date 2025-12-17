/*
 * MCC - Micro C Compiler
 * Algebraic Simplification Passes
 * 
 * Implements algebraic simplifications and strength reduction.
 */

#include "opt_internal.h"

/* ============================================================
 * AST Normalization (O0)
 * 
 * Normalizes AST to canonical form for easier optimization:
 * - Commutative operations: constant on right (x + 1, not 1 + x)
 * - Nested associative operations: flatten where possible
 * ============================================================ */

typedef struct {
    int changes;
} normalize_data_t;

static bool is_commutative(mcc_binop_t op)
{
    switch (op) {
        case BINOP_ADD:
        case BINOP_MUL:
        case BINOP_BIT_AND:
        case BINOP_BIT_OR:
        case BINOP_BIT_XOR:
        case BINOP_EQ:
        case BINOP_NE:
        case BINOP_AND:
        case BINOP_OR:
            return true;
        default:
            return false;
    }
}

static bool normalize_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    normalize_data_t *nd = (normalize_data_t *)data;
    (void)opt;
    
    if (node->kind != AST_BINARY_EXPR) return true;
    
    mcc_ast_node_t *lhs = node->data.binary_expr.lhs;
    mcc_ast_node_t *rhs = node->data.binary_expr.rhs;
    mcc_binop_t op = node->data.binary_expr.op;
    
    /* For commutative operations, put constants on the right */
    if (is_commutative(op)) {
        bool lhs_const = opt_is_const_expr(lhs);
        bool rhs_const = opt_is_const_expr(rhs);
        
        /* If LHS is constant and RHS is not, swap them */
        if (lhs_const && !rhs_const) {
            node->data.binary_expr.lhs = rhs;
            node->data.binary_expr.rhs = lhs;
            nd->changes++;
        }
    }
    
    return true;
}

int opt_pass_normalize(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    normalize_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, normalize_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Strength Reduction (O1)
 * 
 * Replaces expensive operations with cheaper ones:
 * - x * 2 -> x << 1
 * - x * 4 -> x << 2
 * - x * 2^n -> x << n
 * - x / 2 -> x >> 1 (for unsigned)
 * - x / 2^n -> x >> n (for unsigned)
 * - x % 2^n -> x & (2^n - 1) (for unsigned)
 * ============================================================ */

/* Check if value is a power of 2 and return the exponent */
static bool is_power_of_2(int64_t value, int *exponent)
{
    if (value <= 0) return false;
    if ((value & (value - 1)) != 0) return false;
    
    int exp = 0;
    while ((value >> exp) != 1) {
        exp++;
    }
    *exponent = exp;
    return true;
}

typedef struct {
    int changes;
} strength_red_data_t;

static bool strength_red_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    strength_red_data_t *srd = (strength_red_data_t *)data;
    
    if (node->kind != AST_BINARY_EXPR) return true;
    
    mcc_ast_node_t *lhs = node->data.binary_expr.lhs;
    mcc_ast_node_t *rhs = node->data.binary_expr.rhs;
    mcc_binop_t op = node->data.binary_expr.op;
    
    int64_t const_val;
    int exponent;
    
    switch (op) {
        case BINOP_MUL:
            /* x * 2^n -> x << n */
            if (rhs->kind == AST_INT_LIT) {
                const_val = rhs->data.int_lit.value;
                if (is_power_of_2(const_val, &exponent)) {
                    /* Transform to left shift */
                    node->data.binary_expr.op = BINOP_LSHIFT;
                    rhs->data.int_lit.value = exponent;
                    srd->changes++;
                }
            }
            /* 2^n * x -> x << n */
            else if (lhs->kind == AST_INT_LIT) {
                const_val = lhs->data.int_lit.value;
                if (is_power_of_2(const_val, &exponent)) {
                    /* Swap and transform to left shift */
                    node->data.binary_expr.lhs = rhs;
                    node->data.binary_expr.rhs = lhs;
                    node->data.binary_expr.op = BINOP_LSHIFT;
                    lhs->data.int_lit.value = exponent;
                    srd->changes++;
                }
            }
            break;
            
        case BINOP_DIV:
            /* x / 2^n -> x >> n (only for unsigned or known positive) */
            /* TODO: Check if type is unsigned */
            if (rhs->kind == AST_INT_LIT) {
                const_val = rhs->data.int_lit.value;
                if (is_power_of_2(const_val, &exponent)) {
                    /* For now, only do this if we know it's safe */
                    /* This requires type information from sema */
                    if (opt->sema && node->type) {
                        /* Check if unsigned */
                        /* TODO: Implement proper type checking */
                    }
                }
            }
            break;
            
        case BINOP_MOD:
            /* x % 2^n -> x & (2^n - 1) (only for unsigned) */
            if (rhs->kind == AST_INT_LIT) {
                const_val = rhs->data.int_lit.value;
                if (is_power_of_2(const_val, &exponent)) {
                    /* For now, only do this if we know it's safe */
                    if (opt->sema && node->type) {
                        /* Check if unsigned */
                        /* TODO: Implement proper type checking */
                    }
                }
            }
            break;
            
        default:
            break;
    }
    
    return true;
}

int opt_pass_strength_red(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    strength_red_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, strength_red_visitor, &data);
    return data.changes;
}

/* ============================================================
 * Algebraic Simplifications (O1)
 * 
 * Various algebraic simplifications:
 * - x - x -> 0
 * - x ^ x -> 0
 * - x & x -> x
 * - x | x -> x
 * - x / x -> 1 (if x != 0)
 * - x * x -> x (for boolean x)
 * ============================================================ */

typedef struct {
    int changes;
} algebraic_data_t;

static bool algebraic_visitor(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data)
{
    algebraic_data_t *ad = (algebraic_data_t *)data;
    
    if (node->kind != AST_BINARY_EXPR) return true;
    
    mcc_ast_node_t *lhs = node->data.binary_expr.lhs;
    mcc_ast_node_t *rhs = node->data.binary_expr.rhs;
    mcc_binop_t op = node->data.binary_expr.op;
    
    /* Check if operands are the same expression */
    if (opt_exprs_equal(lhs, rhs) && opt_is_pure_expr(lhs)) {
        mcc_ast_node_t *replacement = NULL;
        
        switch (op) {
            case BINOP_SUB:
                /* x - x -> 0 */
                replacement = opt_make_int_lit(opt, 0, node->location);
                break;
                
            case BINOP_BIT_XOR:
                /* x ^ x -> 0 */
                replacement = opt_make_int_lit(opt, 0, node->location);
                break;
                
            case BINOP_BIT_AND:
                /* x & x -> x */
                replacement = lhs;
                break;
                
            case BINOP_BIT_OR:
                /* x | x -> x */
                replacement = lhs;
                break;
                
            case BINOP_DIV:
                /* x / x -> 1 (if x is known non-zero) */
                /* Be conservative - only do this for literals */
                if (lhs->kind == AST_INT_LIT && lhs->data.int_lit.value != 0) {
                    replacement = opt_make_int_lit(opt, 1, node->location);
                }
                break;
                
            case BINOP_MOD:
                /* x % x -> 0 (if x is known non-zero) */
                if (lhs->kind == AST_INT_LIT && lhs->data.int_lit.value != 0) {
                    replacement = opt_make_int_lit(opt, 0, node->location);
                }
                break;
                
            default:
                break;
        }
        
        if (replacement) {
            node->kind = replacement->kind;
            node->data = replacement->data;
            ad->changes++;
        }
    }
    
    return true;
}

int opt_pass_algebraic(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
{
    algebraic_data_t data = { .changes = 0 };
    opt_visit_postorder(opt, ast, algebraic_visitor, &data);
    return data.changes;
}
