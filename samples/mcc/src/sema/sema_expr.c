/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Expression Analysis
 * 
 * This file handles type checking and semantic analysis of expressions.
 */

#include "sema_internal.h"
#include <string.h>

/* ============================================================
 * Literal Analysis
 * ============================================================ */

static mcc_type_t *analyze_int_lit(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *type;
    switch (expr->data.int_lit.suffix) {
        case INT_SUFFIX_U:
            type = mcc_type_uint(sema->types);
            break;
        case INT_SUFFIX_L:
            type = mcc_type_long(sema->types);
            break;
        case INT_SUFFIX_UL:
            type = mcc_type_ulong(sema->types);
            break;
        case INT_SUFFIX_LL:
            if (!sema_has_long_long(sema)) {
                mcc_warning_at(sema->ctx, expr->location,
                               "long long is a C99 extension");
            }
            type = mcc_type_llong(sema->types);
            break;
        case INT_SUFFIX_ULL:
            if (!sema_has_long_long(sema)) {
                mcc_warning_at(sema->ctx, expr->location,
                               "unsigned long long is a C99 extension");
            }
            type = mcc_type_ullong(sema->types);
            break;
        default:
            type = mcc_type_int(sema->types);
            break;
    }
    expr->type = type;
    return type;
}

static mcc_type_t *analyze_float_lit(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *type;
    switch (expr->data.float_lit.suffix) {
        case FLOAT_SUFFIX_F:
            type = mcc_type_float(sema->types);
            break;
        case FLOAT_SUFFIX_L:
            type = mcc_type_long_double(sema->types);
            break;
        default:
            type = mcc_type_double(sema->types);
            break;
    }
    expr->type = type;
    return type;
}

static mcc_type_t *analyze_char_lit(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    /* Character literals have type int in C */
    expr->type = mcc_type_int(sema->types);
    return expr->type;
}

static mcc_type_t *analyze_string_lit(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    /* String literal is array of char, decays to pointer */
    expr->type = mcc_type_pointer(sema->types, mcc_type_char(sema->types));
    return expr->type;
}

/* ============================================================
 * Identifier Analysis
 * ============================================================ */

static mcc_type_t *analyze_ident_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    const char *name = expr->data.ident_expr.name;
    
    /* C99: __func__ predefined identifier */
    if (strcmp(name, "__func__") == 0) {
        if (!sema_has_feature(sema, MCC_FEAT_FUNC_NAME)) {
            mcc_warning_at(sema->ctx, expr->location,
                           "__func__ is a C99 feature");
        }
        /* __func__ is equivalent to a static const char array */
        /* containing the function name, decays to const char* */
        mcc_type_t *char_type = mcc_type_char(sema->types);
        char_type->qualifiers |= QUAL_CONST;
        expr->type = mcc_type_pointer(sema->types, char_type);
        expr->data.ident_expr.is_func_name = true;
        return expr->type;
    }
    
    mcc_symbol_t *sym = mcc_symtab_lookup(sema->symtab, name);
    if (!sym) {
        /* C89 allows implicit function declarations */
        if (sema_has_implicit_func_decl(sema)) {
            mcc_warning_at(sema->ctx, expr->location,
                           "implicit declaration of function '%s'",
                           name);
            /* Create implicit declaration: int name() */
            mcc_type_t *func_type = mcc_type_function(sema->types,
                mcc_type_int(sema->types), NULL, 0, false);
            sym = mcc_symtab_define(sema->symtab, name,
                                    SYM_FUNC, func_type, expr->location);
        } else {
            mcc_error_at(sema->ctx, expr->location,
                         SEMA_ERR_UNDECLARED_IDENT, name);
            return NULL;
        }
    }
    expr->data.ident_expr.symbol = sym;
    sym->is_used = true;
    expr->type = sym->type;
    return expr->type;
}

