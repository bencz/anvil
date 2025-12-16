/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Constant Expression Evaluation
 * 
 * This file handles compile-time evaluation of constant expressions.
 */

#include "sema_internal.h"

/* ============================================================
 * Constant Expression Evaluation
 * ============================================================ */

bool sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr, int64_t *result)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        case AST_INT_LIT:
            *result = (int64_t)expr->data.int_lit.value;
            return true;
            
        case AST_CHAR_LIT:
            *result = expr->data.char_lit.value;
            return true;
            
        case AST_BINARY_EXPR: {
            int64_t lhs, rhs;
            if (!sema_eval_const_expr(sema, expr->data.binary_expr.lhs, &lhs) ||
                !sema_eval_const_expr(sema, expr->data.binary_expr.rhs, &rhs)) {
                return false;
            }
            
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD:
                    *result = lhs + rhs;
                    break;
                case BINOP_SUB:
                    *result = lhs - rhs;
                    break;
                case BINOP_MUL:
                    *result = lhs * rhs;
                    break;
                case BINOP_DIV:
                    if (rhs == 0) {
                        mcc_error_at(sema->ctx, expr->location,
                                     "division by zero in constant expression");
                        return false;
                    }
                    *result = lhs / rhs;
                    break;
                case BINOP_MOD:
                    if (rhs == 0) {
                        mcc_error_at(sema->ctx, expr->location,
                                     "division by zero in constant expression");
                        return false;
                    }
                    *result = lhs % rhs;
                    break;
                case BINOP_LSHIFT:
                    if (rhs < 0 || rhs >= 64) {
                        mcc_warning_at(sema->ctx, expr->location,
                                       "shift count out of range");
                    }
                    *result = lhs << rhs;
                    break;
                case BINOP_RSHIFT:
                    if (rhs < 0 || rhs >= 64) {
                        mcc_warning_at(sema->ctx, expr->location,
                                       "shift count out of range");
                    }
                    *result = lhs >> rhs;
                    break;
                case BINOP_BIT_AND:
                    *result = lhs & rhs;
                    break;
                case BINOP_BIT_OR:
                    *result = lhs | rhs;
                    break;
                case BINOP_BIT_XOR:
                    *result = lhs ^ rhs;
                    break;
                case BINOP_EQ:
                    *result = lhs == rhs;
                    break;
                case BINOP_NE:
                    *result = lhs != rhs;
                    break;
                case BINOP_LT:
                    *result = lhs < rhs;
                    break;
                case BINOP_GT:
                    *result = lhs > rhs;
                    break;
                case BINOP_LE:
                    *result = lhs <= rhs;
                    break;
                case BINOP_GE:
                    *result = lhs >= rhs;
                    break;
                case BINOP_AND:
                    *result = lhs && rhs;
                    break;
                case BINOP_OR:
                    *result = lhs || rhs;
                    break;
                default:
                    return false;
            }
            return true;
        }
        
        case AST_UNARY_EXPR: {
            int64_t val;
            if (!sema_eval_const_expr(sema, expr->data.unary_expr.operand, &val)) {
                return false;
            }
            
            switch (expr->data.unary_expr.op) {
                case UNOP_NEG:
                    *result = -val;
                    break;
                case UNOP_POS:
                    *result = val;
                    break;
                case UNOP_NOT:
                    *result = !val;
                    break;
                case UNOP_BIT_NOT:
                    *result = ~val;
                    break;
                default:
                    return false;
            }
            return true;
        }
        
        case AST_TERNARY_EXPR: {
            int64_t cond;
            if (!sema_eval_const_expr(sema, expr->data.ternary_expr.cond, &cond)) {
                return false;
            }
            return sema_eval_const_expr(sema,
                cond ? expr->data.ternary_expr.then_expr : expr->data.ternary_expr.else_expr,
                result);
        }
        
        case AST_CAST_EXPR: {
            /* Evaluate the inner expression */
            int64_t val;
            if (!sema_eval_const_expr(sema, expr->data.cast_expr.expr, &val)) {
                return false;
            }
            /* TODO: Apply type conversion based on target type */
            *result = val;
            return true;
        }
        
        case AST_SIZEOF_EXPR: {
            /* sizeof is a constant expression */
            mcc_type_t *type = expr->data.sizeof_expr.type_arg;
            if (!type && expr->data.sizeof_expr.expr_arg) {
                type = expr->data.sizeof_expr.expr_arg->type;
            }
            if (type) {
                *result = (int64_t)mcc_type_sizeof(type);
                return true;
            }
            return false;
        }
        
        case AST_IDENT_EXPR: {
            /* Check for enum constant */
            mcc_symbol_t *sym = mcc_symtab_lookup(sema->symtab, expr->data.ident_expr.name);
            if (sym && sym->kind == SYM_ENUM_CONST) {
                *result = sym->data.enum_value;
                return true;
            }
            /* Not a constant */
            return false;
        }
        
        case AST_COMMA_EXPR: {
            /* Only the right side matters for value */
            int64_t left;
            sema_eval_const_expr(sema, expr->data.comma_expr.left, &left);
            return sema_eval_const_expr(sema, expr->data.comma_expr.right, result);
        }
        
        default:
            return false;
    }
}
