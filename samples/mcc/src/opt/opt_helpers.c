/*
 * MCC - Micro C Compiler
 * AST Optimization Helper Functions
 * 
 * Common utilities used by optimization passes.
 */

#include "opt_internal.h"

/* ============================================================
 * Constant Expression Evaluation
 * ============================================================ */

bool opt_is_const_expr(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        case AST_INT_LIT:
        case AST_FLOAT_LIT:
        case AST_CHAR_LIT:
            return true;
            
        case AST_UNARY_EXPR:
            return opt_is_const_expr(expr->data.unary_expr.operand);
            
        case AST_BINARY_EXPR:
            return opt_is_const_expr(expr->data.binary_expr.lhs) &&
                   opt_is_const_expr(expr->data.binary_expr.rhs);
            
        case AST_CAST_EXPR:
            return opt_is_const_expr(expr->data.cast_expr.expr);
            
        case AST_SIZEOF_EXPR:
            /* sizeof is always constant (except VLAs) */
            return true;
            
        default:
            return false;
    }
}

bool opt_eval_const_int(mcc_ast_node_t *expr, int64_t *result)
{
    if (!expr || !result) return false;
    
    switch (expr->kind) {
        case AST_INT_LIT:
            *result = expr->data.int_lit.value;
            return true;
            
        case AST_CHAR_LIT:
            *result = expr->data.char_lit.value;
            return true;
            
        case AST_UNARY_EXPR: {
            int64_t val;
            if (!opt_eval_const_int(expr->data.unary_expr.operand, &val)) {
                return false;
            }
            
            switch (expr->data.unary_expr.op) {
                case UNOP_NEG:      *result = -val; return true;
                case UNOP_NOT:      *result = !val; return true;
                case UNOP_BIT_NOT:  *result = ~val; return true;
                case UNOP_POS:      *result = val; return true;
                default:            return false;
            }
        }
        
        case AST_BINARY_EXPR: {
            int64_t lhs, rhs;
            if (!opt_eval_const_int(expr->data.binary_expr.lhs, &lhs) ||
                !opt_eval_const_int(expr->data.binary_expr.rhs, &rhs)) {
                return false;
            }
            
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD:     *result = lhs + rhs; return true;
                case BINOP_SUB:     *result = lhs - rhs; return true;
                case BINOP_MUL:     *result = lhs * rhs; return true;
                case BINOP_DIV:     
                    if (rhs == 0) return false;
                    *result = lhs / rhs; 
                    return true;
                case BINOP_MOD:     
                    if (rhs == 0) return false;
                    *result = lhs % rhs; 
                    return true;
                case BINOP_BIT_AND: *result = lhs & rhs; return true;
                case BINOP_BIT_OR:  *result = lhs | rhs; return true;
                case BINOP_BIT_XOR: *result = lhs ^ rhs; return true;
                case BINOP_LSHIFT:  *result = lhs << rhs; return true;
                case BINOP_RSHIFT:  *result = lhs >> rhs; return true;
                case BINOP_EQ:      *result = lhs == rhs; return true;
                case BINOP_NE:      *result = lhs != rhs; return true;
                case BINOP_LT:      *result = lhs < rhs; return true;
                case BINOP_LE:      *result = lhs <= rhs; return true;
                case BINOP_GT:      *result = lhs > rhs; return true;
                case BINOP_GE:      *result = lhs >= rhs; return true;
                case BINOP_AND:     *result = lhs && rhs; return true;
                case BINOP_OR:      *result = lhs || rhs; return true;
                default:            return false;
            }
        }
        
        case AST_CAST_EXPR:
            /* For now, just pass through */
            return opt_eval_const_int(expr->data.cast_expr.expr, result);
            
        default:
            return false;
    }
}

