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
            mcc_error_at(sema->ctx, expr->location,
                         "long double literals are not implemented by MCC");
            return NULL;
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
        /* containing the function name, decays to const char*.
         * Never mutate the shared 'char' singleton — use
         * mcc_type_qualified to get a fresh const-qualified copy. */
        mcc_type_t *char_type = mcc_type_char(sema->types);
        mcc_type_t *const_char = mcc_type_qualified(sema->types, char_type, QUAL_CONST);
        expr->type = mcc_type_pointer(sema->types, const_char);
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
            expr->type = mcc_type_ptrdiff_t(sema->types);
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
    
    if (mcc_type_is_record(then_type) || mcc_type_is_record(else_type))
    {
        if (!mcc_type_is_compatible(then_type, else_type))
        {
            mcc_error_at(sema->ctx, expr->location, "conditional aggregate operands must have compatible types");
            return NULL;
        }

        expr->type = then_type;
    }
    else
    {
        expr->type = sema_apply_usual_conversions(sema, then_type, else_type);
    }
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

static size_t count_promoted_members(mcc_type_t *type, const char *name)
{
    if (!type || !mcc_type_is_record(type)) return 0;

    size_t count = 0;
    for (mcc_struct_field_t *field = type->data.record.fields;
         field; field = field->next) {
        if (field->name) {
            if (strcmp(field->name, name) == 0) count++;
        } else if (field->type && mcc_type_is_record(field->type)) {
            count += count_promoted_members(field->type, name);
        }
    }
    return count;
}

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
    
    size_t matches = count_promoted_members(
        obj_type, expr->data.member_expr.member);
    if (matches > 1) {
        mcc_error_at(sema->ctx, expr->location,
                     "member '%s' is ambiguous through anonymous records",
                     expr->data.member_expr.member);
        return NULL;
    }

    mcc_struct_field_t *field = mcc_type_find_field(
        obj_type, expr->data.member_expr.member);
    if (!field) {
        mcc_error_at(sema->ctx, expr->location,
                     SEMA_ERR_NO_MEMBER, expr->data.member_expr.member);
        return NULL;
    }
    
    expr->type = field->type;
    if (obj_type->qualifiers & (QUAL_CONST | QUAL_VOLATILE))
        expr->type = mcc_type_qualified(sema->types, field->type, field->type->qualifiers | (obj_type->qualifiers & (QUAL_CONST | QUAL_VOLATILE)));

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
    mcc_type_t *operand_type = expr->data.sizeof_expr.type_arg;
    if (expr->data.sizeof_expr.type_arg) {
        /* sizeof(type) */
    } else if (expr->data.sizeof_expr.expr_arg) {
        operand_type = sema_analyze_expr(sema,
                                         expr->data.sizeof_expr.expr_arg);
    }
    while (operand_type && operand_type->kind == TYPE_TYPEDEF)
        operand_type = operand_type->data.typedef_ref.underlying;
    if (!operand_type || mcc_type_is_function(operand_type) ||
        !sema_check_complete_type(sema, operand_type, expr->location)) {
        mcc_error_at(sema->ctx, expr->location,
                     "sizeof requires a complete object type");
        return NULL;
    }
    if (mcc_type_is_array(operand_type) && operand_type->data.array.is_vla) {
        /* Correct C semantics retain the evaluated VLA bound from its
         * declaration. MCC currently retains only the source expression;
         * reevaluating it here would duplicate side effects. Fail closed. */
        mcc_error_at(sema->ctx, expr->location,
                     "sizeof on a variable-length array is not implemented");
        return NULL;
    }
    expr->type = mcc_type_size_t(sema->types);
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
        mcc_ast_node_t *elem = expr->data.init_list.exprs[i];
        sema_analyze_expr(sema, elem);
        
        /* Try to fold constant expressions to integer literals */
        if (elem && elem->kind != AST_INT_LIT && elem->kind != AST_CHAR_LIT) {
            int64_t val;
            if (sema_eval_const_expr(sema, elem, &val)) {
                /* Replace with integer literal */
                elem->kind = AST_INT_LIT;
                elem->data.int_lit.value = (uint64_t)val;
                elem->data.int_lit.suffix = INT_SUFFIX_NONE;
            }
        }
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

        case AST_ALIGNOF_EXPR:
            /* _Alignof(type) yields a size_t constant. If an expression
             * form was used (GNU extension), analyse it first to pin down
             * the type, then drop the expression. */
            {
                mcc_type_t *operand_type = expr->data.alignof_expr.type_arg;
                if (expr->data.alignof_expr.expr_arg)
                    operand_type = sema_analyze_expr(
                        sema, expr->data.alignof_expr.expr_arg);
                while (operand_type && operand_type->kind == TYPE_TYPEDEF)
                    operand_type = operand_type->data.typedef_ref.underlying;
                if (!operand_type || mcc_type_is_function(operand_type) ||
                    !sema_check_complete_type(sema, operand_type,
                                              expr->location)) {
                    mcc_error_at(sema->ctx, expr->location,
                                 "_Alignof requires a complete object type");
                    return NULL;
                }
            }
            expr->type = mcc_type_size_t(sema->types);
            return expr->type;

        case AST_NULL_PTR:
            /* MCC has no distinct nullptr_t yet.  Represent the value as
             * void * for the existing conversion machinery; the AST kind,
             * rather than this surrogate type, retains its null-constant
             * semantics. */
            expr->type = mcc_type_pointer(sema->types,
                                          mcc_type_void(sema->types));
            return expr->type;

        case AST_STMT_EXPR: {
            /* GNU statement expression: ({ ... ; expr; }). Its type is the
             * type of the last expression statement in the compound.
             * Analyse the inner compound so labels/goto/variables are
             * processed normally, then pluck the last expr's type. */
            mcc_ast_node_t *stmt = expr->data.stmt_expr.stmt;
            if (stmt) sema_analyze_stmt(sema, stmt);

            mcc_type_t *last_type = mcc_type_void(sema->types);
            if (stmt && stmt->kind == AST_COMPOUND_STMT &&
                stmt->data.compound_stmt.num_stmts > 0) {
                mcc_ast_node_t *last = stmt->data.compound_stmt.stmts[
                    stmt->data.compound_stmt.num_stmts - 1];
                if (last && last->kind == AST_EXPR_STMT &&
                    last->data.expr_stmt.expr &&
                    last->data.expr_stmt.expr->type) {
                    last_type = last->data.expr_stmt.expr->type;
                }
            }
            expr->type = last_type;
            return last_type;
        }

        case AST_GENERIC_EXPR: {
            /* _Generic(expr, T1: e1, T2: e2, default: ed).
             * Type of the selection = type of the chosen association.
             * We analyse the controlling expression, find a matching
             * association by type, and fall back to default otherwise. */
            mcc_type_t *ctrl = sema_analyze_expr(sema, expr->data.generic_expr.controlling_expr);
            mcc_type_t *result = NULL;
            mcc_ast_node_t *selected = NULL;
            mcc_generic_assoc_t *a = expr->data.generic_expr.associations;
            for (; a; a = a->next) {
                if (a->type && ctrl && mcc_type_is_compatible(a->type, ctrl)) {
                    selected = a->expr;
                    result = sema_analyze_expr(sema, selected);
                    break;
                }
            }
            if (!result && expr->data.generic_expr.default_expr) {
                selected = expr->data.generic_expr.default_expr;
                result = sema_analyze_expr(sema, selected);
            }
            expr->data.generic_expr.selected_expr = selected;
            expr->type = result;
            return result;
        }

        default:
            return NULL;
    }
}
