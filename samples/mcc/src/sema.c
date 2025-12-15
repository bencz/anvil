/*
 * MCC - Micro C Compiler
 * Semantic analysis implementation
 */

#include "mcc.h"

mcc_sema_t *mcc_sema_create(mcc_context_t *ctx)
{
    mcc_sema_t *sema = mcc_alloc(ctx, sizeof(mcc_sema_t));
    sema->ctx = ctx;
    sema->types = mcc_type_context_create(ctx);
    sema->symtab = mcc_symtab_create(ctx, sema->types);
    return sema;
}

void mcc_sema_destroy(mcc_sema_t *sema)
{
    (void)sema; /* Arena allocated */
}

/* Forward declarations */
static mcc_type_t *analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr);
static bool analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt);
static bool analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl);

/* Analyze expression and return its type */
static mcc_type_t *analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_INT_LIT: {
            /* Determine type based on suffix and value */
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
                default:
                    type = mcc_type_int(sema->types);
                    break;
            }
            expr->type = type;
            return type;
        }
        
        case AST_FLOAT_LIT: {
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
        
        case AST_CHAR_LIT:
            expr->type = mcc_type_int(sema->types); /* char literals are int in C */
            return expr->type;
            
        case AST_STRING_LIT:
            /* String literal is array of char */
            expr->type = mcc_type_pointer(sema->types, mcc_type_char(sema->types));
            return expr->type;
            
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = mcc_symtab_lookup(sema->symtab, expr->data.ident_expr.name);
            if (!sym) {
                mcc_error_at(sema->ctx, expr->location, "Undeclared identifier '%s'",
                             expr->data.ident_expr.name);
                return NULL;
            }
            expr->data.ident_expr.symbol = sym;
            sym->is_used = true;
            expr->type = sym->type;
            return expr->type;
        }
        
        case AST_BINARY_EXPR: {
            mcc_type_t *lhs_type = analyze_expr(sema, expr->data.binary_expr.lhs);
            mcc_type_t *rhs_type = analyze_expr(sema, expr->data.binary_expr.rhs);
            
            if (!lhs_type || !rhs_type) return NULL;
            
            mcc_binop_t op = expr->data.binary_expr.op;
            
            /* Assignment operators */
            if (op >= BINOP_ASSIGN && op <= BINOP_RSHIFT_ASSIGN) {
                /* LHS must be lvalue */
                mcc_ast_node_t *lhs = expr->data.binary_expr.lhs;
                if (lhs->kind != AST_IDENT_EXPR && 
                    lhs->kind != AST_SUBSCRIPT_EXPR &&
                    lhs->kind != AST_MEMBER_EXPR &&
                    !(lhs->kind == AST_UNARY_EXPR && lhs->data.unary_expr.op == UNOP_DEREF)) {
                    mcc_error_at(sema->ctx, expr->location, "Expression is not assignable");
                }
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
            
            /* Usual arithmetic conversions */
            expr->type = mcc_type_common(sema->types, lhs_type, rhs_type);
            return expr->type;
        }
        
        case AST_UNARY_EXPR: {
            mcc_type_t *operand_type = analyze_expr(sema, expr->data.unary_expr.operand);
            if (!operand_type) return NULL;
            
            switch (expr->data.unary_expr.op) {
                case UNOP_NEG:
                case UNOP_POS:
                    expr->type = mcc_type_promote(sema->types, operand_type);
                    break;
                case UNOP_NOT:
                    expr->type = mcc_type_int(sema->types);
                    break;
                case UNOP_BIT_NOT:
                    expr->type = mcc_type_promote(sema->types, operand_type);
                    break;
                case UNOP_DEREF:
                    if (!mcc_type_is_pointer(operand_type)) {
                        mcc_error_at(sema->ctx, expr->location,
                                     "Cannot dereference non-pointer type");
                        return NULL;
                    }
                    expr->type = operand_type->data.pointer.pointee;
                    break;
                case UNOP_ADDR:
                    expr->type = mcc_type_pointer(sema->types, operand_type);
                    break;
                case UNOP_PRE_INC:
                case UNOP_PRE_DEC:
                case UNOP_POST_INC:
                case UNOP_POST_DEC:
                    expr->type = operand_type;
                    break;
                default:
                    expr->type = operand_type;
                    break;
            }
            return expr->type;
        }
        
        case AST_TERNARY_EXPR: {
            mcc_type_t *cond_type = analyze_expr(sema, expr->data.ternary_expr.cond);
            mcc_type_t *then_type = analyze_expr(sema, expr->data.ternary_expr.then_expr);
            mcc_type_t *else_type = analyze_expr(sema, expr->data.ternary_expr.else_expr);
            
            if (!cond_type || !then_type || !else_type) return NULL;
            
            if (!mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Condition must be scalar type");
            }
            
            expr->type = mcc_type_common(sema->types, then_type, else_type);
            return expr->type;
        }
        
        case AST_CALL_EXPR: {
            mcc_type_t *func_type = analyze_expr(sema, expr->data.call_expr.func);
            if (!func_type) return NULL;
            
            /* Handle pointer to function */
            if (mcc_type_is_pointer(func_type)) {
                func_type = func_type->data.pointer.pointee;
            }
            
            if (!mcc_type_is_function(func_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Called object is not a function");
                return NULL;
            }
            
            /* Check argument count */
            int expected = func_type->data.function.num_params;
            int actual = (int)expr->data.call_expr.num_args;
            
            if (!func_type->data.function.is_variadic && actual != expected) {
                mcc_error_at(sema->ctx, expr->location,
                             "Function expects %d arguments, got %d", expected, actual);
            } else if (func_type->data.function.is_variadic && actual < expected) {
                mcc_error_at(sema->ctx, expr->location,
                             "Function expects at least %d arguments, got %d", expected, actual);
            }
            
            /* Analyze arguments */
            for (size_t i = 0; i < expr->data.call_expr.num_args; i++) {
                analyze_expr(sema, expr->data.call_expr.args[i]);
            }
            
            expr->type = func_type->data.function.return_type;
            return expr->type;
        }
        
        case AST_SUBSCRIPT_EXPR: {
            mcc_type_t *array_type = analyze_expr(sema, expr->data.subscript_expr.array);
            mcc_type_t *index_type = analyze_expr(sema, expr->data.subscript_expr.index);
            
            if (!array_type || !index_type) return NULL;
            
            /* Array decays to pointer */
            if (mcc_type_is_array(array_type)) {
                array_type = mcc_type_pointer(sema->types, array_type->data.array.element);
            }
            
            if (!mcc_type_is_pointer(array_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Subscripted value is not an array or pointer");
                return NULL;
            }
            
            if (!mcc_type_is_integer(index_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Array subscript is not an integer");
            }
            
            expr->type = array_type->data.pointer.pointee;
            return expr->type;
        }
        
        case AST_MEMBER_EXPR: {
            mcc_type_t *obj_type = analyze_expr(sema, expr->data.member_expr.object);
            if (!obj_type) return NULL;
            
            /* Handle arrow operator */
            if (expr->data.member_expr.is_arrow) {
                if (!mcc_type_is_pointer(obj_type)) {
                    mcc_error_at(sema->ctx, expr->location,
                                 "Member reference type is not a pointer");
                    return NULL;
                }
                obj_type = obj_type->data.pointer.pointee;
            }
            
            if (!mcc_type_is_record(obj_type)) {
                mcc_error_at(sema->ctx, expr->location,
                             "Member reference base type is not a struct or union");
                return NULL;
            }
            
            mcc_struct_field_t *field = mcc_type_find_field(obj_type, expr->data.member_expr.member);
            if (!field) {
                mcc_error_at(sema->ctx, expr->location,
                             "No member named '%s'", expr->data.member_expr.member);
                return NULL;
            }
            
            expr->type = field->type;
            return expr->type;
        }
        
        case AST_CAST_EXPR: {
            analyze_expr(sema, expr->data.cast_expr.expr);
            expr->type = expr->data.cast_expr.target_type;
            return expr->type;
        }
        
        case AST_SIZEOF_EXPR: {
            if (expr->data.sizeof_expr.type_arg) {
                /* sizeof(type) */
            } else if (expr->data.sizeof_expr.expr_arg) {
                analyze_expr(sema, expr->data.sizeof_expr.expr_arg);
            }
            expr->type = mcc_type_ulong(sema->types); /* size_t */
            return expr->type;
        }
        
        case AST_COMMA_EXPR: {
            analyze_expr(sema, expr->data.comma_expr.left);
            mcc_type_t *right_type = analyze_expr(sema, expr->data.comma_expr.right);
            expr->type = right_type;
            return expr->type;
        }
        
        case AST_INIT_LIST: {
            for (size_t i = 0; i < expr->data.init_list.num_exprs; i++) {
                analyze_expr(sema, expr->data.init_list.exprs[i]);
            }
            return NULL; /* Init list type depends on context */
        }
        
        default:
            return NULL;
    }
}

/* Analyze statement */
static bool analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    if (!stmt) return true;
    
    switch (stmt->kind) {
        case AST_COMPOUND_STMT:
            mcc_symtab_push_scope(sema->symtab);
            for (size_t i = 0; i < stmt->data.compound_stmt.num_stmts; i++) {
                mcc_ast_node_t *s = stmt->data.compound_stmt.stmts[i];
                if (s->kind == AST_VAR_DECL || s->kind == AST_FUNC_DECL) {
                    analyze_decl(sema, s);
                } else {
                    analyze_stmt(sema, s);
                }
            }
            mcc_symtab_pop_scope(sema->symtab);
            return true;
            
        case AST_EXPR_STMT:
            if (stmt->data.expr_stmt.expr) {
                analyze_expr(sema, stmt->data.expr_stmt.expr);
            }
            return true;
            
        case AST_IF_STMT: {
            mcc_type_t *cond_type = analyze_expr(sema, stmt->data.if_stmt.cond);
            if (cond_type && !mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "If condition must be scalar type");
            }
            analyze_stmt(sema, stmt->data.if_stmt.then_stmt);
            if (stmt->data.if_stmt.else_stmt) {
                analyze_stmt(sema, stmt->data.if_stmt.else_stmt);
            }
            return true;
        }
        
        case AST_WHILE_STMT: {
            mcc_type_t *cond_type = analyze_expr(sema, stmt->data.while_stmt.cond);
            if (cond_type && !mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "While condition must be scalar type");
            }
            sema->loop_depth++;
            analyze_stmt(sema, stmt->data.while_stmt.body);
            sema->loop_depth--;
            return true;
        }
        
        case AST_DO_STMT: {
            sema->loop_depth++;
            analyze_stmt(sema, stmt->data.do_stmt.body);
            sema->loop_depth--;
            mcc_type_t *cond_type = analyze_expr(sema, stmt->data.do_stmt.cond);
            if (cond_type && !mcc_type_is_scalar(cond_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Do-while condition must be scalar type");
            }
            return true;
        }
        
        case AST_FOR_STMT: {
            mcc_symtab_push_scope(sema->symtab);
            if (stmt->data.for_stmt.init) {
                analyze_expr(sema, stmt->data.for_stmt.init);
            }
            if (stmt->data.for_stmt.cond) {
                mcc_type_t *cond_type = analyze_expr(sema, stmt->data.for_stmt.cond);
                if (cond_type && !mcc_type_is_scalar(cond_type)) {
                    mcc_error_at(sema->ctx, stmt->location,
                                 "For condition must be scalar type");
                }
            }
            if (stmt->data.for_stmt.incr) {
                analyze_expr(sema, stmt->data.for_stmt.incr);
            }
            sema->loop_depth++;
            analyze_stmt(sema, stmt->data.for_stmt.body);
            sema->loop_depth--;
            mcc_symtab_pop_scope(sema->symtab);
            return true;
        }
        
        case AST_SWITCH_STMT: {
            mcc_type_t *expr_type = analyze_expr(sema, stmt->data.switch_stmt.expr);
            if (expr_type && !mcc_type_is_integer(expr_type)) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Switch expression must be integer type");
            }
            sema->switch_depth++;
            analyze_stmt(sema, stmt->data.switch_stmt.body);
            sema->switch_depth--;
            return true;
        }
        
        case AST_CASE_STMT:
            if (sema->switch_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Case statement outside of switch");
            }
            analyze_expr(sema, stmt->data.case_stmt.expr);
            analyze_stmt(sema, stmt->data.case_stmt.stmt);
            return true;
            
        case AST_DEFAULT_STMT:
            if (sema->switch_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Default statement outside of switch");
            }
            analyze_stmt(sema, stmt->data.default_stmt.stmt);
            return true;
            
        case AST_BREAK_STMT:
            if (sema->loop_depth == 0 && sema->switch_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Break statement outside of loop or switch");
            }
            return true;
            
        case AST_CONTINUE_STMT:
            if (sema->loop_depth == 0) {
                mcc_error_at(sema->ctx, stmt->location,
                             "Continue statement outside of loop");
            }
            return true;
            
        case AST_RETURN_STMT: {
            mcc_type_t *expr_type = NULL;
            if (stmt->data.return_stmt.expr) {
                expr_type = analyze_expr(sema, stmt->data.return_stmt.expr);
            }
            
            if (sema->current_return_type) {
                if (mcc_type_is_void(sema->current_return_type)) {
                    if (expr_type) {
                        mcc_error_at(sema->ctx, stmt->location,
                                     "Void function should not return a value");
                    }
                } else {
                    if (!expr_type) {
                        mcc_error_at(sema->ctx, stmt->location,
                                     "Non-void function should return a value");
                    }
                }
            }
            return true;
        }
        
        case AST_GOTO_STMT: {
            mcc_symbol_t *label = mcc_symtab_lookup_label(sema->symtab, stmt->data.goto_stmt.label);
            if (!label) {
                /* Forward reference - will be checked at end of function */
            }
            return true;
        }
        
        case AST_LABEL_STMT:
            mcc_symtab_define_label(sema->symtab, stmt->data.label_stmt.label, stmt->location);
            analyze_stmt(sema, stmt->data.label_stmt.stmt);
            return true;
            
        case AST_NULL_STMT:
            return true;
            
        default:
            return true;
    }
}