bool opt_eval_const_float(mcc_ast_node_t *expr, double *result)
{
    if (!expr || !result) return false;
    
    switch (expr->kind) {
        case AST_FLOAT_LIT:
            *result = expr->data.float_lit.value;
            return true;
            
        case AST_INT_LIT:
            *result = (double)expr->data.int_lit.value;
            return true;
            
        case AST_UNARY_EXPR: {
            double val;
            if (!opt_eval_const_float(expr->data.unary_expr.operand, &val)) {
                return false;
            }
            
            switch (expr->data.unary_expr.op) {
                case UNOP_NEG:  *result = -val; return true;
                case UNOP_POS:  *result = val; return true;
                default:        return false;
            }
        }
        
        case AST_BINARY_EXPR: {
            double lhs, rhs;
            if (!opt_eval_const_float(expr->data.binary_expr.lhs, &lhs) ||
                !opt_eval_const_float(expr->data.binary_expr.rhs, &rhs)) {
                return false;
            }
            
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD: *result = lhs + rhs; return true;
                case BINOP_SUB: *result = lhs - rhs; return true;
                case BINOP_MUL: *result = lhs * rhs; return true;
                case BINOP_DIV: 
                    if (rhs == 0.0) return false;
                    *result = lhs / rhs; 
                    return true;
                default:        return false;
            }
        }
        
        default:
            return false;
    }
}

/* ============================================================
 * AST Node Creation
 * ============================================================ */

mcc_ast_node_t *opt_make_int_lit(mcc_ast_opt_t *opt, int64_t value, 
                                  mcc_location_t loc)
{
    mcc_ast_node_t *node = mcc_alloc(opt->ctx, sizeof(mcc_ast_node_t));
    if (!node) return NULL;
    
    memset(node, 0, sizeof(mcc_ast_node_t));
    node->kind = AST_INT_LIT;
    node->location = loc;
    node->data.int_lit.value = (uint64_t)value;
    node->data.int_lit.suffix = INT_SUFFIX_NONE;
    
    return node;
}

mcc_ast_node_t *opt_make_float_lit(mcc_ast_opt_t *opt, double value,
                                    mcc_location_t loc)
{
    mcc_ast_node_t *node = mcc_alloc(opt->ctx, sizeof(mcc_ast_node_t));
    if (!node) return NULL;
    
    memset(node, 0, sizeof(mcc_ast_node_t));
    node->kind = AST_FLOAT_LIT;
    node->location = loc;
    node->data.float_lit.value = value;
    node->data.float_lit.suffix = FLOAT_SUFFIX_NONE;
    
    return node;
}

/* ============================================================
 * Side Effect Analysis
 * ============================================================ */

bool opt_has_side_effects(mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        /* Literals have no side effects */
        case AST_INT_LIT:
        case AST_FLOAT_LIT:
        case AST_CHAR_LIT:
        case AST_STRING_LIT:
            return false;
            
        /* Variable references have no side effects */
        case AST_IDENT_EXPR:
            return false;
            
        /* Unary operators */
        case AST_UNARY_EXPR:
            /* ++, --, & (address-of) and * (deref) may have side effects */
            switch (expr->data.unary_expr.op) {
                case UNOP_PRE_INC:
                case UNOP_PRE_DEC:
                case UNOP_POST_INC:
                case UNOP_POST_DEC:
                    return true;
                default:
                    return opt_has_side_effects(expr->data.unary_expr.operand);
            }
            
        /* Binary operators */
        case AST_BINARY_EXPR:
            /* Assignment operators have side effects */
            switch (expr->data.binary_expr.op) {
                case BINOP_ASSIGN:
                case BINOP_ADD_ASSIGN:
                case BINOP_SUB_ASSIGN:
                case BINOP_MUL_ASSIGN:
                case BINOP_DIV_ASSIGN:
                case BINOP_MOD_ASSIGN:
                case BINOP_AND_ASSIGN:
                case BINOP_OR_ASSIGN:
                case BINOP_XOR_ASSIGN:
                case BINOP_LSHIFT_ASSIGN:
                case BINOP_RSHIFT_ASSIGN:
                    return true;
                default:
                    return opt_has_side_effects(expr->data.binary_expr.lhs) ||
                           opt_has_side_effects(expr->data.binary_expr.rhs);
            }
            
        /* Function calls always have potential side effects */
        case AST_CALL_EXPR:
            return true;
            
        /* Comma expression */
        case AST_COMMA_EXPR:
            return opt_has_side_effects(expr->data.comma_expr.left) ||
                   opt_has_side_effects(expr->data.comma_expr.right);
            
        /* Ternary */
        case AST_TERNARY_EXPR:
            return opt_has_side_effects(expr->data.ternary_expr.cond) ||
                   opt_has_side_effects(expr->data.ternary_expr.then_expr) ||
                   opt_has_side_effects(expr->data.ternary_expr.else_expr);
            
        /* Cast */
        case AST_CAST_EXPR:
            return opt_has_side_effects(expr->data.cast_expr.expr);
            
        /* Member access */
        case AST_MEMBER_EXPR:
            return opt_has_side_effects(expr->data.member_expr.object);
            
        /* Array subscript */
        case AST_SUBSCRIPT_EXPR:
            return opt_has_side_effects(expr->data.subscript_expr.array) ||
                   opt_has_side_effects(expr->data.subscript_expr.index);
            
        default:
            /* Assume side effects for unknown nodes */
            return true;
    }
}

