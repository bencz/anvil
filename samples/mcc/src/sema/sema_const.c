/*
 * MCC - target-width integer constant expression evaluation.
 *
 * Evaluation uses unsigned storage and explicit width normalization so host C
 * overflow is never used to model target arithmetic. Signed overflow and
 * invalid shifts/division are diagnosed instead of becoming compiler UB.
 */

#include "sema_internal.h"

#include <limits.h>
#include <stdint.h>

static mcc_type_t *unwrap_type(mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    return type;
}

static unsigned integer_bits(mcc_sema_t *sema, mcc_type_t *type)
{
    type = unwrap_type(type);
    if (!type) return 64;
    switch (type->kind) {
        case TYPE_BOOL:
        case TYPE_CHAR: return 8;
        case TYPE_SHORT: return 16;
        case TYPE_INT:
        case TYPE_ENUM: return 32;
        case TYPE_LONG:
            switch (sema->ctx->options.arch) {
                case MCC_ARCH_X86_64:
                case MCC_ARCH_ZARCH:
                case MCC_ARCH_PPC64:
                case MCC_ARCH_PPC64LE:
                case MCC_ARCH_ARM64:
                case MCC_ARCH_ARM64_MACOS:
                    return 64;
                default:
                    return 32;
            }
        case TYPE_LONG_LONG: return 64;
        default: return 64;
    }
}

static bool integer_unsigned(mcc_type_t *type)
{
    type = unwrap_type(type);
    return type && (type->kind == TYPE_BOOL || type->is_unsigned);
}