/* Analyze declaration */
static bool analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    if (!decl) return true;
    
    switch (decl->kind) {
        case AST_FUNC_DECL: {
            /* Create function type */
            mcc_func_param_t *params = NULL;
            mcc_func_param_t *param_tail = NULL;
            
            for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
                mcc_ast_node_t *p = decl->data.func_decl.params[i];
                mcc_func_param_t *param = mcc_alloc(sema->ctx, sizeof(mcc_func_param_t));
                param->name = p->data.param_decl.name;
                param->type = p->data.param_decl.param_type;
                
                if (!params) params = param;
                if (param_tail) param_tail->next = param;
                param_tail = param;
            }
            
            mcc_type_t *func_type = mcc_type_function(sema->types,
                decl->data.func_decl.func_type,
                params,
                (int)decl->data.func_decl.num_params,
                false);
            
            /* Define function symbol */
            mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
                decl->data.func_decl.name,
                SYM_FUNC,
                func_type,
                decl->location);
            
            if (sym && decl->data.func_decl.is_definition) {
                sym->is_defined = true;
                sym->ast_node = decl;
                
                /* Analyze function body */
                sema->current_func = sym;
                sema->current_return_type = decl->data.func_decl.func_type;
                
                mcc_symtab_push_function_scope(sema->symtab);
                
                /* Define parameters */
                for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
                    mcc_ast_node_t *p = decl->data.func_decl.params[i];
                    if (p->data.param_decl.name) {
                        mcc_symtab_define(sema->symtab,
                            p->data.param_decl.name,
                            SYM_PARAM,
                            p->data.param_decl.param_type,
                            p->location);
                    }
                }
                
                analyze_stmt(sema, decl->data.func_decl.body);
                
                mcc_symtab_pop_scope(sema->symtab);
                
                sema->current_func = NULL;
                sema->current_return_type = NULL;
            }
            return true;
        }
        
        case AST_VAR_DECL: {
            mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
                decl->data.var_decl.name,
                SYM_VAR,
                decl->data.var_decl.var_type,
                decl->location);
            
            if (sym && decl->data.var_decl.init) {
                mcc_type_t *init_type = analyze_expr(sema, decl->data.var_decl.init);
                (void)init_type; /* TODO: Check type compatibility */
            }
            return true;
        }
        
        default:
            return true;
    }
}