bool opt_is_pure_expr(mcc_ast_node_t *expr)
{
    /* A pure expression has no side effects and always produces the same result */
    if (opt_has_side_effects(expr)) return false;
    
    /* Check for volatile accesses */
    /* TODO: Check type qualifiers */
    
    return true;
}

/* ============================================================
 * Expression Comparison
 * ============================================================ */

bool opt_exprs_equal(mcc_ast_node_t *a, mcc_ast_node_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case AST_INT_LIT:
            return a->data.int_lit.value == b->data.int_lit.value;
            
        case AST_FLOAT_LIT:
            return a->data.float_lit.value == b->data.float_lit.value;
            
        case AST_CHAR_LIT:
            return a->data.char_lit.value == b->data.char_lit.value;
            
        case AST_STRING_LIT:
            return strcmp(a->data.string_lit.value, b->data.string_lit.value) == 0;
            
        case AST_IDENT_EXPR:
            return strcmp(a->data.ident_expr.name, b->data.ident_expr.name) == 0;
            
        case AST_UNARY_EXPR:
            return a->data.unary_expr.op == b->data.unary_expr.op &&
                   opt_exprs_equal(a->data.unary_expr.operand, 
                                   b->data.unary_expr.operand);
            
        case AST_BINARY_EXPR:
            return a->data.binary_expr.op == b->data.binary_expr.op &&
                   opt_exprs_equal(a->data.binary_expr.lhs, b->data.binary_expr.lhs) &&
                   opt_exprs_equal(a->data.binary_expr.rhs, b->data.binary_expr.rhs);
            
        default:
            /* For complex expressions, be conservative */
            return false;
    }
}

/* ============================================================
 * AST Traversal
 * ============================================================ */

void opt_visit_preorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast,
                        opt_visit_fn fn, void *data)
{
    if (!ast || !fn) return;
    
    /* Visit this node first */
    if (!fn(opt, ast, data)) return;
    
    /* Then visit children based on node kind */
    switch (ast->kind) {
        case AST_TRANSLATION_UNIT:
            for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
                opt_visit_preorder(opt, ast->data.translation_unit.decls[i], fn, data);
            }
            break;
            
        case AST_FUNC_DECL:
            if (ast->data.func_decl.body) {
                opt_visit_preorder(opt, ast->data.func_decl.body, fn, data);
            }
            break;
            
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < ast->data.compound_stmt.num_stmts; i++) {
                opt_visit_preorder(opt, ast->data.compound_stmt.stmts[i], fn, data);
            }
            break;
            
        case AST_IF_STMT:
            opt_visit_preorder(opt, ast->data.if_stmt.cond, fn, data);
            opt_visit_preorder(opt, ast->data.if_stmt.then_stmt, fn, data);
            if (ast->data.if_stmt.else_stmt) {
                opt_visit_preorder(opt, ast->data.if_stmt.else_stmt, fn, data);
            }
            break;
            
        case AST_WHILE_STMT:
            opt_visit_preorder(opt, ast->data.while_stmt.cond, fn, data);
            opt_visit_preorder(opt, ast->data.while_stmt.body, fn, data);
            break;
            
        case AST_FOR_STMT:
            if (ast->data.for_stmt.init) {
                opt_visit_preorder(opt, ast->data.for_stmt.init, fn, data);
            }
            if (ast->data.for_stmt.cond) {
                opt_visit_preorder(opt, ast->data.for_stmt.cond, fn, data);
            }
            if (ast->data.for_stmt.incr) {
                opt_visit_preorder(opt, ast->data.for_stmt.incr, fn, data);
            }
            opt_visit_preorder(opt, ast->data.for_stmt.body, fn, data);
            break;
            
        case AST_RETURN_STMT:
            if (ast->data.return_stmt.expr) {
                opt_visit_preorder(opt, ast->data.return_stmt.expr, fn, data);
            }
            break;
            
        case AST_EXPR_STMT:
            if (ast->data.expr_stmt.expr) {
                opt_visit_preorder(opt, ast->data.expr_stmt.expr, fn, data);
            }
            break;
            
        case AST_BINARY_EXPR:
            opt_visit_preorder(opt, ast->data.binary_expr.lhs, fn, data);
            opt_visit_preorder(opt, ast->data.binary_expr.rhs, fn, data);
            break;
            
        case AST_UNARY_EXPR:
            opt_visit_preorder(opt, ast->data.unary_expr.operand, fn, data);
            break;
            
        case AST_CAST_EXPR:
            opt_visit_preorder(opt, ast->data.cast_expr.expr, fn, data);
            break;
            
        case AST_CALL_EXPR:
            opt_visit_preorder(opt, ast->data.call_expr.func, fn, data);
            for (size_t i = 0; i < ast->data.call_expr.num_args; i++) {
                opt_visit_preorder(opt, ast->data.call_expr.args[i], fn, data);
            }
            break;
            
        default:
            /* Leaf nodes or unhandled - nothing to traverse */
            break;
    }
}

