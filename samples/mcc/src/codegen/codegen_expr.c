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

static anvil_type_t *codegen_uintptr_type(mcc_codegen_t *cg)
{
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    return arch && arch->ptr_size == 4
        ? anvil_type_u32(cg->anvil_ctx)
        : anvil_type_u64(cg->anvil_ctx);
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
    if (!type) {
        mcc_error(cg->mcc_ctx,
                  "cannot emit an integer constant without a resolved C type");
        return NULL;
    }

    switch (type->kind) {
        case TYPE_BOOL:
            return anvil_const_i1(cg->anvil_ctx, val != 0);
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
            mcc_error(cg->mcc_ctx,
                      "cannot emit integer constant for type '%s'",
                      mcc_type_kind_name(type->kind));
            return NULL;
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
    /* Do not return solely because the C types compare equal.  Some IR
     * producers (notably comparisons) intentionally describe their i1
     * result with the C type it will eventually acquire.  The actual Anvil
     * source type is authoritative for deciding whether a cast is needed. */
    if (!from || !to) return val;

    anvil_type_t *dst = codegen_type(cg, to);
    anvil_type_t *src = anvil_value_get_type(val);
    const char *cast_name = name ? name : "cast";

    /* C conversion to _Bool is truth comparison, never low-bit truncation. */
    if (to->kind == TYPE_BOOL) {
        if (anvil_type_is_bool(src)) return val;
        if (mcc_type_is_floating(from)) {
            anvil_value_t *zero = anvil_type_size(src) == 4
                ? anvil_const_f32(cg->anvil_ctx, 0.0f)
                : anvil_const_f64(cg->anvil_ctx, 0.0);
            return anvil_build_fcmp(cg->anvil_ctx, ANVIL_FCMP_UNE,
                                    val, zero, cast_name);
        }
        anvil_value_t *zero = anvil_type_is_pointer(src)
            ? anvil_const_null(cg->anvil_ctx, src)
            : codegen_const_int_for_type(cg, src, 0);
        return anvil_build_cmp_ne(cg->anvil_ctx, val, zero, cast_name);
    }

    if (mcc_type_is_same(from, to)) {
        /* Validate scalar IR shape even when sema says the C types match;
         * aggregate values are already represented according to their
         * nominal C type and require no conversion. */
        if (mcc_type_is_integer(to)) {
            if (anvil_type_is_integer(src) &&
                anvil_type_bit_width(src) == anvil_type_bit_width(dst) &&
                anvil_type_is_signed(src) == anvil_type_is_signed(dst)) {
                return val;
            }
        } else if (mcc_type_is_floating(to)) {
            if (anvil_type_is_floating(src) &&
                anvil_type_size(src) == anvil_type_size(dst)) return val;
        } else if (mcc_type_is_pointer(to)) {
            if (anvil_type_is_pointer(src)) return val;
        } else if (mcc_type_is_record(to) || to->kind == TYPE_ARRAY ||
                   to->kind == TYPE_FUNCTION || to->kind == TYPE_VOID) {
            return val;
        }
        mcc_error(cg->mcc_ctx,
                  "IR value type does not match resolved C type '%s'",
                  mcc_type_kind_name(to->kind));
        return NULL;
    }

    if (mcc_type_is_integer(from) && mcc_type_is_integer(to)) {
        unsigned src_bits = anvil_type_bit_width(src);
        unsigned dst_bits = anvil_type_bit_width(dst);
        if (src_bits < dst_bits) {
            return anvil_type_is_signed(src)
                ? anvil_build_sext(cg->anvil_ctx, val, dst, cast_name)
                : anvil_build_zext(cg->anvil_ctx, val, dst, cast_name);
        }
        if (src_bits > dst_bits) {
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
        return anvil_type_is_signed(src)
            ? anvil_build_sitofp(cg->anvil_ctx, val, dst, cast_name)
            : anvil_build_uitofp(cg->anvil_ctx, val, dst, cast_name);
    }

    if (mcc_type_is_floating(from) && mcc_type_is_integer(to)) {
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

    mcc_error(cg->mcc_ctx,
              "unsupported code generation conversion from '%s' to '%s'",
              mcc_type_kind_name(from->kind), mcc_type_kind_name(to->kind));
    return NULL;
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

    anvil_type_t *operand_type = anvil_value_get_type(lhs);
    unsigned bits = anvil_type_bit_width(operand_type);
    if (bits == 0 || bits > 64) return NULL;
    bool is_unsigned = !anvil_type_is_signed(operand_type);
    uint64_t mask = bits == 64 ? UINT64_MAX
                               : (UINT64_C(1) << bits) - 1;
    uint64_t lu = anvil_const_int_unsigned_value(lhs) & mask;
    uint64_t ru = anvil_const_int_unsigned_value(rhs) & mask;
    int64_t ls, rs;

#define SIGN_EXTEND(raw_, out_) do {                                      \
        uint64_t raw_value_ = (raw_) & mask;                              \
        uint64_t sign_ = UINT64_C(1) << (bits - 1);                       \
        if (!(raw_value_ & sign_)) (out_) = (int64_t)raw_value_;          \
        else if (bits == 64 && raw_value_ == sign_) (out_) = INT64_MIN;   \
        else (out_) = -(int64_t)(((~raw_value_) & mask) + 1);             \
    } while (0)
    SIGN_EXTEND(lu, ls);
    SIGN_EXTEND(ru, rs);
#undef SIGN_EXTEND

    int64_t signed_min = bits == 64 ? INT64_MIN
        : -(INT64_C(1) << (bits - 1));
    int64_t signed_max = bits == 64 ? INT64_MAX
        : (INT64_C(1) << (bits - 1)) - 1;
    uint64_t raw_result = 0;
    bool comparison = false;
    bool bool_result = false;

    switch (op) {
        case BINOP_ADD:
        case BINOP_SUB:
        case BINOP_MUL:
            if (is_unsigned) {
                if (op == BINOP_ADD) raw_result = (lu + ru) & mask;
                else if (op == BINOP_SUB) raw_result = (lu - ru) & mask;
                else raw_result = (lu * ru) & mask;
            } else {
                __int128 wide = op == BINOP_ADD ? (__int128)ls + rs
                    : op == BINOP_SUB ? (__int128)ls - rs
                    : (__int128)ls * rs;
                if (wide < signed_min || wide > signed_max) return NULL;
                raw_result = (uint64_t)(int64_t)wide & mask;
            }
            break;
        case BINOP_DIV:
        case BINOP_MOD:
            if (is_unsigned) {
                if (ru == 0) return NULL;
                raw_result = op == BINOP_DIV ? lu / ru : lu % ru;
            } else {
                if (rs == 0 || (ls == signed_min && rs == -1)) return NULL;
                int64_t value = op == BINOP_DIV ? ls / rs : ls % rs;
                raw_result = (uint64_t)value & mask;
            }
            break;
        case BINOP_BIT_AND: raw_result = lu & ru; break;
        case BINOP_BIT_OR: raw_result = lu | ru; break;
        case BINOP_BIT_XOR: raw_result = lu ^ ru; break;
        case BINOP_LSHIFT:
            if (ru >= bits) return NULL;
            if (is_unsigned) raw_result = (lu << (unsigned)ru) & mask;
            else {
                if (ls < 0) return NULL;
                __int128 wide = (__int128)ls << (unsigned)ru;
                if (wide > signed_max) return NULL;
                raw_result = (uint64_t)(int64_t)wide & mask;
            }
            break;
        case BINOP_RSHIFT:
            if (ru >= bits) return NULL;
            if (is_unsigned || ls >= 0) raw_result = lu >> (unsigned)ru;
            else {
                raw_result = lu >> (unsigned)ru;
                if (ru != 0) raw_result |= mask ^ (mask >> (unsigned)ru);
                raw_result &= mask;
            }
            break;
        case BINOP_EQ: comparison = true; bool_result = lu == ru; break;
        case BINOP_NE: comparison = true; bool_result = lu != ru; break;
        case BINOP_LT: comparison = true; bool_result = is_unsigned ? lu < ru : ls < rs; break;
        case BINOP_GT: comparison = true; bool_result = is_unsigned ? lu > ru : ls > rs; break;
        case BINOP_LE: comparison = true; bool_result = is_unsigned ? lu <= ru : ls <= rs; break;
        case BINOP_GE: comparison = true; bool_result = is_unsigned ? lu >= ru : ls >= rs; break;
        default: return NULL;
    }

    if (comparison) {
        return codegen_const_int_for_mcc_type(cg, result_type,
                                              bool_result ? 1 : 0);
    }
    if (is_unsigned) {
        switch (bits) {
            case 8: return anvil_const_u8(cg->anvil_ctx, (uint8_t)raw_result);
            case 16: return anvil_const_u16(cg->anvil_ctx, (uint16_t)raw_result);
            case 32: return anvil_const_u32(cg->anvil_ctx, (uint32_t)raw_result);
            case 64: return anvil_const_u64(cg->anvil_ctx, raw_result);
            default: return NULL;
        }
    }

    int64_t signed_result;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    if (!(raw_result & sign)) signed_result = (int64_t)raw_result;
    else if (bits == 64 && raw_result == sign) signed_result = INT64_MIN;
    else signed_result = -(int64_t)(((~raw_result) & mask) + 1);
    return codegen_const_int_for_mcc_type(cg, result_type, signed_result);
}

/* C comparison semantics map directly to first-class ANVIL FCMP predicates.
 * In particular != is unordered-or-not-equal while all relational predicates
 * are ordered. The target backend owns IEEE/HFP representation details. */
static anvil_value_t *codegen_float_predicate(mcc_codegen_t *cg,
                                              mcc_binop_t op,
                                              anvil_value_t *lhs,
                                              anvil_value_t *rhs)
{
    anvil_fcmp_pred_t predicate;
    switch (op) {
        case BINOP_EQ: predicate = ANVIL_FCMP_OEQ; break;
        case BINOP_NE: predicate = ANVIL_FCMP_UNE; break;
        case BINOP_LT: predicate = ANVIL_FCMP_OLT; break;
        case BINOP_LE: predicate = ANVIL_FCMP_OLE; break;
        case BINOP_GT: predicate = ANVIL_FCMP_OGT; break;
        case BINOP_GE: predicate = ANVIL_FCMP_OGE; break;
        default: return NULL;
    }
    return anvil_build_fcmp(cg->anvil_ctx, predicate, lhs, rhs, "fcmp");
}

static anvil_value_t *codegen_float_negate(mcc_codegen_t *cg,
                                           anvil_value_t *value)
{
    return anvil_build_fneg(cg->anvil_ctx, value, "fneg");
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

        case AST_NULL_PTR:
            return anvil_const_null(cg->anvil_ctx, codegen_type(cg, expr->type));
            
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
                    anvil_value_t *indices[] = {
                        anvil_const_i64(cg->anvil_ctx, 0),
                        anvil_const_i64(cg->anvil_ctx, 0)
                    };
                    anvil_type_t *array_type = codegen_type(cg, sym->type);
                    return anvil_build_gep(cg->anvil_ctx, array_type, ptr,
                                           indices, 2, "array.decay");
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
                anvil_value_t *result;

                if (op == BINOP_ASSIGN) {
                    result = codegen_convert_value(cg, rhs, rhs_c_type,
                                                   lhs_c_type, "assign.cast");
                } else {
                    /* C compound assignments perform the operation after the
                     * usual promotions, then convert once back to the lhs.
                     * Doing the operation in the narrow lhs type is observably
                     * wrong (and made unsigned /= select signed division). */
                    mcc_type_t *op_lhs_type;
                    mcc_type_t *op_rhs_type;
                    if (op == BINOP_LSHIFT_ASSIGN ||
                        op == BINOP_RSHIFT_ASSIGN) {
                        op_lhs_type = mcc_type_promote(cg->types, lhs_c_type);
                        op_rhs_type = mcc_type_promote(cg->types, rhs_c_type);
                    } else {
                        op_lhs_type = mcc_type_common(cg->types, lhs_c_type,
                                                     rhs_c_type);
                        op_rhs_type = op_lhs_type;
                    }

                    anvil_type_t *lhs_storage_type = codegen_type(cg, lhs_c_type);
                    anvil_value_t *lhs = anvil_build_load(cg->anvil_ctx,
                        lhs_storage_type, lhs_ptr, "lhs");
                    lhs = codegen_convert_value(cg, lhs, lhs_c_type,
                                                op_lhs_type, "lhs.promote");
                    rhs = codegen_convert_value(cg, rhs, rhs_c_type,
                                                op_rhs_type, "rhs.promote");
                    if (!lhs || !rhs) return NULL;

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
                            result = op_lhs_type && op_lhs_type->is_unsigned
                                ? anvil_build_udiv(cg->anvil_ctx, lhs, rhs, "div")
                                : anvil_build_sdiv(cg->anvil_ctx, lhs, rhs, "div");
                            break;
                        case BINOP_MOD_ASSIGN:
                            result = op_lhs_type && op_lhs_type->is_unsigned
                                ? anvil_build_umod(cg->anvil_ctx, lhs, rhs, "mod")
                                : anvil_build_smod(cg->anvil_ctx, lhs, rhs, "mod");
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
                            result = op_lhs_type && op_lhs_type->is_unsigned
                                ? anvil_build_shr(cg->anvil_ctx, lhs, rhs, "shr")
                                : anvil_build_sar(cg->anvil_ctx, lhs, rhs, "sar");
                            break;
                        default:
                            result = NULL;
                            break;
                    }
                    result = codegen_convert_value(cg, result, op_lhs_type,
                                                   lhs_c_type, "assign.cast");
                }

                if (!result || !anvil_build_store(cg->anvil_ctx, result, lhs_ptr)) {
                    return NULL;
                }
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
                } else if (mcc_type_is_pointer(lhs_type) &&
                           mcc_type_is_pointer(rhs_type) &&
                           !mcc_type_is_same(lhs_type, rhs_type)) {
                    rhs = codegen_convert_value(cg, rhs, rhs_type, lhs_type,
                                                "ptr.cmp.cast");
                    rhs_type = lhs_type;
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
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_eq(cg->anvil_ctx, lhs, rhs, "eq"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_NE:
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_ne(cg->anvil_ctx, lhs, rhs, "ne"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_LT:
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    if (mcc_type_is_pointer(lhs_type)) {
                        anvil_type_t *uintptr_type = codegen_uintptr_type(cg);
                        lhs = anvil_build_ptrtoint(cg->anvil_ctx, lhs,
                                                  uintptr_type, "ptr.lhs");
                        rhs = anvil_build_ptrtoint(cg->anvil_ctx, rhs,
                                                  uintptr_type, "ptr.rhs");
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ult(cg->anvil_ctx, lhs, rhs, "ptr.ult"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ult(cg->anvil_ctx, lhs, rhs, "ult"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_lt(cg->anvil_ctx, lhs, rhs, "lt"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_GT:
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    if (mcc_type_is_pointer(lhs_type)) {
                        anvil_type_t *uintptr_type = codegen_uintptr_type(cg);
                        lhs = anvil_build_ptrtoint(cg->anvil_ctx, lhs,
                                                  uintptr_type, "ptr.lhs");
                        rhs = anvil_build_ptrtoint(cg->anvil_ctx, rhs,
                                                  uintptr_type, "ptr.rhs");
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ugt(cg->anvil_ctx, lhs, rhs, "ptr.ugt"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ugt(cg->anvil_ctx, lhs, rhs, "ugt"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_gt(cg->anvil_ctx, lhs, rhs, "gt"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_LE:
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    if (mcc_type_is_pointer(lhs_type)) {
                        anvil_type_t *uintptr_type = codegen_uintptr_type(cg);
                        lhs = anvil_build_ptrtoint(cg->anvil_ctx, lhs,
                                                  uintptr_type, "ptr.lhs");
                        rhs = anvil_build_ptrtoint(cg->anvil_ctx, rhs,
                                                  uintptr_type, "ptr.rhs");
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ule(cg->anvil_ctx, lhs, rhs, "ptr.ule"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    if (lhs_type && lhs_type->is_unsigned) {
                        return codegen_convert_value(cg,
                            anvil_build_cmp_ule(cg->anvil_ctx, lhs, rhs, "ule"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
                    return codegen_convert_value(cg,
                        anvil_build_cmp_le(cg->anvil_ctx, lhs, rhs, "le"),
                        mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                case BINOP_GE:
                    if (lhs_type && mcc_type_is_floating(lhs_type)) {
                        return codegen_convert_value(cg,
                            codegen_float_predicate(cg, op, lhs, rhs),
                            mcc_type_uchar(cg->types), expr->type, "fcmp.cast");
                    }
                    if (mcc_type_is_pointer(lhs_type)) {
                        anvil_type_t *uintptr_type = codegen_uintptr_type(cg);
                        lhs = anvil_build_ptrtoint(cg->anvil_ctx, lhs,
                                                  uintptr_type, "ptr.lhs");
                        rhs = anvil_build_ptrtoint(cg->anvil_ctx, rhs,
                                                  uintptr_type, "ptr.rhs");
                        return codegen_convert_value(cg,
                            anvil_build_cmp_uge(cg->anvil_ctx, lhs, rhs, "ptr.uge"),
                            mcc_type_uchar(cg->types), expr->type, "cmp.cast");
                    }
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
                        return codegen_float_negate(cg, val);
                    }
                    return anvil_build_neg(cg->anvil_ctx, val, "neg");
                }
                case UNOP_POS:
                    return codegen_expr(cg, expr->data.unary_expr.operand);
                case UNOP_NOT: {
                    anvil_value_t *val = codegen_expr(cg, expr->data.unary_expr.operand);
                    anvil_value_t *as_bool = codegen_to_bool(cg, val);
                    anvil_value_t *zero = anvil_const_i1(cg->anvil_ctx, false);
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
                case UNOP_ADDR: {
                    mcc_ast_node_t *operand = expr->data.unary_expr.operand;
                    anvil_value_t *address = codegen_lvalue(cg, operand);
                    /* ANVIL globals are addressable memory objects whose value
                     * type is the object type. Materialize a first-class C
                     * pointer with a zero GEP when taking a global's address. */
                    if (operand && operand->kind == AST_IDENT_EXPR &&
                        !codegen_find_local(cg, operand->data.ident_expr.name)) {
                        anvil_value_t *zero = anvil_const_i64(cg->anvil_ctx, 0);
                        return anvil_build_gep(cg->anvil_ctx,
                            codegen_type(cg, operand->type), address,
                            &zero, 1, "global.addr");
                    }
                    return address;
                }
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
            if (!type) return NULL;
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

        case AST_ALIGNOF_EXPR: {
            mcc_type_t *type = expr->data.alignof_expr.type_arg;
            if (!type && expr->data.alignof_expr.expr_arg)
                type = expr->data.alignof_expr.expr_arg->type;
            if (!type) return NULL;
            size_t align = anvil_type_align(codegen_type(cg, type));
            return codegen_const_int_for_mcc_type(cg, expr->type,
                                                  (int64_t)align);
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

        case AST_GENERIC_EXPR:
            /* The controlling expression of _Generic is not evaluated.  Sema
             * records the unique selected association; emit only that arm. */
            return codegen_expr(cg, expr->data.generic_expr.selected_expr);

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
            /* A typed GEP's first index steps over objects of the source type;
             * subsequent indices descend aggregates.  An array lvalue is a
             * pointer to the complete array object, so use {0, index}.  A C
             * pointer value already points at an element, so a single index
             * is the correct walk. */
            mcc_ast_node_t *array_expr = expr->data.subscript_expr.array;
            mcc_type_t *array_type = codegen_unwrap_type(array_expr->type);

            anvil_value_t *base;
            anvil_value_t *index = codegen_expr(cg, expr->data.subscript_expr.index);
            if (!array_type || !index) return NULL;

            if (array_type && array_type->kind == TYPE_ARRAY) {
                base = codegen_lvalue(cg, array_expr);
                anvil_value_t *indices[] = {
                    anvil_const_i64(cg->anvil_ctx, 0), index
                };
                return anvil_build_gep(cg->anvil_ctx,
                                       codegen_type(cg, array_type), base,
                                       indices, 2, "arr.idx");
            } else {
                base = codegen_expr(cg, array_expr);
                if (array_type->kind != TYPE_POINTER) return NULL;
                return anvil_build_gep(cg->anvil_ctx,
                    codegen_type(cg, array_type->data.pointer.pointee), base,
                    &index, 1, "arr.idx");
            }
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
            
            /* Follow every physical nesting level for C11 anonymous record
             * members. The semantic lookup returns the promoted leaf, whereas
             * struct GEP requires the complete path. */
            mcc_type_t *current_type = obj_type;
            const char *member = expr->data.member_expr.member;
            while (current_type) {
                int field_idx = 0;
                mcc_struct_field_t *match = NULL;
                mcc_struct_field_t *anonymous_path = NULL;
                for (mcc_struct_field_t *f = current_type->data.record.fields;
                     f; f = f->next) {
                    bool physical = f->name ||
                        (f->type && mcc_type_is_record(f->type) &&
                         f->bitfield_width == 0);
                    if (!physical) continue;
                    if (f->name && strcmp(f->name, member) == 0) {
                        match = f;
                        break;
                    }
                    if (!f->name && f->type && mcc_type_is_record(f->type) &&
                        mcc_type_find_field(f->type, member)) {
                        anonymous_path = f;
                        break;
                    }
                    field_idx++;
                }

                if (!match && !anonymous_path) return NULL;
                if (current_type->kind == TYPE_UNION) {
                    /* A union is represented by one maximally aligned storage
                     * field. Every member aliases offset zero; cast that
                     * storage pointer to the selected member pointer type. */
                    ptr = anvil_build_struct_gep(cg->anvil_ctx,
                        codegen_type(cg, current_type), ptr, 0,
                        "union.storage");
                    mcc_type_t *member_type = match
                        ? match->type : anonymous_path->type;
                    anvil_type_t *member_ptr_type = anvil_type_ptr(
                        cg->anvil_ctx, codegen_type(cg, member_type));
                    ptr = anvil_build_bitcast(cg->anvil_ctx, ptr,
                                              member_ptr_type,
                                              "union.member");
                } else {
                    ptr = anvil_build_struct_gep(cg->anvil_ctx,
                        codegen_type(cg, current_type), ptr,
                        (unsigned)field_idx, "field");
                }
                if (match) return ptr;
                current_type = anonymous_path->type;
            }
            return NULL;
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
    if (anvil_type_is_floating(t)) {
        anvil_value_t *zero = anvil_type_size(t) == 4
            ? anvil_const_f32(cg->anvil_ctx, 0.0f)
            : anvil_const_f64(cg->anvil_ctx, 0.0);
        return anvil_build_fcmp(cg->anvil_ctx, ANVIL_FCMP_UNE,
                                val, zero, "tobool");
    }
    anvil_value_t *zero = anvil_type_is_pointer(t)
        ? anvil_const_null(cg->anvil_ctx, t)
        : codegen_const_int_for_type(cg, t, 0);
    return anvil_build_cmp_ne(cg->anvil_ctx, val, zero, "tobool");
}
