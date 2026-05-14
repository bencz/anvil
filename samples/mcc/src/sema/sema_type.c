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
    (void)loc;
    if (!type) return false;
    
    /* Void is incomplete */
    if (mcc_type_is_void(type)) {
        return false;
    }
    
    /* Array with unknown size is incomplete, unless it's a VLA (C99) */
    if (mcc_type_is_array(type) && type->data.array.length == 0) {
        /* VLA: array with size expression but no constant length */
        if (type->data.array.is_vla && sema_has_vla(sema)) {
            /* VLA is complete in C99 */
            return true;
        }
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

    /* Arithmetic types are compatible (implicit conversion) */
    if (mcc_type_is_arithmetic(lhs) && mcc_type_is_arithmetic(rhs)) return true;

    /* Pointer to void is compatible with any data pointer */
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

        /* Genuine mismatch (e.g. int* vs struct*). Warn but accept so we
         * match gcc/clang default behaviour; callers that want a hard
         * error should pass -Werror. */
        mcc_warning_at(sema->ctx, loc,
                       "incompatible pointer types in assignment: '%s' vs '%s'",
                       mcc_type_to_string(lhs), mcc_type_to_string(rhs));
        return true;
    }

    /* Null pointer constant (integer 0) can be assigned to pointer */
    if (mcc_type_is_pointer(lhs) && mcc_type_is_integer(rhs)) {
        /* Without access to the expression we can't verify it's the
         * literal 0 — assume it is and return true quietly to match
         * common code that assigns 0 to pointers. */
        return true;
    }

    /* Pointer can be assigned to integer (with warning) */
    if (mcc_type_is_integer(lhs) && mcc_type_is_pointer(rhs)) {
        mcc_warning_at(sema->ctx, loc,
                       "incompatible pointer to integer conversion");
        return true;
    }

    /* Function can be assigned to pointer-to-function (function decays to pointer) */
    if (mcc_type_is_pointer(lhs) && rhs->kind == TYPE_FUNCTION) {
        mcc_type_t *lhs_pointee = lhs->data.pointer.pointee;
        if (lhs_pointee && lhs_pointee->kind == TYPE_FUNCTION) {
            if (mcc_type_is_compatible(lhs_pointee, rhs)) {
                return true;
            }
            mcc_warning_at(sema->ctx, loc,
                           "incompatible function pointer types in assignment");
            return true;
        }
    }

    /* C23: nullptr can be assigned to any pointer */
    if (sema_has_nullptr(sema) && mcc_type_is_pointer(lhs)) {
        /* TODO: Check for nullptr constant */
    }

    /* Array initialization from compatible array (e.g. char[] = "str") or
     * from pointer-to-compatible (rare) — accept silently. Arrays vs
     * pointers with same element decay-equivalent also fine. */
    if (lhs->kind == TYPE_ARRAY && rhs->kind == TYPE_ARRAY) {
        if (mcc_type_is_compatible(lhs->data.array.element,
                                   rhs->data.array.element)) {
            return true;
        }
    }
    if (lhs->kind == TYPE_ARRAY && mcc_type_is_pointer(rhs)) {
        if (mcc_type_is_compatible(lhs->data.array.element,
                                   rhs->data.pointer.pointee)) {
            return true;
        }
    }

    /* Struct-to-struct assignment of the same (tagged) type was handled by
     * mcc_type_is_same at the top. Genuinely incompatible struct mismatches
     * are the main case we want to catch here. */
    if ((lhs->kind == TYPE_STRUCT || lhs->kind == TYPE_UNION) &&
        (rhs->kind == TYPE_STRUCT || rhs->kind == TYPE_UNION)) {
        mcc_error_at(sema->ctx, loc,
                     "incompatible record types in assignment: '%s' vs '%s'",
                     mcc_type_to_string(lhs), mcc_type_to_string(rhs));
        return false;
    }

    /* Everything else: warn but accept, matching historical behaviour. */
    mcc_warning_at(sema->ctx, loc,
                   "incompatible types in assignment: '%s' = '%s'",
                   mcc_type_to_string(lhs), mcc_type_to_string(rhs));
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
    /* Previously duplicated mcc_type_decay's array logic here. Delegate
     * to the canonical implementation so both sides stay in sync. */
    return type && mcc_type_is_array(type) ? mcc_type_decay(sema->types, type) : type;
}

mcc_type_t *sema_apply_function_decay(mcc_sema_t *sema, mcc_type_t *type)
{
    return type && mcc_type_is_function(type) ? mcc_type_decay(sema->types, type) : type;
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
