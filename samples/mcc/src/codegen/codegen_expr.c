/*
 * MCC - Micro C Compiler
 * Code Generator - Expression Generation
 * 
 * This file handles code generation for all expression types.
 */

#include "codegen_internal.h"

/* Generate code for expression (returns value) */
anvil_value_t *codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_INT_LIT: {
            /* Use appropriate type based on suffix */
            switch (expr->data.int_lit.suffix) {
                case INT_SUFFIX_LL:
                case INT_SUFFIX_ULL:
                    return anvil_const_i64(cg->anvil_ctx, (int64_t)expr->data.int_lit.value);
                case INT_SUFFIX_L:
                case INT_SUFFIX_UL:
                    /* On 64-bit systems, long is 64-bit */
                    return anvil_const_i64(cg->anvil_ctx, (int64_t)expr->data.int_lit.value);
                default:
                    return anvil_const_i32(cg->anvil_ctx, (int32_t)expr->data.int_lit.value);
            }
        }
            
        case AST_FLOAT_LIT:
            if (expr->data.float_lit.suffix == FLOAT_SUFFIX_F) {
                return anvil_const_f32(cg->anvil_ctx, (float)expr->data.float_lit.value);
            }
            return anvil_const_f64(cg->anvil_ctx, expr->data.float_lit.value);
            
        case AST_CHAR_LIT:
            return anvil_const_i8(cg->anvil_ctx, (int8_t)expr->data.char_lit.value);
            
        case AST_STRING_LIT:
            return codegen_get_string_literal(cg, expr->data.string_lit.value);
            
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = expr->data.ident_expr.symbol;
            const char *name = expr->data.ident_expr.name;
            
            /* C99: __func__ predefined identifier */
            if (expr->data.ident_expr.is_func_name) {
                /* Get current function name as string literal */
                const char *func_name = cg->current_func_name ? cg->current_func_name : "";
                return codegen_get_string_literal(cg, func_name);
            }
            
            /* Try to find local by name */
            anvil_value_t *ptr = codegen_find_local(cg, name);
            
            if (ptr) {
                /* For arrays, the pointer IS the value (array decays to pointer) */
                if (sym && sym->type && sym->type->kind == TYPE_ARRAY) {
                    return ptr;
                }
                /* Load from local variable */
                anvil_type_t *type = sym ? codegen_type(cg, sym->type) 
                                         : anvil_type_i32(cg->anvil_ctx);
                return anvil_build_load(cg->anvil_ctx, type, ptr, "load");
            }
            
            /* For functions, get or declare the function and return its value */
            if (sym && sym->kind == SYM_FUNC) {
                anvil_func_t *func = codegen_get_or_declare_func(cg, sym);
                return anvil_func_get_value(func);
            }
            
            /* Global variable - create global reference and load */
            if (sym && sym->kind == SYM_VAR) {
                anvil_type_t *type = codegen_type(cg, sym->type);
                anvil_value_t *global = codegen_get_or_add_global(cg, name, type);
                return anvil_build_load(cg->anvil_ctx, type, global, "gload");
            }
            
            return NULL;
        }
        
        case AST_BINARY_EXPR: {
            mcc_binop_t op = expr->data.binary_expr.op;
            
            /* Handle assignment */
            if (op >= BINOP_ASSIGN && op <= BINOP_RSHIFT_ASSIGN) {
                anvil_value_t *lhs_ptr = codegen_lvalue(cg, expr->data.binary_expr.lhs);
                anvil_value_t *rhs = codegen_expr(cg, expr->data.binary_expr.rhs);
                
                if (!lhs_ptr || !rhs) return NULL;
                
                anvil_value_t *result = rhs;
                
                /* Compound assignment */
                if (op != BINOP_ASSIGN) {
                    anvil_type_t *type = codegen_type(cg, expr->data.binary_expr.lhs->type);
                    anvil_value_t *lhs = anvil_build_load(cg->anvil_ctx, type, lhs_ptr, "lhs");
                    
                    switch (op) {
                        case BINOP_ADD_ASSIGN:
                            result = anvil_build_add(cg->anvil_ctx, lhs, rhs, "add");
                            break;
                        case BINOP_SUB_ASSIGN:
                            result = anvil_build_sub(cg->anvil_ctx, lhs, rhs, "sub");
                            break;
                        case BINOP_MUL_ASSIGN:
                            result = anvil_build_mul(cg->anvil_ctx, lhs, rhs, "mul");
                            break;
                        case BINOP_DIV_ASSIGN:
                            result = anvil_build_sdiv(cg->anvil_ctx, lhs, rhs, "div");
                            break;
                        case BINOP_MOD_ASSIGN:
                            result = anvil_build_smod(cg->anvil_ctx, lhs, rhs, "mod");
                            break;
                        case BINOP_AND_ASSIGN:
                            result = anvil_build_and(cg->anvil_ctx, lhs, rhs, "and");
                            break;
                        case BINOP_OR_ASSIGN:
                            result = anvil_build_or(cg->anvil_ctx, lhs, rhs, "or");
                            break;
                        case BINOP_XOR_ASSIGN:
                            result = anvil_build_xor(cg->anvil_ctx, lhs, rhs, "xor");
                            break;
                        case BINOP_LSHIFT_ASSIGN:
                            result = anvil_build_shl(cg->anvil_ctx, lhs, rhs, "shl");
                            break;
                        case BINOP_RSHIFT_ASSIGN:
                            result = anvil_build_shr(cg->anvil_ctx, lhs, rhs, "shr");
                            break;
                        default:
                            break;
                    }
                }
                
                anvil_build_store(cg->anvil_ctx, result, lhs_ptr);
                return result;
            }
            
            /* Handle short-circuit logical operators */
            if (op == BINOP_AND || op == BINOP_OR) {
                /* Use a temporary variable instead of PHI (simpler codegen) */
                anvil_type_t *i32_type = anvil_type_i32(cg->anvil_ctx);
                anvil_value_t *result_ptr = anvil_build_alloca(cg->anvil_ctx, i32_type, "land.result");
                
                anvil_value_t *lhs = codegen_expr(cg, expr->data.binary_expr.lhs);
                
                int id = cg->label_counter++;
                char rhs_name[32], end_name[32];
                snprintf(rhs_name, sizeof(rhs_name), "land%d.rhs", id);
                snprintf(end_name, sizeof(end_name), "land%d.end", id);
                
                anvil_block_t *rhs_block = anvil_block_create(cg->current_func, rhs_name);
                anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
                
                /* Compare LHS to zero */
                anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
                anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                anvil_value_t *lhs_bool = anvil_build_cmp_ne(cg->anvil_ctx, lhs, zero, "cmp");
                
                if (op == BINOP_AND) {
                    /* AND: if LHS is false, result is 0; else evaluate RHS */
                    anvil_build_store(cg->anvil_ctx, zero, result_ptr);
                    anvil_build_br_cond(cg->anvil_ctx, lhs_bool, rhs_block, end_block);
                } else {
                    /* OR: if LHS is true, result is 1; else evaluate RHS */
                    anvil_build_store(cg->anvil_ctx, one, result_ptr);
                    anvil_build_br_cond(cg->anvil_ctx, lhs_bool, end_block, rhs_block);
                }
                
                /* RHS block */
                codegen_set_current_block(cg, rhs_block);
                anvil_value_t *rhs = codegen_expr(cg, expr->data.binary_expr.rhs);
                anvil_value_t *rhs_bool = anvil_build_cmp_ne(cg->anvil_ctx, rhs, zero, "cmp");
                anvil_build_store(cg->anvil_ctx, rhs_bool, result_ptr);
                anvil_build_br(cg->anvil_ctx, end_block);
                
                /* End block - load result */
                codegen_set_current_block(cg, end_block);
                return anvil_build_load(cg->anvil_ctx, i32_type, result_ptr, "land.val");
            }
            
            /* Regular binary operators */
            anvil_value_t *lhs = codegen_expr(cg, expr->data.binary_expr.lhs);
            anvil_value_t *rhs = codegen_expr(cg, expr->data.binary_expr.rhs);
            
            if (!lhs || !rhs) return NULL;
            
            /* Handle pointer arithmetic */
            mcc_type_t *lhs_type = expr->data.binary_expr.lhs->type;
            mcc_type_t *rhs_type = expr->data.binary_expr.rhs->type;
            
            if ((op == BINOP_ADD || op == BINOP_SUB) && lhs_type && lhs_type->kind == TYPE_POINTER) {
                /* ptr + int or ptr - int: scale by element size */
                mcc_type_t *pointee = lhs_type->data.pointer.pointee;
                int elem_size = pointee ? codegen_sizeof(cg, pointee) : 1;
                if (elem_size > 1) {
                    anvil_value_t *scale = anvil_const_i64(cg->anvil_ctx, elem_size);
                    rhs = anvil_build_mul(cg->anvil_ctx, rhs, scale, "scale");
                }
                if (op == BINOP_ADD) {
                    return anvil_build_add(cg->anvil_ctx, lhs, rhs, "ptr.add");
                } else {
                    return anvil_build_sub(cg->anvil_ctx, lhs, rhs, "ptr.sub");
                }
            }
            
            if (op == BINOP_ADD && rhs_type && rhs_type->kind == TYPE_POINTER) {
                /* int + ptr: scale lhs by element size */
                mcc_type_t *pointee = rhs_type->data.pointer.pointee;
                int elem_size = pointee ? codegen_sizeof(cg, pointee) : 1;
                if (elem_size > 1) {
                    anvil_value_t *scale = anvil_const_i64(cg->anvil_ctx, elem_size);
                    lhs = anvil_build_mul(cg->anvil_ctx, lhs, scale, "scale");
                }
                return anvil_build_add(cg->anvil_ctx, lhs, rhs, "ptr.add");
            }
            
            switch (op) {
                case BINOP_ADD:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fadd(cg->anvil_ctx, lhs, rhs, "fadd");
                    }
                    return anvil_build_add(cg->anvil_ctx, lhs, rhs, "add");
                case BINOP_SUB:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fsub(cg->anvil_ctx, lhs, rhs, "fsub");
                    }
                    return anvil_build_sub(cg->anvil_ctx, lhs, rhs, "sub");
                case BINOP_MUL:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fmul(cg->anvil_ctx, lhs, rhs, "fmul");
                    }
                    return anvil_build_mul(cg->anvil_ctx, lhs, rhs, "mul");
                case BINOP_DIV:
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fdiv(cg->anvil_ctx, lhs, rhs, "fdiv");
                    }
                    if (expr->type && expr->type->is_unsigned) {
                        return anvil_build_udiv(cg->anvil_ctx, lhs, rhs, "udiv");
                    }
                    return anvil_build_sdiv(cg->anvil_ctx, lhs, rhs, "sdiv");
                case BINOP_MOD:
                    if (expr->type && expr->type->is_unsigned) {
                        return anvil_build_umod(cg->anvil_ctx, lhs, rhs, "umod");
                    }
                    return anvil_build_smod(cg->anvil_ctx, lhs, rhs, "smod");
                case BINOP_BIT_AND:
                    return anvil_build_and(cg->anvil_ctx, lhs, rhs, "and");
                case BINOP_BIT_OR:
                    return anvil_build_or(cg->anvil_ctx, lhs, rhs, "or");
                case BINOP_BIT_XOR:
                    return anvil_build_xor(cg->anvil_ctx, lhs, rhs, "xor");
                case BINOP_LSHIFT:
                    return anvil_build_shl(cg->anvil_ctx, lhs, rhs, "shl");
                case BINOP_RSHIFT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_shr(cg->anvil_ctx, lhs, rhs, "shr");
                    }
                    return anvil_build_sar(cg->anvil_ctx, lhs, rhs, "sar");
                case BINOP_EQ:
                    return anvil_build_cmp_eq(cg->anvil_ctx, lhs, rhs, "eq");
                case BINOP_NE:
                    return anvil_build_cmp_ne(cg->anvil_ctx, lhs, rhs, "ne");
                case BINOP_LT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ult(cg->anvil_ctx, lhs, rhs, "ult");
                    }
                    return anvil_build_cmp_lt(cg->anvil_ctx, lhs, rhs, "lt");
                case BINOP_GT:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ugt(cg->anvil_ctx, lhs, rhs, "ugt");
                    }
                    return anvil_build_cmp_gt(cg->anvil_ctx, lhs, rhs, "gt");
                case BINOP_LE:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_ule(cg->anvil_ctx, lhs, rhs, "ule");
                    }
                    return anvil_build_cmp_le(cg->anvil_ctx, lhs, rhs, "le");
                case BINOP_GE:
                    if (expr->data.binary_expr.lhs->type &&
                        expr->data.binary_expr.lhs->type->is_unsigned) {
                        return anvil_build_cmp_uge(cg->anvil_ctx, lhs, rhs, "uge");
                    }
                    return anvil_build_cmp_ge(cg->anvil_ctx, lhs, rhs, "ge");
                default:
                    return NULL;
            }
        }
        
        case AST_UNARY_EXPR: {
            mcc_unop_t op = expr->data.unary_expr.op;
            
            switch (op) {
                case UNOP_NEG: {
                    anvil_value_t *val = codegen_expr(cg, expr->data.unary_expr.operand);
                    if (expr->type && mcc_type_is_floating(expr->type)) {
                        return anvil_build_fneg(cg->anvil_ctx, val, "fneg");
                    }
                    return anvil_build_neg(cg->anvil_ctx, val, "neg");
                }
                case UNOP_POS:
                    return codegen_expr(cg, expr->data.unary_expr.operand);
                case UNOP_NOT: {
                    anvil_value_t *val = codegen_expr(cg, expr->data.unary_expr.operand);
                    anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
                    return anvil_build_cmp_eq(cg->anvil_ctx, val, zero, "not");
                }
                case UNOP_BIT_NOT: {
                    anvil_value_t *val = codegen_expr(cg, expr->data.unary_expr.operand);
                    return anvil_build_not(cg->anvil_ctx, val, "bitnot");
                }
                case UNOP_DEREF: {
                    anvil_value_t *ptr = codegen_expr(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = codegen_type(cg, expr->type);
                    return anvil_build_load(cg->anvil_ctx, type, ptr, "deref");
                }
                case UNOP_ADDR:
                    return codegen_lvalue(cg, expr->data.unary_expr.operand);
                case UNOP_PRE_INC:
                case UNOP_PRE_DEC: {
                    anvil_value_t *ptr = codegen_lvalue(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = codegen_type(cg, expr->data.unary_expr.operand->type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                    anvil_value_t *result = (op == UNOP_PRE_INC) ?
                        anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                        anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    anvil_build_store(cg->anvil_ctx, result, ptr);
                    return result;
                }
                case UNOP_POST_INC:
                case UNOP_POST_DEC: {
                    anvil_value_t *ptr = codegen_lvalue(cg, expr->data.unary_expr.operand);
                    anvil_type_t *type = codegen_type(cg, expr->data.unary_expr.operand->type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                    anvil_value_t *result = (op == UNOP_POST_INC) ?
                        anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                        anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    anvil_build_store(cg->anvil_ctx, result, ptr);
                    return val; /* Return original value */
                }
                default:
                    return NULL;
            }
        }
        
        case AST_TERNARY_EXPR: {
            /* Use a temporary variable instead of PHI (simpler codegen) */
            anvil_type_t *type = codegen_type(cg, expr->type);
            if (!type) type = anvil_type_i32(cg->anvil_ctx);
            anvil_value_t *result_ptr = anvil_build_alloca(cg->anvil_ctx, type, "ternary.result");
            
            anvil_value_t *cond = codegen_expr(cg, expr->data.ternary_expr.cond);
            
            int id = cg->label_counter++;
            char then_name[32], else_name[32], end_name[32];
            snprintf(then_name, sizeof(then_name), "ternary%d.then", id);
            snprintf(else_name, sizeof(else_name), "ternary%d.else", id);
            snprintf(end_name, sizeof(end_name), "ternary%d.end", id);
            
            anvil_block_t *then_block = anvil_block_create(cg->current_func, then_name);
            anvil_block_t *else_block = anvil_block_create(cg->current_func, else_name);
            anvil_block_t *end_block = anvil_block_create(cg->current_func, end_name);
            
            /* Compare to zero */
            anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
            anvil_value_t *cond_bool = anvil_build_cmp_ne(cg->anvil_ctx, cond, zero, "cond");
            anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block, else_block);
            
            /* Then block */
            codegen_set_current_block(cg, then_block);
            anvil_value_t *then_val = codegen_expr(cg, expr->data.ternary_expr.then_expr);
            anvil_build_store(cg->anvil_ctx, then_val, result_ptr);
            anvil_build_br(cg->anvil_ctx, end_block);
            
            /* Else block */
            codegen_set_current_block(cg, else_block);
            anvil_value_t *else_val = codegen_expr(cg, expr->data.ternary_expr.else_expr);
            anvil_build_store(cg->anvil_ctx, else_val, result_ptr);
            anvil_build_br(cg->anvil_ctx, end_block);
            
            /* End block - load result */
            codegen_set_current_block(cg, end_block);
            return anvil_build_load(cg->anvil_ctx, type, result_ptr, "ternary.val");
        }
        
        case AST_CALL_EXPR: {
            anvil_value_t *func = codegen_expr(cg, expr->data.call_expr.func);
            
            size_t num_args = expr->data.call_expr.num_args;
            anvil_value_t **args = NULL;
            if (num_args > 0) {
                args = mcc_alloc(cg->mcc_ctx, num_args * sizeof(anvil_value_t*));
                for (size_t i = 0; i < num_args; i++) {
                    args[i] = codegen_expr(cg, expr->data.call_expr.args[i]);
                }
            }
            
            anvil_type_t *func_type = codegen_type(cg, expr->data.call_expr.func->type);
            return anvil_build_call(cg->anvil_ctx, func_type, func, args, num_args, "call");
        }
        
        case AST_SUBSCRIPT_EXPR: {
            anvil_value_t *ptr = codegen_lvalue(cg, expr);
            anvil_type_t *type = codegen_type(cg, expr->type);
            return anvil_build_load(cg->anvil_ctx, type, ptr, "subscript");
        }
        
        case AST_MEMBER_EXPR: {
            anvil_value_t *ptr = codegen_lvalue(cg, expr);
            anvil_type_t *type = codegen_type(cg, expr->type);
            return anvil_build_load(cg->anvil_ctx, type, ptr, "member");
        }
        
        case AST_CAST_EXPR: {
            anvil_value_t *val = codegen_expr(cg, expr->data.cast_expr.expr);
            mcc_type_t *from = expr->data.cast_expr.expr->type;
            mcc_type_t *to = expr->data.cast_expr.target_type;
            
            if (!from || !to) return val;
            
            /* Integer to integer */
            if (mcc_type_is_integer(from) && mcc_type_is_integer(to)) {
                if (from->size < to->size) {
                    if (from->is_unsigned) {
                        return anvil_build_zext(cg->anvil_ctx, val,
                            codegen_type(cg, to), "zext");
                    }
                    return anvil_build_sext(cg->anvil_ctx, val,
                        codegen_type(cg, to), "sext");
                } else if (from->size > to->size) {
                    return anvil_build_trunc(cg->anvil_ctx, val,
                        codegen_type(cg, to), "trunc");
                }
            }
            
            /* Integer to float */
            if (mcc_type_is_integer(from) && mcc_type_is_floating(to)) {
                if (from->is_unsigned) {
                    return anvil_build_uitofp(cg->anvil_ctx, val,
                        codegen_type(cg, to), "uitofp");
                }
                return anvil_build_sitofp(cg->anvil_ctx, val,
                    codegen_type(cg, to), "sitofp");
            }
            
            /* Float to integer */
            if (mcc_type_is_floating(from) && mcc_type_is_integer(to)) {
                if (to->is_unsigned) {
                    return anvil_build_fptoui(cg->anvil_ctx, val,
                        codegen_type(cg, to), "fptoui");
                }
                return anvil_build_fptosi(cg->anvil_ctx, val,
                    codegen_type(cg, to), "fptosi");
            }
            
            /* Pointer casts */
            if (mcc_type_is_pointer(from) || mcc_type_is_pointer(to)) {
                return anvil_build_bitcast(cg->anvil_ctx, val,
                    codegen_type(cg, to), "bitcast");
            }
            
            return val;
        }
        
        case AST_SIZEOF_EXPR: {
            size_t size;
            if (expr->data.sizeof_expr.type_arg) {
                size = codegen_sizeof(cg, expr->data.sizeof_expr.type_arg);
            } else if (expr->data.sizeof_expr.expr_arg) {
                size = codegen_sizeof(cg, expr->data.sizeof_expr.expr_arg->type);
            } else {
                size = 0;
            }
            return anvil_const_i32(cg->anvil_ctx, (int32_t)size);
        }
        
        case AST_COMMA_EXPR:
            codegen_expr(cg, expr->data.comma_expr.left);
            return codegen_expr(cg, expr->data.comma_expr.right);
            
        default:
            return NULL;
    }
}

/* Generate code for lvalue (returns pointer) */
anvil_value_t *codegen_lvalue(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_IDENT_EXPR: {
            const char *name = expr->data.ident_expr.name;
            mcc_symbol_t *sym = expr->data.ident_expr.symbol;
            
            anvil_value_t *ptr = codegen_find_local(cg, name);
            if (ptr) return ptr;
            
            /* Global variable - return global reference */
            if (sym && sym->kind == SYM_VAR) {
                anvil_type_t *type = codegen_type(cg, sym->type);
                anvil_value_t *global = codegen_get_or_add_global(cg, name, type);
                return global;
            }
            
            return NULL;
        }
        
        case AST_UNARY_EXPR:
            if (expr->data.unary_expr.op == UNOP_DEREF) {
                return codegen_expr(cg, expr->data.unary_expr.operand);
            }
            return NULL;
            
        case AST_SUBSCRIPT_EXPR: {
            /* For array subscript, we need to calculate the address correctly */
            mcc_ast_node_t *array_expr = expr->data.subscript_expr.array;
            mcc_type_t *array_type = array_expr->type;
            
            anvil_value_t *base;
            /* If the array expression is itself an array type, get its lvalue (address) */
            if (array_type && array_type->kind == TYPE_ARRAY) {
                base = codegen_lvalue(cg, array_expr);
            } else {
                /* Otherwise it's a pointer, get its value */
                base = codegen_expr(cg, array_expr);
            }
            
            anvil_value_t *index = codegen_expr(cg, expr->data.subscript_expr.index);
            
            /* Calculate element size for proper scaling */
            mcc_type_t *elem_type_mcc = expr->type;
            int elem_size = elem_type_mcc ? codegen_sizeof(cg, elem_type_mcc) : 4;
            
            /* Scale index by element size */
            anvil_value_t *offset;
            if (elem_size > 1) {
                anvil_value_t *scale = anvil_const_i64(cg->anvil_ctx, elem_size);
                offset = anvil_build_mul(cg->anvil_ctx, index, scale, "idx.scale");
            } else {
                offset = index;
            }
            
            /* Add offset to base */
            return anvil_build_add(cg->anvil_ctx, base, offset, "arr.idx");
        }
        
        case AST_MEMBER_EXPR: {
            mcc_ast_node_t *obj = expr->data.member_expr.object;
            anvil_value_t *ptr;
            
            if (expr->data.member_expr.is_arrow) {
                ptr = codegen_expr(cg, obj);
            } else {
                ptr = codegen_lvalue(cg, obj);
            }
            
            /* Find field index */
            mcc_type_t *obj_type = obj->type;
            if (expr->data.member_expr.is_arrow && mcc_type_is_pointer(obj_type)) {
                obj_type = obj_type->data.pointer.pointee;
            }
            
            int field_idx = 0;
            for (mcc_struct_field_t *f = obj_type->data.record.fields; f; f = f->next, field_idx++) {
                /* Skip anonymous fields (padding bitfields) */
                if (f->name && strcmp(f->name, expr->data.member_expr.member) == 0) {
                    break;
                }
            }
            
            anvil_type_t *struct_type = codegen_type(cg, obj_type);
            return anvil_build_struct_gep(cg->anvil_ctx, struct_type, ptr, field_idx, "field");
        }
        
        default:
            return NULL;
    }
}