/* ============================================================
 * Binary Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_binary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *lhs_type = sema_analyze_expr(sema, expr->data.binary_expr.lhs);
    mcc_type_t *rhs_type = sema_analyze_expr(sema, expr->data.binary_expr.rhs);
    
    if (!lhs_type || !rhs_type) return NULL;
    
    mcc_binop_t op = expr->data.binary_expr.op;
    
    /* Assignment operators */
    if (op >= BINOP_ASSIGN && op <= BINOP_RSHIFT_ASSIGN) {
        if (!sema_check_lvalue(sema, expr->data.binary_expr.lhs, expr->location)) {
            return NULL;
        }
        sema_check_assignment_compat(sema, lhs_type, rhs_type, expr->location);
        expr->type = lhs_type;
        return expr->type;
    }
    
    /* Comparison operators return int */
    if (op >= BINOP_EQ && op <= BINOP_GE) {
        expr->type = mcc_type_int(sema->types);
        return expr->type;
    }
    
    /* Logical operators return int */
    if (op == BINOP_AND || op == BINOP_OR) {
        if (!sema_check_scalar(sema, lhs_type, expr->location, "logical operand") ||
            !sema_check_scalar(sema, rhs_type, expr->location, "logical operand")) {
            return NULL;
        }
        expr->type = mcc_type_int(sema->types);
        return expr->type;
    }
    
    /* Pointer arithmetic */
    if (mcc_type_is_pointer(lhs_type) && mcc_type_is_integer(rhs_type)) {
        if (op == BINOP_ADD || op == BINOP_SUB) {
            expr->type = lhs_type;
            return expr->type;
        }
    }
    if (mcc_type_is_integer(lhs_type) && mcc_type_is_pointer(rhs_type)) {
        if (op == BINOP_ADD) {
            expr->type = rhs_type;
            return expr->type;
        }
    }
    
    /* Pointer subtraction */
    if (mcc_type_is_pointer(lhs_type) && mcc_type_is_pointer(rhs_type)) {
        if (op == BINOP_SUB) {
            expr->type = mcc_type_long(sema->types); /* ptrdiff_t */
            return expr->type;
        }
    }
    
    /* Bitwise operators require integer types */
    if (op == BINOP_BIT_AND || op == BINOP_BIT_OR || op == BINOP_BIT_XOR ||
        op == BINOP_LSHIFT || op == BINOP_RSHIFT) {
        if (!sema_check_integer(sema, lhs_type, expr->location, "bitwise operand") ||
            !sema_check_integer(sema, rhs_type, expr->location, "bitwise operand")) {
            return NULL;
        }
    }
    
    /* Usual arithmetic conversions */
    expr->type = sema_apply_usual_conversions(sema, lhs_type, rhs_type);
    return expr->type;
}

/* ============================================================
 * Unary Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_unary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *operand_type = sema_analyze_expr(sema, expr->data.unary_expr.operand);
    if (!operand_type) return NULL;
    
    switch (expr->data.unary_expr.op) {
        case UNOP_NEG:
        case UNOP_POS:
            expr->type = sema_apply_integer_promotions(sema, operand_type);
            break;
            
        case UNOP_NOT:
            if (!sema_check_scalar(sema, operand_type, expr->location, "logical operand")) {
                return NULL;
            }
            expr->type = mcc_type_int(sema->types);
            break;
            
        case UNOP_BIT_NOT:
            if (!sema_check_integer(sema, operand_type, expr->location, "bitwise operand")) {
                return NULL;
            }
            expr->type = sema_apply_integer_promotions(sema, operand_type);
            break;
            
        case UNOP_DEREF:
            if (!sema_check_pointer(sema, operand_type, expr->location, "dereference")) {
                return NULL;
            }
            expr->type = operand_type->data.pointer.pointee;
            break;
            
        case UNOP_ADDR:
            if (!sema_check_lvalue(sema, expr->data.unary_expr.operand, expr->location)) {
                mcc_warning_at(sema->ctx, expr->location,
                               "taking address of non-lvalue");
            }
            expr->type = mcc_type_pointer(sema->types, operand_type);
            break;
            
        case UNOP_PRE_INC:
        case UNOP_PRE_DEC:
        case UNOP_POST_INC:
        case UNOP_POST_DEC:
            if (!sema_check_lvalue(sema, expr->data.unary_expr.operand, expr->location)) {
                return NULL;
            }
            expr->type = operand_type;
            break;
            
        default:
            expr->type = operand_type;
            break;
    }
    return expr->type;
}

/* ============================================================
 * Ternary Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_ternary_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *cond_type = sema_analyze_expr(sema, expr->data.ternary_expr.cond);
    mcc_type_t *then_type = sema_analyze_expr(sema, expr->data.ternary_expr.then_expr);
    mcc_type_t *else_type = sema_analyze_expr(sema, expr->data.ternary_expr.else_expr);
    
    if (!cond_type || !then_type || !else_type) return NULL;
    
    if (!sema_check_scalar(sema, cond_type, expr->location, "condition")) {
        return NULL;
    }
    
    expr->type = sema_apply_usual_conversions(sema, then_type, else_type);
    return expr->type;
}

/* ============================================================
 * Call Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_call_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *func_type = sema_analyze_expr(sema, expr->data.call_expr.func);
    if (!func_type) return NULL;
    
    /* Handle pointer to function */
    func_type = sema_apply_function_decay(sema, func_type);
    if (mcc_type_is_pointer(func_type)) {
        func_type = func_type->data.pointer.pointee;
    }
    
    if (!sema_check_function(sema, func_type, expr->location)) {
        return NULL;
    }
    
    /* Check argument count */
    int expected = func_type->data.function.num_params;
    int actual = (int)expr->data.call_expr.num_args;
    
    if (!func_type->data.function.is_variadic && actual != expected) {
        mcc_error_at(sema->ctx, expr->location,
                     SEMA_ERR_ARG_COUNT, expected, actual);
    } else if (func_type->data.function.is_variadic && actual < expected) {
        mcc_error_at(sema->ctx, expr->location,
                     SEMA_ERR_ARG_COUNT_VARIADIC, expected, actual);
    }
    
    /* Analyze arguments */
    for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
        sema_analyze_expr(sema, expr->data.call_expr.args[i]);
    }
    
    expr->type = func_type->data.function.return_type;
    return expr->type;
}

