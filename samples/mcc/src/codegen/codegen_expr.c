/*
 * MCC - Micro C Compiler
 * Code Generator - Expression Generation
 * 
 * This file handles code generation for all expression types.
 */

#include "codegen_internal.h"

static mcc_type_t *codegen_unwrap_type(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF) {
        type = type->data.typedef_ref.underlying;
    }
    return type;
}

static mcc_type_t *codegen_decay_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    type = codegen_unwrap_type(type);
    if (!type) return NULL;
    return mcc_type_decay(cg->types, type);
}

static mcc_type_t *codegen_default_arg_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    type = codegen_decay_type(cg, type);
    if (!type) return NULL;
    if (mcc_type_is_integer(type)) {
        return mcc_type_promote(cg->types, type);
    }
    if (type->kind == TYPE_FLOAT) {
        return mcc_type_double(cg->types);
    }
    return type;
}

static mcc_type_t *codegen_callee_function_type(mcc_ast_node_t *func_expr)
{
    mcc_type_t *type = func_expr ? codegen_unwrap_type(func_expr->type) : NULL;
    if (type && type->kind == TYPE_POINTER) {
        type = codegen_unwrap_type(type->data.pointer.pointee);
    }
    return type && type->kind == TYPE_FUNCTION ? type : NULL;
}

static anvil_value_t *codegen_const_int_for_mcc_type(mcc_codegen_t *cg,
                                                     mcc_type_t *type,
                                                     int64_t val)
{
    type = codegen_unwrap_type(type);
    if (!type) return anvil_const_i32(cg->anvil_ctx, (int32_t)val);

    switch (type->kind) {
        case TYPE_BOOL:
            return anvil_const_u8(cg->anvil_ctx, val ? 1 : 0);
        case TYPE_CHAR:
            return type->is_unsigned
                ? anvil_const_u8(cg->anvil_ctx, (uint8_t)val)
                : anvil_const_i8(cg->anvil_ctx, (int8_t)val);
        case TYPE_SHORT:
            return type->is_unsigned
                ? anvil_const_u16(cg->anvil_ctx, (uint16_t)val)
                : anvil_const_i16(cg->anvil_ctx, (int16_t)val);
        case TYPE_INT:
        case TYPE_ENUM:
            return type->is_unsigned
                ? anvil_const_u32(cg->anvil_ctx, (uint32_t)val)
                : anvil_const_i32(cg->anvil_ctx, (int32_t)val);
        case TYPE_LONG: {
            size_t sz = codegen_sizeof(cg, type);
            if (sz == 8) {
                return type->is_unsigned
                    ? anvil_const_u64(cg->anvil_ctx, (uint64_t)val)
                    : anvil_const_i64(cg->anvil_ctx, val);
            }
            return type->is_unsigned
                ? anvil_const_u32(cg->anvil_ctx, (uint32_t)val)
                : anvil_const_i32(cg->anvil_ctx, (int32_t)val);
        }
        case TYPE_LONG_LONG:
            return type->is_unsigned
                ? anvil_const_u64(cg->anvil_ctx, (uint64_t)val)
                : anvil_const_i64(cg->anvil_ctx, val);
        default:
            return anvil_const_i32(cg->anvil_ctx, (int32_t)val);
    }
}

