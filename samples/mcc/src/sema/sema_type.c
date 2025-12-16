/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Type Checking Utilities
 * 
 * This file provides type checking and type conversion utilities.
 */

#include "sema_internal.h"

/* ============================================================
 * Type Checking Predicates
 * ============================================================ */

bool sema_check_lvalue(mcc_sema_t *sema, mcc_ast_node_t *expr, mcc_location_t loc)
{
    if (!expr) return false;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR:
            /* Variable is an lvalue */
            return true;
            
        case AST_SUBSCRIPT_EXPR:
            /* Array subscript is an lvalue */
            return true;
            
        case AST_MEMBER_EXPR:
            /* Member access is an lvalue */
            return true;
            
        case AST_UNARY_EXPR:
            /* Dereference is an lvalue */
            if (expr->data.unary_expr.op == UNOP_DEREF) {
                return true;
            }
            break;
            
        default:
            break;
    }
    
    mcc_error_at(sema->ctx, loc, SEMA_ERR_NOT_LVALUE);
    return false;
}

bool sema_check_scalar(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context)
{
    if (!type) return false;
    
    if (!mcc_type_is_scalar(type)) {
        mcc_error_at(sema->ctx, loc, SEMA_ERR_NOT_SCALAR, context);
        return false;
    }
    return true;
}

bool sema_check_integer(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context)
{
    if (!type) return false;
    
    if (!mcc_type_is_integer(type)) {
        mcc_error_at(sema->ctx, loc, SEMA_ERR_NOT_INTEGER, context);
        return false;
    }
    return true;
}

bool sema_check_pointer(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context)
{
    (void)context;
    if (!type) return false;
    
    if (!mcc_type_is_pointer(type)) {
        mcc_error_at(sema->ctx, loc, SEMA_ERR_NOT_POINTER);
        return false;
    }
    return true;
}

bool sema_check_function(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc)
{
    if (!type) return false;
    
    if (!mcc_type_is_function(type)) {
        mcc_error_at(sema->ctx, loc, SEMA_ERR_NOT_FUNCTION);
        return false;
    }
    return true;
}

bool sema_check_complete_type(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc)
{
    (void)sema;
    (void)loc;
    if (!type) return false;
    
    /* Void is incomplete */
    if (mcc_type_is_void(type)) {
        return false;
    }
    
    /* Array with unknown size is incomplete */
    if (mcc_type_is_array(type) && type->data.array.length == 0) {
        return false;
    }
    
    /* Forward-declared struct/union is incomplete */
    if (mcc_type_is_record(type) && !type->data.record.is_complete) {
        return false;
    }
    
    return true;
}

/* ============================================================
 * Assignment Compatibility
 * ============================================================ */

bool sema_check_assignment_compat(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs, mcc_location_t loc)
{
    if (!lhs || !rhs) return false;
    
    /* Same type */
    if (mcc_type_is_same(lhs, rhs)) return true;
    
    /* Arithmetic types are compatible */
    if (mcc_type_is_arithmetic(lhs) && mcc_type_is_arithmetic(rhs)) return true;
    
    /* Pointer to void is compatible with any pointer */
    if (mcc_type_is_pointer(lhs) && mcc_type_is_pointer(rhs)) {
        mcc_type_t *lhs_pointee = lhs->data.pointer.pointee;
        mcc_type_t *rhs_pointee = rhs->data.pointer.pointee;
        
        if (mcc_type_is_void(lhs_pointee) || mcc_type_is_void(rhs_pointee)) {
            return true;
        }
        
        /* Check pointer compatibility (ignoring qualifiers for now) */
        if (mcc_type_is_compatible(lhs_pointee, rhs_pointee)) {
            return true;
        }
        
        mcc_warning_at(sema->ctx, loc,
                       "incompatible pointer types in assignment");
        return true;
    }
    
    /* Null pointer constant (integer 0) can be assigned to pointer */
    if (mcc_type_is_pointer(lhs) && mcc_type_is_integer(rhs)) {
        /* TODO: Check if rhs is null pointer constant */
        mcc_warning_at(sema->ctx, loc,
                       "incompatible integer to pointer conversion");
        return true;
    }
    
    /* Pointer can be assigned to integer (with warning) */
    if (mcc_type_is_integer(lhs) && mcc_type_is_pointer(rhs)) {
        mcc_warning_at(sema->ctx, loc,
                       "incompatible pointer to integer conversion");
        return true;
    }
    
    /* C23: nullptr can be assigned to any pointer */
    if (sema_has_nullptr(sema) && mcc_type_is_pointer(lhs)) {
        /* TODO: Check for nullptr constant */
    }
    
    mcc_warning_at(sema->ctx, loc, SEMA_ERR_INCOMPATIBLE_TYPES, "assignment");
    return true;
}

/* ============================================================
 * Type Promotions and Conversions
 * ============================================================ */

mcc_type_t *sema_apply_integer_promotions(mcc_sema_t *sema, mcc_type_t *type)
{
    if (!type) return NULL;
    
    /* Integer promotion: types smaller than int are promoted to int */
    return mcc_type_promote(sema->types, type);
}

mcc_type_t *sema_apply_usual_conversions(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs)
{
    if (!lhs || !rhs) return NULL;
    
    /* Usual arithmetic conversions */
    return mcc_type_common(sema->types, lhs, rhs);
}

mcc_type_t *sema_apply_array_decay(mcc_sema_t *sema, mcc_type_t *type)
{
    if (!type) return NULL;
    
    /* Array decays to pointer to first element */
    if (mcc_type_is_array(type)) {
        return mcc_type_pointer(sema->types, type->data.array.element);
    }
    
    return type;
}

mcc_type_t *sema_apply_function_decay(mcc_sema_t *sema, mcc_type_t *type)
{
    if (!type) return NULL;
    
    /* Function decays to pointer to function */
    if (mcc_type_is_function(type)) {
        return mcc_type_pointer(sema->types, type);
    }
    
    return type;
}

/* ============================================================
 * Null Pointer Constant Check
 * ============================================================ */

bool sema_is_null_pointer_constant(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    if (!expr) return false;
    
    /* Integer constant 0 */
    if (expr->kind == AST_INT_LIT && expr->data.int_lit.value == 0) {
        return true;
    }
    
    /* Cast of 0 to void* */
    if (expr->kind == AST_CAST_EXPR) {
        mcc_type_t *target = expr->data.cast_expr.target_type;
        if (mcc_type_is_pointer(target) && 
            mcc_type_is_void(target->data.pointer.pointee)) {
            return sema_is_null_pointer_constant(sema, expr->data.cast_expr.expr);
        }
    }
    
    /* C23: nullptr */
    if (sema_has_nullptr(sema)) {
        /* TODO: Check for nullptr keyword */
    }
    
    return false;
}