static uint64_t width_mask(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

static uint64_t normalize(uint64_t value, unsigned bits)
{
    return value & width_mask(bits);
}

static int64_t signed_value(uint64_t value, unsigned bits)
{
    value = normalize(value, bits);
    uint64_t sign = UINT64_C(1) << (bits - 1);
    if (!(value & sign)) return (int64_t)value;
    uint64_t magnitude = normalize(~value, bits) + 1;
    if (bits == 64 && magnitude == (UINT64_C(1) << 63)) return INT64_MIN;
    return -(int64_t)magnitude;
}

static bool signed_fits(int64_t value, unsigned bits)
{
    if (bits >= 64) return true;
    int64_t min = -(INT64_C(1) << (bits - 1));
    int64_t max = (INT64_C(1) << (bits - 1)) - 1;
    return value >= min && value <= max;
}

static int64_t signed_minimum(unsigned bits)
{
    return bits >= 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
}

static uint64_t convert_integer(mcc_sema_t *sema, uint64_t value,
                                mcc_type_t *from, unsigned target_bits)
{
    unsigned from_bits = integer_bits(sema, from);
    value = normalize(value, from_bits);
    if (!integer_unsigned(from) && (value & (UINT64_C(1) << (from_bits - 1))))
        value = (uint64_t)signed_value(value, from_bits);
    return normalize(value, target_bits);
}

static bool eval_const_bits(mcc_sema_t *sema, mcc_ast_node_t *expr,
                            uint64_t *result);

static bool record_member_offset(mcc_type_t *record, const char *name,
                                 size_t *offset)
{
    record = unwrap_type(record);
    if (!record || !name || !offset || !mcc_type_is_record(record))
        return false;
    for (mcc_struct_field_t *field = record->data.record.fields;
         field; field = field->next) {
        if (field->name && strcmp(field->name, name) == 0) {
            *offset = field->offset;
            return true;
        }
        if (!field->name && field->type && mcc_type_is_record(field->type)) {
            size_t inner;
            if (record_member_offset(field->type, name, &inner)) {
                if (inner > SIZE_MAX - field->offset) return false;
                *offset = field->offset + inner;
                return true;
            }
        }
    }
    return false;
}

static bool eval_null_pointer(mcc_sema_t *sema, mcc_ast_node_t *expr,
                              uint64_t *result)
{
    if (!expr || !result) return false;
    if (expr->kind == AST_NULL_PTR) {
        *result = 0;
        return true;
    }
    if (expr->kind != AST_CAST_EXPR) return false;
    mcc_type_t *target = unwrap_type(expr->data.cast_expr.target_type);
    if (!target || !mcc_type_is_pointer(target)) return false;
    uint64_t value;
    if (!eval_const_bits(sema, expr->data.cast_expr.expr, &value) || value != 0)
        return false;
    *result = 0;
    return true;
}

/* Recognize the null-based address expression used by the standard offsetof
 * macro.  This is deliberately narrow: arbitrary object addresses are not
 * integer constant expressions. Nested member designators and promoted
 * anonymous records compose their exact frontend-computed offsets. */
static bool eval_offset_address(mcc_sema_t *sema, mcc_ast_node_t *expr,
                                uint64_t *result)
{
    if (!expr || expr->kind != AST_MEMBER_EXPR || !result) return false;
    mcc_ast_node_t *object = expr->data.member_expr.object;
    mcc_type_t *record = unwrap_type(object ? object->type : NULL);
    uint64_t base;
    if (expr->data.member_expr.is_arrow) {
        if (!record || !mcc_type_is_pointer(record) ||
            !eval_null_pointer(sema, object, &base)) return false;
        record = unwrap_type(record->data.pointer.pointee);
    } else {
        if (!eval_offset_address(sema, object, &base)) return false;
    }
    size_t field_offset;
    if (!record_member_offset(record, expr->data.member_expr.member,
                              &field_offset) ||
        (uint64_t)field_offset > UINT64_MAX - base) return false;
    *result = base + (uint64_t)field_offset;
    return true;
}

static bool eval_binary(mcc_sema_t *sema, mcc_ast_node_t *expr,
                        uint64_t *result)
{
    mcc_binop_t op = expr->data.binary_expr.op;
    uint64_t lhs_raw;
    if (!eval_const_bits(sema, expr->data.binary_expr.lhs, &lhs_raw))
        return false;

    /* Preserve C's short-circuit semantics. The unselected operand is not
     * evaluated and therefore must not trigger diagnostics such as division
     * by zero in `0 && (1 / 0)` or `1 || (1 / 0)`. */
    if (op == BINOP_AND || op == BINOP_OR) {
        if ((op == BINOP_AND && !lhs_raw) ||
            (op == BINOP_OR && lhs_raw)) {
            *result = op == BINOP_OR;
            return true;
        }
        uint64_t rhs_raw;
        if (!eval_const_bits(sema, expr->data.binary_expr.rhs, &rhs_raw))
            return false;
        *result = !!rhs_raw;
        return true;
    }

    uint64_t rhs_raw;
    if (!eval_const_bits(sema, expr->data.binary_expr.rhs, &rhs_raw))
        return false;

    bool comparison = op >= BINOP_EQ && op <= BINOP_GE;
    mcc_type_t *op_type = comparison
        ? sema_apply_usual_conversions(
              sema, expr->data.binary_expr.lhs->type,
              expr->data.binary_expr.rhs->type)
        : expr->type;
    if (!op_type || !mcc_type_is_integer(op_type)) return false;
    unsigned bits = integer_bits(sema, op_type);
    bool is_unsigned = integer_unsigned(op_type);
    uint64_t lhs = convert_integer(sema, lhs_raw,
        expr->data.binary_expr.lhs->type, bits);
    uint64_t rhs = convert_integer(sema, rhs_raw,
        expr->data.binary_expr.rhs->type, bits);
    int64_t slhs = signed_value(lhs, bits);
    int64_t srhs = signed_value(rhs, bits);

    if (comparison) {
        switch (op) {
            case BINOP_EQ: *result = lhs == rhs; return true;
            case BINOP_NE: *result = lhs != rhs; return true;
            case BINOP_LT: *result = is_unsigned ? lhs < rhs : slhs < srhs; return true;
            case BINOP_GT: *result = is_unsigned ? lhs > rhs : slhs > srhs; return true;
            case BINOP_LE: *result = is_unsigned ? lhs <= rhs : slhs <= srhs; return true;
            case BINOP_GE: *result = is_unsigned ? lhs >= rhs : slhs >= srhs; return true;
            default: return false;
        }
    }

    uint64_t raw = 0;
    int64_t signed_result = 0;
    switch (op) {
        case BINOP_ADD:
            if (is_unsigned) raw = lhs + rhs;
            else if (__builtin_add_overflow(slhs, srhs, &signed_result) ||
                     !signed_fits(signed_result, bits)) goto signed_overflow;
            else raw = (uint64_t)signed_result;
            break;
        case BINOP_SUB:
            if (is_unsigned) raw = lhs - rhs;
            else if (__builtin_sub_overflow(slhs, srhs, &signed_result) ||
                     !signed_fits(signed_result, bits)) goto signed_overflow;
            else raw = (uint64_t)signed_result;
            break;
        case BINOP_MUL:
            if (is_unsigned) raw = lhs * rhs;
            else if (__builtin_mul_overflow(slhs, srhs, &signed_result) ||
                     !signed_fits(signed_result, bits)) goto signed_overflow;
            else raw = (uint64_t)signed_result;
            break;
        case BINOP_DIV:
        case BINOP_MOD:
            if (rhs == 0) {
                mcc_error_at(sema->ctx, expr->location,
                             "division by zero in constant expression");
                return false;
            }
            if (is_unsigned) raw = op == BINOP_DIV ? lhs / rhs : lhs % rhs;
            else {
                if (slhs == signed_minimum(bits) && srhs == -1)
                    goto signed_overflow;
                raw = (uint64_t)(op == BINOP_DIV ? slhs / srhs : slhs % srhs);
            }
            break;
        case BINOP_LSHIFT:
        case BINOP_RSHIFT: {
            if (rhs >= bits) {
                mcc_error_at(sema->ctx, expr->location,
                             "shift count out of range in constant expression");
                return false;
            }
            unsigned amount = (unsigned)rhs;
            if (op == BINOP_RSHIFT) {
                raw = is_unsigned ? lhs >> amount
                                  : (uint64_t)(slhs >> amount);
            } else if (is_unsigned) {
                raw = lhs << amount;
            } else {
                if (slhs < 0 || (amount &&
                    (uint64_t)slhs > (uint64_t)(bits == 64 ? INT64_MAX
                        : ((INT64_C(1) << (bits - 1)) - 1)) >> amount))
                    goto signed_overflow;
                raw = (uint64_t)slhs << amount;
            }
            break;
        }
        case BINOP_BIT_AND: raw = lhs & rhs; break;
        case BINOP_BIT_OR:  raw = lhs | rhs; break;
        case BINOP_BIT_XOR: raw = lhs ^ rhs; break;
        default: return false;
    }
    *result = normalize(raw, bits);
    return true;

signed_overflow:
    mcc_error_at(sema->ctx, expr->location,
                 "signed overflow in constant expression");
    return false;
}

static bool eval_const_bits(mcc_sema_t *sema, mcc_ast_node_t *expr,
                            uint64_t *result)
{
    if (!expr || !result) return false;
    switch (expr->kind) {
        case AST_INT_LIT:
            *result = normalize(expr->data.int_lit.value,
                                integer_bits(sema, expr->type));
            return true;
        case AST_CHAR_LIT:
            *result = normalize((uint64_t)(int64_t)expr->data.char_lit.value,
                                integer_bits(sema, expr->type));
            return true;
        case AST_BINARY_EXPR:
            return eval_binary(sema, expr, result);
        case AST_UNARY_EXPR: {
            uint64_t raw;
            if (expr->data.unary_expr.op == UNOP_ADDR) {
                if (!eval_offset_address(sema,
                        expr->data.unary_expr.operand, &raw)) return false;
                *result = normalize(raw, integer_bits(sema, expr->type));
                return true;
            }
            if (!eval_const_bits(sema, expr->data.unary_expr.operand, &raw))
                return false;
            unsigned bits = integer_bits(sema, expr->type);
            raw = normalize(raw, bits);
            switch (expr->data.unary_expr.op) {
                case UNOP_POS: break;
                case UNOP_NOT: raw = !raw; break;
                case UNOP_BIT_NOT: raw = ~raw; break;
                case UNOP_NEG:
                    if (!integer_unsigned(expr->type) &&
                        signed_value(raw, bits) == signed_minimum(bits)) {
                        mcc_error_at(sema->ctx, expr->location,
                                     "signed overflow in constant expression");
                        return false;
                    }
                    raw = UINT64_C(0) - raw;
                    break;
                default: return false;
            }
            *result = normalize(raw, bits);
            return true;
        }
        case AST_TERNARY_EXPR: {
            uint64_t cond;
            if (!eval_const_bits(sema, expr->data.ternary_expr.cond, &cond))
                return false;
            return eval_const_bits(sema, cond
                ? expr->data.ternary_expr.then_expr
                : expr->data.ternary_expr.else_expr, result);
        }
        case AST_CAST_EXPR: {
            uint64_t raw;
            if (!eval_const_bits(sema, expr->data.cast_expr.expr, &raw))
                return false;
            mcc_type_t *target = unwrap_type(expr->data.cast_expr.target_type);
            if (!target || !mcc_type_is_integer(target)) return false;
            if (target->kind == TYPE_BOOL) raw = !!raw;
            *result = convert_integer(sema, raw,
                expr->data.cast_expr.expr->type, integer_bits(sema, target));
            return true;
        }
        case AST_SIZEOF_EXPR: {
            mcc_type_t *type = expr->data.sizeof_expr.type_arg;
            if (!type && expr->data.sizeof_expr.expr_arg)
                type = expr->data.sizeof_expr.expr_arg->type;
            if (!type) return false;
            *result = mcc_type_sizeof(type);
            return true;
        }
        case AST_ALIGNOF_EXPR: {
            mcc_type_t *type = expr->data.alignof_expr.type_arg;
            if (!type && expr->data.alignof_expr.expr_arg)
                type = expr->data.alignof_expr.expr_arg->type;
            if (!type) return false;
            *result = mcc_type_alignof(type);
            return true;
        }
        case AST_IDENT_EXPR: {
            mcc_symbol_t *sym = mcc_symtab_lookup(
                sema->symtab, expr->data.ident_expr.name);
            if (!sym || sym->kind != SYM_ENUM_CONST) return false;
            *result = normalize((uint64_t)(int64_t)sym->data.enum_value,
                                integer_bits(sema, expr->type));
            return true;
        }
        case AST_COMMA_EXPR: {
            uint64_t ignored;
            if (!eval_const_bits(sema, expr->data.comma_expr.left, &ignored))
                return false;
            return eval_const_bits(sema, expr->data.comma_expr.right, result);
        }
        default:
            return false;
    }
}

bool sema_eval_const_expr(mcc_sema_t *sema, mcc_ast_node_t *expr,
                          int64_t *result)
{
    uint64_t raw;
    if (!sema || !result || !eval_const_bits(sema, expr, &raw)) return false;
    *result = signed_value(raw, integer_bits(sema, expr->type));
    return true;
}