anvil_value_t *codegen_convert_value(mcc_codegen_t *cg,
                                     anvil_value_t *val,
                                     mcc_type_t *from,
                                     mcc_type_t *to,
                                     const char *name)
{
    if (!val) return NULL;
    from = codegen_decay_type(cg, from);
    to = codegen_decay_type(cg, to);
    if (!from || !to || mcc_type_is_same(from, to)) return val;

    anvil_type_t *dst = codegen_type(cg, to);
    anvil_type_t *src = anvil_value_get_type(val);
    const char *cast_name = name ? name : "cast";

    if (mcc_type_is_integer(from) && mcc_type_is_integer(to)) {
        size_t src_size = anvil_type_size(src);
        size_t dst_size = anvil_type_size(dst);
        if (src_size < dst_size) {
            return from->is_unsigned
                ? anvil_build_zext(cg->anvil_ctx, val, dst, cast_name)
                : anvil_build_sext(cg->anvil_ctx, val, dst, cast_name);
        }
        if (src_size > dst_size) {
            return anvil_build_trunc(cg->anvil_ctx, val, dst, cast_name);
        }
        if (anvil_type_is_signed(src) == anvil_type_is_signed(dst)) {
            return val;
        }
        return anvil_build_bitcast(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_floating(from) && mcc_type_is_floating(to)) {
        if (from->size < to->size) {
            return anvil_build_fpext(cg->anvil_ctx, val, dst, cast_name);
        }
        if (from->size > to->size) {
            return anvil_build_fptrunc(cg->anvil_ctx, val, dst, cast_name);
        }
        return val;
    }

    if (mcc_type_is_integer(from) && mcc_type_is_floating(to)) {
        if (anvil_value_is_const_int(val)) {
            double converted = anvil_type_is_signed(src)
                ? (double)anvil_const_int_signed_value(val)
                : (double)anvil_const_int_unsigned_value(val);
            return anvil_type_size(dst) == 4
                ? anvil_const_f32(cg->anvil_ctx, (float)converted)
                : anvil_const_f64(cg->anvil_ctx, converted);
        }
        return from->is_unsigned
            ? anvil_build_uitofp(cg->anvil_ctx, val, dst, cast_name)
            : anvil_build_sitofp(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_floating(from) && mcc_type_is_integer(to)) {
        if (anvil_value_is_const_float(val)) {
            return codegen_const_int_for_type(
                cg, dst, (int64_t)anvil_const_float_value(val));
        }
        return to->is_unsigned
            ? anvil_build_fptoui(cg->anvil_ctx, val, dst, cast_name)
            : anvil_build_fptosi(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_pointer(from) && mcc_type_is_pointer(to)) {
        return anvil_build_bitcast(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_pointer(from) && mcc_type_is_integer(to)) {
        return anvil_build_ptrtoint(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_integer(from) && mcc_type_is_pointer(to)) {
        return anvil_build_inttoptr(cg->anvil_ctx, val, dst, cast_name);
    }

    return val;
}

static anvil_value_t *codegen_fold_integer_binary(mcc_codegen_t *cg,
                                                  mcc_binop_t op,
                                                  anvil_value_t *lhs,
                                                  anvil_value_t *rhs,
                                                  mcc_type_t *result_type)
{
    if (!anvil_value_is_const_int(lhs) || !anvil_value_is_const_int(rhs)) {
        return NULL;
    }
    if (!result_type || !mcc_type_is_integer(result_type)) return NULL;

    bool is_unsigned = result_type->is_unsigned;
    uint64_t lu = anvil_const_int_unsigned_value(lhs);
    uint64_t ru = anvil_const_int_unsigned_value(rhs);
    int64_t ls = anvil_const_int_signed_value(lhs);
    int64_t rs = anvil_const_int_signed_value(rhs);
    uint64_t ur = 0;
    int64_t sr = 0;
    bool comparison = false;

    switch (op) {
        case BINOP_ADD: ur = lu + ru; sr = ls + rs; break;
        case BINOP_SUB: ur = lu - ru; sr = ls - rs; break;
        case BINOP_MUL: ur = lu * ru; sr = ls * rs; break;
        case BINOP_DIV:
            if ((is_unsigned && ru == 0) || (!is_unsigned && rs == 0)) return NULL;
            ur = lu / ru; sr = ls / rs; break;
        case BINOP_MOD:
            if ((is_unsigned && ru == 0) || (!is_unsigned && rs == 0)) return NULL;
            ur = lu % ru; sr = ls % rs; break;
        case BINOP_BIT_AND: ur = lu & ru; sr = ls & rs; break;
        case BINOP_BIT_OR: ur = lu | ru; sr = ls | rs; break;
        case BINOP_BIT_XOR: ur = lu ^ ru; sr = ls ^ rs; break;
        case BINOP_LSHIFT: ur = lu << (ru & 63); sr = ls << (ru & 63); break;
        case BINOP_RSHIFT: ur = lu >> (ru & 63); sr = ls >> (ru & 63); break;
        case BINOP_EQ: comparison = true; sr = lu == ru; break;
        case BINOP_NE: comparison = true; sr = lu != ru; break;
        case BINOP_LT: comparison = true; sr = is_unsigned ? (lu < ru) : (ls < rs); break;
        case BINOP_GT: comparison = true; sr = is_unsigned ? (lu > ru) : (ls > rs); break;
        case BINOP_LE: comparison = true; sr = is_unsigned ? (lu <= ru) : (ls <= rs); break;
        case BINOP_GE: comparison = true; sr = is_unsigned ? (lu >= ru) : (ls >= rs); break;
        default: return NULL;
    }

    return codegen_const_int_for_mcc_type(
        cg, result_type, comparison || !is_unsigned ? sr : (int64_t)ur);
}

/* Generate code for expression (returns value) */
anvil_value_t *codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case AST_INT_LIT: {
            if (expr->type && mcc_type_is_integer(expr->type)) {
                return codegen_const_int_for_mcc_type(cg, expr->type,
                    (int64_t)expr->data.int_lit.value);
            }
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
            return anvil_const_i32(cg->anvil_ctx, (int32_t)expr->data.char_lit.value);
            
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
                    anvil_value_t *zero = anvil_const_i64(cg->anvil_ctx, 0);
                    anvil_type_t *elem_type =
                        codegen_type(cg, sym->type->data.array.element);
                    return anvil_build_gep(cg->anvil_ctx, elem_type, ptr,
                                           &zero, 1, "array.decay");
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
            
            /* Enum constant - return its integer value */
            if (sym && sym->kind == SYM_ENUM_CONST) {
                return anvil_const_i32(cg->anvil_ctx, sym->data.enum_value);
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

                mcc_type_t *lhs_c_type = expr->data.binary_expr.lhs->type;
                mcc_type_t *rhs_c_type = expr->data.binary_expr.rhs->type;
                rhs = codegen_convert_value(cg, rhs, rhs_c_type, lhs_c_type,
                                            "assign.cast");
                
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
                
                anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
                anvil_value_t *one = anvil_const_i32(cg->anvil_ctx, 1);
                anvil_value_t *lhs_bool = codegen_to_bool(cg, lhs);
                
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
                anvil_value_t *rhs_bool = codegen_to_bool(cg, rhs);
                anvil_value_t *rhs_i32 = codegen_convert_value(cg, rhs_bool,
                    mcc_type_uchar(cg->types), mcc_type_int(cg->types),
                    "bool.cast");
                anvil_build_store(cg->anvil_ctx, rhs_i32, result_ptr);
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
                mcc_type_t *pointee = lhs_type->data.pointer.pointee;
                if (op == BINOP_SUB && rhs_type && rhs_type->kind == TYPE_POINTER) {
                    anvil_type_t *i64 = anvil_type_i64(cg->anvil_ctx);
                    anvil_value_t *lhs_int = anvil_build_ptrtoint(cg->anvil_ctx, lhs, i64, "ptr.lhs");
                    anvil_value_t *rhs_int = anvil_build_ptrtoint(cg->anvil_ctx, rhs, i64, "ptr.rhs");
                    anvil_value_t *diff = anvil_build_sub(cg->anvil_ctx, lhs_int, rhs_int, "ptr.diff");
                    int elem_size = pointee ? codegen_sizeof(cg, pointee) : 1;
                    if (elem_size > 1) {
                        diff = anvil_build_sdiv(cg->anvil_ctx, diff,
                                                anvil_const_i64(cg->anvil_ctx, elem_size),
                                                "ptr.diff.elem");
                    }
                    return codegen_convert_value(cg, diff, mcc_type_long(cg->types),
                                                 expr->type, "ptrdiff.cast");
                }
                anvil_type_t *elem_type = codegen_type(cg, pointee);
                anvil_value_t *index = rhs;
                if (op == BINOP_ADD) {
                    return anvil_build_gep(cg->anvil_ctx, elem_type, lhs, &index, 1, "ptr.gep");
                }
                index = anvil_build_neg(cg->anvil_ctx, rhs, "ptr.negidx");
                return anvil_build_gep(cg->anvil_ctx, elem_type, lhs, &index, 1, "ptr.gep");
            }
            
            if (op == BINOP_ADD && rhs_type && rhs_type->kind == TYPE_POINTER) {
                mcc_type_t *pointee = rhs_type->data.pointer.pointee;
                anvil_type_t *elem_type = codegen_type(cg, pointee);
                anvil_value_t *index = lhs;
                return anvil_build_gep(cg->anvil_ctx, elem_type, rhs, &index, 1, "ptr.gep");
            }

            if (op >= BINOP_EQ && op <= BINOP_GE) {
                if (mcc_type_is_pointer(lhs_type) && mcc_type_is_integer(rhs_type)) {
                    rhs = codegen_convert_value(cg, rhs, rhs_type, lhs_type,
                                                "null.cast");
                    rhs_type = lhs_type;
                } else if (mcc_type_is_integer(lhs_type) && mcc_type_is_pointer(rhs_type)) {
                    lhs = codegen_convert_value(cg, lhs, lhs_type, rhs_type,
                                                "null.cast");
                    lhs_type = rhs_type;
                }
            }

            if (mcc_type_is_arithmetic(lhs_type) && mcc_type_is_arithmetic(rhs_type)) {
                mcc_type_t *operand_type = NULL;
                if (op >= BINOP_EQ && op <= BINOP_GE) {
                    operand_type = mcc_type_common(cg->types, lhs_type, rhs_type);
                } else if (op == BINOP_LSHIFT || op == BINOP_RSHIFT) {
                    mcc_type_t *lhs_promoted = mcc_type_promote(cg->types, lhs_type);
                    mcc_type_t *rhs_promoted = mcc_type_promote(cg->types, rhs_type);
                    lhs = codegen_convert_value(cg, lhs, lhs_type, lhs_promoted,
                                                "lhs.promote");
                    rhs = codegen_convert_value(cg, rhs, rhs_type, rhs_promoted,
                                                "rhs.promote");
                    lhs_type = lhs_promoted;
                    rhs_type = rhs_promoted;
                } else {
                    operand_type = expr->type;
                }

                if (operand_type) {
                    lhs = codegen_convert_value(cg, lhs, lhs_type, operand_type,
                                                "lhs.cast");
                    rhs = codegen_convert_value(cg, rhs, rhs_type, operand_type,
                                                "rhs.cast");
                    lhs_type = operand_type;
                    rhs_type = operand_type;
                }
            }

            anvil_value_t *folded =
                codegen_fold_integer_binary(cg, op, lhs, rhs, expr->type);
            if (folded) return folded;
            
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
                    return codegen_convert_value(cg,
                        anvil_build_cmp_eq(cg->anvil_ctx, lhs, rhs, "eq"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_NE:
                    return codegen_convert_value(cg,
                        anvil_build_cmp_ne(cg->anvil_ctx, lhs, rhs, "ne"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_LT:
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ult(cg->anvil_ctx, lhs, rhs, "ult"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_lt(cg->anvil_ctx, lhs, rhs, "lt"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_GT:
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ugt(cg->anvil_ctx, lhs, rhs, "ugt"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_gt(cg->anvil_ctx, lhs, rhs, "gt"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_LE:
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ule(cg->anvil_ctx, lhs, rhs, "ule"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_le(cg->anvil_ctx, lhs, rhs, "le"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_GE:
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_uge(cg->anvil_ctx, lhs, rhs, "uge"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_ge(cg->anvil_ctx, lhs, rhs, "ge"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
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
                    anvil_value_t *as_bool = codegen_to_bool(cg, val);
                    anvil_value_t *zero = anvil_const_i8(cg->anvil_ctx, 0);
                    return codegen_convert_value(cg,
                        anvil_build_cmp_eq(cg->anvil_ctx, as_bool, zero, "not"),
                        mcc_type_uchar(cg->types), expr->type, "not.cast");
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
                    mcc_type_t *operand_type = expr->data.unary_expr.operand->type;
                    anvil_type_t *type = codegen_type(cg, operand_type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *result;
                    if (operand_type && operand_type->kind == TYPE_POINTER) {
                        int64_t delta = (op == UNOP_PRE_INC) ? 1 : -1;
                        anvil_value_t *index = anvil_const_i64(cg->anvil_ctx, delta);
                        anvil_type_t *elem_type =
                            codegen_type(cg, operand_type->data.pointer.pointee);
                        result = anvil_build_gep(cg->anvil_ctx, elem_type, val,
                                                 &index, 1, "ptr.inc");
                    } else {
                        anvil_value_t *one = codegen_const_int_for_mcc_type(cg,
                            operand_type, 1);
                        result = (op == UNOP_PRE_INC) ?
                            anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                            anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    }
                    anvil_build_store(cg->anvil_ctx, result, ptr);
                    return result;
                }
                case UNOP_POST_INC:
                case UNOP_POST_DEC: {
                    anvil_value_t *ptr = codegen_lvalue(cg, expr->data.unary_expr.operand);
                    mcc_type_t *operand_type = expr->data.unary_expr.operand->type;
                    anvil_type_t *type = codegen_type(cg, operand_type);
                    anvil_value_t *val = anvil_build_load(cg->anvil_ctx, type, ptr, "val");
                    anvil_value_t *result;
                    if (operand_type && operand_type->kind == TYPE_POINTER) {
                        int64_t delta = (op == UNOP_POST_INC) ? 1 : -1;
                        anvil_value_t *index = anvil_const_i64(cg->anvil_ctx, delta);
                        anvil_type_t *elem_type =
                            codegen_type(cg, operand_type->data.pointer.pointee);
                        result = anvil_build_gep(cg->anvil_ctx, elem_type, val,
                                                 &index, 1, "ptr.inc");
                    } else {
                        anvil_value_t *one = codegen_const_int_for_mcc_type(cg,
                            operand_type, 1);
                        result = (op == UNOP_POST_INC) ?
                            anvil_build_add(cg->anvil_ctx, val, one, "inc") :
                            anvil_build_sub(cg->anvil_ctx, val, one, "dec");
                    }
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
            
            anvil_value_t *cond_bool = codegen_to_bool(cg, cond);
            anvil_build_br_cond(cg->anvil_ctx, cond_bool, then_block, else_block);
            
            /* Then block */
            codegen_set_current_block(cg, then_block);
            anvil_value_t *then_val = codegen_expr(cg, expr->data.ternary_expr.then_expr);
            then_val = codegen_convert_value(cg, then_val,
                                             expr->data.ternary_expr.then_expr->type,
                                             expr->type, "ternary.cast");
            anvil_build_store(cg->anvil_ctx, then_val, result_ptr);
            anvil_build_br(cg->anvil_ctx, end_block);
            
            /* Else block */
            codegen_set_current_block(cg, else_block);
            anvil_value_t *else_val = codegen_expr(cg, expr->data.ternary_expr.else_expr);
            else_val = codegen_convert_value(cg, else_val,
                                             expr->data.ternary_expr.else_expr->type,
                                             expr->type, "ternary.cast");
            anvil_build_store(cg->anvil_ctx, else_val, result_ptr);
            anvil_build_br(cg->anvil_ctx, end_block);
            
            /* End block - load result */
            codegen_set_current_block(cg, end_block);
            return anvil_build_load(cg->anvil_ctx, type, result_ptr, "ternary.val");
        }
        
        case AST_CALL_EXPR: {
            anvil_value_t *func = codegen_expr(cg, expr->data.call_expr.func);
            mcc_type_t *callee_type =
                codegen_callee_function_type(expr->data.call_expr.func);
            
            size_t num_args = expr->data.call_expr.num_args;
            anvil_value_t **args = NULL;
            mcc_func_param_t *param = callee_type ? callee_type->data.function.params : NULL;
            if (num_args > 0) {
                args = mcc_alloc(cg->mcc_ctx, num_args * sizeof(anvil_value_t*));
                for (size_t i = 0; i < num_args; i++) {
                    mcc_ast_node_t *arg_node = expr->data.call_expr.args[i];
                    mcc_type_t *target_type = NULL;
                    if (param) {
                        target_type = param->type;
                        param = param->next;
                    } else {
                        target_type = codegen_default_arg_type(cg, arg_node->type);
                    }
                    if (codegen_type_pass_by_reference(target_type)) {
                        args[i] = codegen_lvalue(cg, arg_node);
                    } else {
                        args[i] = codegen_expr(cg, arg_node);
                        args[i] = codegen_convert_value(cg, args[i], arg_node->type,
                                                        target_type, "arg.cast");
                    }
                }
            }
            
            anvil_type_t *func_type = codegen_type(cg,
                callee_type ? callee_type : expr->data.call_expr.func->type);
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
            return codegen_convert_value(cg, val, from, to, "cast");
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
            return codegen_const_int_for_mcc_type(cg, expr->type, (int64_t)size);
        }
        
        case AST_COMMA_EXPR:
            codegen_expr(cg, expr->data.comma_expr.left);
            return codegen_expr(cg, expr->data.comma_expr.right);

        case AST_STMT_EXPR: {
            /* GNU statement expression: evaluate each inner statement as
             * usual, then return the value of the last expression statement.
             * Non-expression last statements (and empty bodies) yield a
             * void/null result, matching GCC/Clang. */
            mcc_ast_node_t *stmt = expr->data.stmt_expr.stmt;
            if (!stmt || stmt->kind != AST_COMPOUND_STMT) return NULL;

            size_t n = stmt->data.compound_stmt.num_stmts;
            anvil_value_t *last_val = NULL;
            for (size_t i = 0; i < n; i++) {
                mcc_ast_node_t *s = stmt->data.compound_stmt.stmts[i];
                bool is_last = (i + 1 == n);
                if (is_last && s && s->kind == AST_EXPR_STMT &&
                    s->data.expr_stmt.expr) {
                    last_val = codegen_expr(cg, s->data.expr_stmt.expr);
                } else {
                    codegen_stmt(cg, s);
                }
            }
            return last_val;
        }

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

            mcc_type_t *elem_type_mcc = expr->type;
            anvil_type_t *elem_type = codegen_type(cg, elem_type_mcc);
            return anvil_build_gep(cg->anvil_ctx, elem_type, base, &index, 1, "arr.idx");
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
            for (mcc_struct_field_t *f = obj_type->data.record.fields; f; f = f->next) {
                /* Skip anonymous fields (padding bitfields) - don't count them */
                if (!f->name) {
                    continue;
                }
                if (strcmp(f->name, expr->data.member_expr.member) == 0) {
                    break;
                }
                field_idx++;
            }
            
            anvil_type_t *struct_type = codegen_type(cg, obj_type);
            return anvil_build_struct_gep(cg->anvil_ctx, struct_type, ptr, field_idx, "field");
        }
        
        default:
            return NULL;
    }
}

/* Convert value to boolean for conditional branch.
 * Avoids redundant CMP_NE when value is already boolean (comparison result).
 */
anvil_value_t *codegen_to_bool(mcc_codegen_t *cg, anvil_value_t *val)
{
    if (!val) return NULL;

    /* Check if value is already boolean (comparison result) */
    if (anvil_value_is_bool(val)) {
        return val;
    }

    /* Not a boolean — compare with zero of the same Anvil type so the
     * backend isn't handed a cmp_ne(i64, i32) or cmp_ne(ptr, i32). */
    anvil_type_t *t = anvil_value_get_type(val);
    anvil_value_t *zero = anvil_type_is_pointer(t)
        ? anvil_const_null(cg->anvil_ctx, t)
        : codegen_const_int_for_type(cg, t, 0);
    return anvil_build_cmp_ne(cg->anvil_ctx, val, zero, "tobool");
}