void opt_visit_postorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast,
                         opt_visit_fn fn, void *data)
{
    if (!ast || !fn) return;
    
    /* Visit children first based on node kind */
    switch (ast->kind) {
        case AST_TRANSLATION_UNIT:
            for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
                opt_visit_postorder(opt, ast->data.translation_unit.decls[i], fn, data);
            }
            break;
            
        case AST_FUNC_DECL:
            if (ast->data.func_decl.body) {
                opt_visit_postorder(opt, ast->data.func_decl.body, fn, data);
            }
            break;
            
        case AST_COMPOUND_STMT:
            for (size_t i = 0; i < ast->data.compound_stmt.num_stmts; i++) {
                opt_visit_postorder(opt, ast->data.compound_stmt.stmts[i], fn, data);
            }
            break;
            
        case AST_IF_STMT:
            opt_visit_postorder(opt, ast->data.if_stmt.cond, fn, data);
            opt_visit_postorder(opt, ast->data.if_stmt.then_stmt, fn, data);
            if (ast->data.if_stmt.else_stmt) {
                opt_visit_postorder(opt, ast->data.if_stmt.else_stmt, fn, data);
            }
            break;
            
        case AST_WHILE_STMT:
            opt_visit_postorder(opt, ast->data.while_stmt.cond, fn, data);
            opt_visit_postorder(opt, ast->data.while_stmt.body, fn, data);
            break;
            
        case AST_FOR_STMT:
            if (ast->data.for_stmt.init) {
                opt_visit_postorder(opt, ast->data.for_stmt.init, fn, data);
            }
            if (ast->data.for_stmt.cond) {
                opt_visit_postorder(opt, ast->data.for_stmt.cond, fn, data);
            }
            if (ast->data.for_stmt.incr) {
                opt_visit_postorder(opt, ast->data.for_stmt.incr, fn, data);
            }
            opt_visit_postorder(opt, ast->data.for_stmt.body, fn, data);
            break;
            
        case AST_RETURN_STMT:
            if (ast->data.return_stmt.expr) {
                opt_visit_postorder(opt, ast->data.return_stmt.expr, fn, data);
            }
            break;
            
        case AST_EXPR_STMT:
            if (ast->data.expr_stmt.expr) {
                opt_visit_postorder(opt, ast->data.expr_stmt.expr, fn, data);
            }
            break;
            
        case AST_BINARY_EXPR:
            opt_visit_postorder(opt, ast->data.binary_expr.lhs, fn, data);
            opt_visit_postorder(opt, ast->data.binary_expr.rhs, fn, data);
            break;
            
        case AST_UNARY_EXPR:
            opt_visit_postorder(opt, ast->data.unary_expr.operand, fn, data);
            break;
            
        case AST_CAST_EXPR:
            opt_visit_postorder(opt, ast->data.cast_expr.expr, fn, data);
            break;
            
        case AST_CALL_EXPR:
            opt_visit_postorder(opt, ast->data.call_expr.func, fn, data);
            for (size_t i = 0; i < ast->data.call_expr.num_args; i++) {
                opt_visit_postorder(opt, ast->data.call_expr.args[i], fn, data);
            }
            break;
            
        default:
            /* Leaf nodes or unhandled - nothing to traverse */
            break;
    }
    
    /* Then visit this node */
    fn(opt, ast, data);
}