/* ============================================================
 * Subscript Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_subscript_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *array_type = sema_analyze_expr(sema, expr->data.subscript_expr.array);
    mcc_type_t *index_type = sema_analyze_expr(sema, expr->data.subscript_expr.index);
    
    if (!array_type || !index_type) return NULL;
    
    /* Array decays to pointer */
    array_type = sema_apply_array_decay(sema, array_type);
    
    if (!mcc_type_is_pointer(array_type)) {
        mcc_error_at(sema->ctx, expr->location, SEMA_ERR_NOT_ARRAY_OR_PTR);
        return NULL;
    }
    
    if (!mcc_type_is_integer(index_type)) {
        mcc_error_at(sema->ctx, expr->location,
                     "array subscript is not an integer");
    }
    
    expr->type = array_type->data.pointer.pointee;
    return expr->type;
}

/* ============================================================
 * Member Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_member_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    mcc_type_t *obj_type = sema_analyze_expr(sema, expr->data.member_expr.object);
    if (!obj_type) return NULL;
    
    /* Handle arrow operator */
    if (expr->data.member_expr.is_arrow) {
        if (!mcc_type_is_pointer(obj_type)) {
            mcc_error_at(sema->ctx, expr->location,
                         "member reference type is not a pointer");
            return NULL;
        }
        obj_type = obj_type->data.pointer.pointee;
    }
    
    if (!mcc_type_is_record(obj_type)) {
        mcc_error_at(sema->ctx, expr->location, SEMA_ERR_NOT_STRUCT_OR_UNION);
        return NULL;
    }
    
    mcc_struct_field_t *field = mcc_type_find_field(obj_type, expr->data.member_expr.member);
    if (!field) {
        /* C11: Check anonymous struct/union members */
        if (sema_has_anonymous_struct(sema)) {
            /* TODO: Search in anonymous members */
        }
        mcc_error_at(sema->ctx, expr->location,
                     SEMA_ERR_NO_MEMBER, expr->data.member_expr.member);
        return NULL;
    }
    
    expr->type = field->type;
    return expr->type;
}

/* ============================================================
 * Cast Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_cast_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    sema_analyze_expr(sema, expr->data.cast_expr.expr);
    expr->type = expr->data.cast_expr.target_type;
    return expr->type;
}

/* ============================================================
 * Sizeof Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_sizeof_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    if (expr->data.sizeof_expr.type_arg) {
        /* sizeof(type) */
        if (!sema_check_complete_type(sema, expr->data.sizeof_expr.type_arg, expr->location)) {
            /* VLA is allowed in C99 */
            if (!sema_has_vla(sema)) {
                return NULL;
            }
        }
    } else if (expr->data.sizeof_expr.expr_arg) {
        sema_analyze_expr(sema, expr->data.sizeof_expr.expr_arg);
    }
    expr->type = mcc_type_ulong(sema->types); /* size_t */
    return expr->type;
}

/* ============================================================
 * Comma Expression Analysis
 * ============================================================ */

mcc_type_t *sema_analyze_comma_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    sema_analyze_expr(sema, expr->data.comma_expr.left);
    mcc_type_t *right_type = sema_analyze_expr(sema, expr->data.comma_expr.right);
    expr->type = right_type;
    return expr->type;
}

/* ============================================================
 * Init List Analysis
 * ============================================================ */

static mcc_type_t *analyze_init_list(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    for (size_t i = 0; i < expr->data.init_list.num_exprs; i++) {
        sema_analyze_expr(sema, expr->data.init_list.exprs[i]);
    }
    return NULL; /* Init list type depends on context */
}

/* ============================================================
 * Main Expression Analysis Entry Point
 * ============================================================ */

mcc_type_t *sema_analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_INT_LIT:
            return analyze_int_lit(sema, expr);
            
        case AST_FLOAT_LIT:
            return analyze_float_lit(sema, expr);
            
        case AST_CHAR_LIT:
            return analyze_char_lit(sema, expr);
            
        case AST_STRING_LIT:
            return analyze_string_lit(sema, expr);
            
        case AST_IDENT_EXPR:
            return analyze_ident_expr(sema, expr);
            
        case AST_BINARY_EXPR:
            return sema_analyze_binary_expr(sema, expr);
            
        case AST_UNARY_EXPR:
            return sema_analyze_unary_expr(sema, expr);
            
        case AST_TERNARY_EXPR:
            return sema_analyze_ternary_expr(sema, expr);
            
        case AST_CALL_EXPR:
            return sema_analyze_call_expr(sema, expr);
            
        case AST_SUBSCRIPT_EXPR:
            return sema_analyze_subscript_expr(sema, expr);
            
        case AST_MEMBER_EXPR:
            return sema_analyze_member_expr(sema, expr);
            
        case AST_CAST_EXPR:
            return sema_analyze_cast_expr(sema, expr);
            
        case AST_SIZEOF_EXPR:
            return sema_analyze_sizeof_expr(sema, expr);
            
        case AST_COMMA_EXPR:
            return sema_analyze_comma_expr(sema, expr);
            
        case AST_INIT_LIST:
            return analyze_init_list(sema, expr);
            
        default:
            return NULL;
    }
}