/* Main analysis entry point */
bool mcc_sema_analyze(mcc_sema_t *sema, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        analyze_decl(sema, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(sema->ctx);
}

bool mcc_sema_analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    return analyze_decl(sema, decl);
}

bool mcc_sema_analyze_stmt(mcc_sema_t *sema, mcc_ast_node_t *stmt)
{
    return analyze_stmt(sema, stmt);
}

mcc_type_t *mcc_sema_analyze_expr(mcc_sema_t *sema, mcc_ast_node_t *expr)
{
    return analyze_expr(sema, expr);
}

bool mcc_sema_check_assignment(mcc_sema_t *sema, mcc_type_t *lhs, mcc_type_t *rhs,
                                mcc_location_t loc)
{
    if (!lhs || !rhs) return false;
    
    /* Same type */
    if (mcc_type_is_same(lhs, rhs)) return true;
    
    /* Arithmetic types are compatible */
    if (mcc_type_is_arithmetic(lhs) && mcc_type_is_arithmetic(rhs)) return true;
    
    /* Pointer to void is compatible with any pointer */
    if (mcc_type_is_pointer(lhs) && mcc_type_is_pointer(rhs)) {
        if (mcc_type_is_void(lhs->data.pointer.pointee) ||
            mcc_type_is_void(rhs->data.pointer.pointee)) {
            return true;
        }
    }
    
    /* Null pointer constant */
    if (mcc_type_is_pointer(lhs) && mcc_type_is_integer(rhs)) {
        return true; /* Allow, but could warn */
    }
    
    mcc_warning_at(sema->ctx, loc, "Incompatible types in assignment");
    return true;
}

