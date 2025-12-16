/*
 * MCC - Micro C Compiler
 * Semantic Analysis Internal Header
 * 
 * This file contains internal structures, constants, and function
 * declarations used by the semantic analysis implementation.
 */

#ifndef MCC_SEMA_INTERNAL_H
#define MCC_SEMA_INTERNAL_H

#include "mcc.h"

/* ============================================================
 * Internal Function Declarations
 * ============================================================ */

/* Expression Analysis (sema_expr.c) */
mcc_type_t *sema_analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_binary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_unary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_call_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_subscript_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_member_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_ternary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_cast_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_sizeof_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
mcc_type_t *sema_analyze_comma_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);

/* Statement Analysis (sema_stmt.c) */
bool sema_analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_compound_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_if_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_while_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_do_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_for_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_switch_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
bool sema_analyze_return_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);

/* Declaration Analysis (sema_decl.c) */
bool sema_analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl);
bool sema_analyze_func_decl(mcc_sema_t *sema, mcc_ast_node_t *decl);
bool sema_analyze_var_decl(mcc_sema_t *sema, mcc_ast_node_t *decl);

/* Type Checking (sema_type.c) */
bool sema_check_assignment_compat(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs, mcc_location_t loc);
bool sema_check_lvalue(mcc_sema_t *sema, mcc_ast_node_t *expr, mcc_location_t loc);
bool sema_check_scalar(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context);
bool sema_check_integer(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context);
bool sema_check_pointer(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc, const char *context);
bool sema_check_function(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc);
bool sema_check_complete_type(mcc_sema_t *sema, mcc_type_t *type, mcc_location_t loc);
mcc_type_t *sema_apply_integer_promotions(mcc_sema_t *sema, mcc_type_t *type);
mcc_type_t *sema_apply_usual_conversions(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs);
mcc_type_t *sema_apply_array_decay(mcc_sema_t *sema, mcc_type_t *type);
mcc_type_t *sema_apply_function_decay(mcc_sema_t *sema, mcc_type_t *type);

/* Constant Expression Evaluation (sema_const.c) */
bool sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr, int64_t *result);
bool sema_is_null_pointer_constant(mcc_sema_t *sema, mcc_ast_node_t *expr);

/* ============================================================
 * C Standard Feature Checks for Semantic Analysis
 * ============================================================ */

/* Check if a semantic feature is enabled */
static inline bool sema_has_feature(mcc_sema_t *sema, mcc_feature_id_t feat)
{
    return mcc_ctx_has_feature(sema->ctx, feat);
}

/* C89: Implicit int return type */
static inline bool sema_has_implicit_int(mcc_sema_t *sema)
{
    /* Implicit int is allowed in C89, deprecated in C99, removed in C11 */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    return std == MCC_STD_C89 || std == MCC_STD_C90 || std == MCC_STD_GNU89;
}

/* C89: Implicit function declarations */
static inline bool sema_has_implicit_func_decl(mcc_sema_t *sema)
{
    /* Implicit function declarations allowed in C89, removed in C99 */
    mcc_c_std_t std = mcc_ctx_get_std(sema->ctx);
    return std == MCC_STD_C89 || std == MCC_STD_C90 || std == MCC_STD_GNU89;
}

/* C99: Variable Length Arrays */
static inline bool sema_has_vla(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_VLA);
}

/* C99: Designated initializers */
static inline bool sema_has_designated_init(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_DESIGNATED_INIT);
}

/* C99: Compound literals */
static inline bool sema_has_compound_lit(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_COMPOUND_LIT);
}

/* C99: Flexible array members */
static inline bool sema_has_flexible_array(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_FLEXIBLE_ARRAY);
}

/* C99: _Bool type */
static inline bool sema_has_bool(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_BOOL);
}

/* C99: long long type */
static inline bool sema_has_long_long(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_LONG_LONG);
}

/* C11: _Static_assert */
static inline bool sema_has_static_assert(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_STATIC_ASSERT);
}

/* C11: _Generic selection */
static inline bool sema_has_generic(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_GENERIC);
}

/* C11: _Noreturn function specifier */
static inline bool sema_has_noreturn(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_NORETURN);
}

/* C11: _Atomic type qualifier */
static inline bool sema_has_atomic(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_ATOMIC);
}

/* C11: Anonymous structs/unions */
static inline bool sema_has_anonymous_struct(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_ANONYMOUS_STRUCT);
}

/* C23: nullptr constant */
static inline bool sema_has_nullptr(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_NULLPTR);
}

/* C23: constexpr specifier */
static inline bool sema_has_constexpr(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_CONSTEXPR);
}

/* C23: typeof operator */
static inline bool sema_has_typeof(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_TYPEOF);
}

/* C23: auto type inference */
static inline bool sema_has_auto_type(mcc_sema_t *sema)
{
    return sema_has_feature(sema, MCC_FEAT_AUTO_TYPE);
}

/* ============================================================
 * Helper Macros for Feature Warnings
 * ============================================================ */

/* Warn if using a feature not available in current standard */
#define SEMA_WARN_FEATURE(sema, feat, loc, msg) \
    do { \
        if (!sema_has_feature(sema, feat)) { \
            mcc_warning_at((sema)->ctx, loc, msg " is a C99 extension"); \
        } \
    } while (0)

/* Error if using a feature not available in current standard */
#define SEMA_REQUIRE_FEATURE(sema, feat, loc, msg) \
    do { \
        if (!sema_has_feature(sema, feat)) { \
            mcc_error_at((sema)->ctx, loc, msg " requires C99 or later"); \
            return false; \
        } \
    } while (0)

/* ============================================================
 * Diagnostic Helpers
 * ============================================================ */

/* Common error messages */
#define SEMA_ERR_UNDECLARED_IDENT     "undeclared identifier '%s'"
#define SEMA_ERR_NOT_ASSIGNABLE       "expression is not assignable"
#define SEMA_ERR_NOT_LVALUE           "expression is not an lvalue"
#define SEMA_ERR_NOT_SCALAR           "%s must be a scalar type"
#define SEMA_ERR_NOT_INTEGER          "%s must be an integer type"
#define SEMA_ERR_NOT_POINTER          "cannot dereference non-pointer type"
#define SEMA_ERR_NOT_FUNCTION         "called object is not a function"
#define SEMA_ERR_NOT_ARRAY_OR_PTR     "subscripted value is not an array or pointer"
#define SEMA_ERR_NOT_STRUCT_OR_UNION  "member reference base type is not a struct or union"
#define SEMA_ERR_NO_MEMBER            "no member named '%s'"
#define SEMA_ERR_INCOMPATIBLE_TYPES   "incompatible types in %s"
#define SEMA_ERR_ARG_COUNT            "function expects %d arguments, got %d"
#define SEMA_ERR_ARG_COUNT_VARIADIC   "function expects at least %d arguments, got %d"
#define SEMA_ERR_VOID_RETURN          "void function should not return a value"
#define SEMA_ERR_NONVOID_RETURN       "non-void function should return a value"
#define SEMA_ERR_BREAK_OUTSIDE        "break statement outside of loop or switch"
#define SEMA_ERR_CONTINUE_OUTSIDE     "continue statement outside of loop"
#define SEMA_ERR_CASE_OUTSIDE         "case statement outside of switch"
#define SEMA_ERR_DEFAULT_OUTSIDE      "default statement outside of switch"

#endif /* MCC_SEMA_INTERNAL_H */