bool mcc_sema_check_call(mcc_sema_t *sema, mcc_type_t *func_type,
                          mcc_ast_node_t **args, size_t num_args, mcc_location_t loc)
{
    (void)sema;
    (void)func_type;
    (void)args;
    (void)num_args;
    (void)loc;
    return true;
}

bool mcc_sema_check_return(mcc_sema_t *sema, mcc_type_t *expr_type, mcc_location_t loc)
{
    (void)sema;
    (void)expr_type;
    (void)loc;
    return true;
}

mcc_ast_node_t *mcc_sema_implicit_cast(mcc_sema_t *sema, mcc_ast_node_t *expr,
                                        mcc_type_t *target)
{
    if (!expr || !target) return expr;
    if (mcc_type_is_same(expr->type, target)) return expr;
    
    mcc_ast_node_t *cast = mcc_ast_create(sema->ctx, AST_CAST_EXPR, expr->location);
    cast->data.cast_expr.target_type = target;
    cast->data.cast_expr.expr = expr;
    cast->type = target;
    return cast;
}

bool mcc_sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr, int64_t *result)
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
            if (!mcc_sema_eval_const_expr(sema, expr->data.binary_expr.lhs, &lhs) ||
                !mcc_sema_eval_const_expr(sema, expr->data.binary_expr.rhs, &rhs)) {
                return false;
            }
            
            switch (expr->data.binary_expr.op) {
                case BINOP_ADD: *result = lhs + rhs; break;
                case BINOP_SUB: *result = lhs - rhs; break;
                case BINOP_MUL: *result = lhs * rhs; break;
                case BINOP_DIV: *result = rhs ? lhs / rhs : 0; break;
                case BINOP_MOD: *result = rhs ? lhs % rhs : 0; break;
                case BINOP_LSHIFT: *result = lhs << rhs; break;
                case BINOP_RSHIFT: *result = lhs >> rhs; break;
                case BINOP_BIT_AND: *result = lhs & rhs; break;
                case BINOP_BIT_OR: *result = lhs | rhs; break;
                case BINOP_BIT_XOR: *result = lhs ^ rhs; break;
                case BINOP_EQ: *result = lhs == rhs; break;
                case BINOP_NE: *result = lhs != rhs; break;
                case BINOP_LT: *result = lhs < rhs; break;
                case BINOP_GT: *result = lhs > rhs; break;
                case BINOP_LE: *result = lhs <= rhs; break;
                case BINOP_GE: *result = lhs >= rhs; break;
                case BINOP_AND: *result = lhs && rhs; break;
                case BINOP_OR: *result = lhs || rhs; break;
                default: return false;
            }
            return true;
        }
        
        case AST_UNARY_EXPR: {
            int64_t val;
            if (!mcc_sema_eval_const_expr(sema, expr->data.unary_expr.operand, &val)) {
                return false;
            }
            
            switch (expr->data.unary_expr.op) {
                case UNOP_NEG: *result = -val; break;
                case UNOP_POS: *result = val; break;
                case UNOP_NOT: *result = !val; break;
                case UNOP_BIT_NOT: *result = ~val; break;
                default: return false;
            }
            return true;
        }
        
        case AST_TERNARY_EXPR: {
            int64_t cond;
            if (!mcc_sema_eval_const_expr(sema, expr->data.ternary_expr.cond, &cond)) {
                return false;
            }
            return mcc_sema_eval_const_expr(sema,
                cond ? expr->data.ternary_expr.then_expr : expr->data.ternary_expr.else_expr,
                result);
        }
        
        default:
            return false;
    }
}
