/*
 * ANVIL - Constant Folding Pass
 * 
 * Evaluates constant expressions at compile time.
 * Examples:
 *   - add 3, 5 -> 8
 *   - mul x, 0 -> 0
 *   - add x, 0 -> x
 *   - mul x, 1 -> x
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Local aliases to keep existing call sites readable. */
#define is_const_int      anvil_opt_is_const_int
#define is_const_float    anvil_opt_is_const_float
#define get_const_int     anvil_opt_get_const_int
#define get_const_float   anvil_opt_get_const_float
#define is_zero           anvil_opt_is_zero
#define is_one            anvil_opt_is_one
#define is_all_ones       anvil_opt_is_all_ones
#define make_const_int    anvil_opt_make_const_int
#define make_const_float  anvil_opt_make_const_float

/* Mark instruction for deletion by setting op to NOP */
static void mark_dead(anvil_instr_t *instr)
{
    instr->op = ANVIL_OP_NOP;
}

static unsigned type_int_bits(const anvil_type_t *type)
{
    if (!type) return 64;

    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 8;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 16;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
            return 32;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
            return 64;
        default:
            break;
    }

    size_t size = anvil_type_size((anvil_type_t *)type);
    if (size == 0) return 64;
    if (size > SIZE_MAX / 8) return 64;

    size_t bits = size * 8;
    if (bits == 0) return 64;
    if (bits > 64) return 64;
    return (unsigned)bits;
}

static uint64_t mask_for_bits(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
}

static bool valid_shift_amount(int64_t amount, unsigned bits)
{
    return amount >= 0 && (uint64_t)amount < bits;
}

static int64_t signed_min_for_bits(unsigned bits)
{
    if (bits >= 64) return INT64_MIN;
    return -(INT64_C(1) << (bits - 1));
}

static int64_t sign_extend_to_bits(uint64_t value, unsigned bits)
{
    uint64_t mask = mask_for_bits(bits);
    value &= mask;
    if (bits < 64 && (value & (UINT64_C(1) << (bits - 1)))) {
        value |= ~mask;
    }
    return (int64_t)value;
}

static anvil_value_t *fold_shift(anvil_ctx_t *ctx, anvil_op_t op,
                                 int64_t lhs, int64_t rhs,
                                 anvil_type_t *type)
{
    unsigned bits = type_int_bits(type);
    if (!valid_shift_amount(rhs, bits)) return NULL;

    unsigned amount = (unsigned)rhs;
    uint64_t mask = mask_for_bits(bits);
    uint64_t value = ((uint64_t)lhs) & mask;
    uint64_t result;

    switch (op) {
        case ANVIL_OP_SHL:
            result = (value << amount) & mask;
            break;
        case ANVIL_OP_SHR:
            result = value >> amount;
            break;
        case ANVIL_OP_SAR:
            result = value >> amount;
            if (amount > 0 && (value & (UINT64_C(1) << (bits - 1)))) {
                result |= mask ^ (mask >> amount);
            }
            result &= mask;
            break;
        default:
            return NULL;
    }

    return make_const_int(ctx, type, (int64_t)result);
}

/* Try to fold a binary integer operation */
static anvil_value_t *try_fold_binop_int(anvil_ctx_t *ctx, anvil_op_t op,
                                          anvil_value_t *lhs, anvil_value_t *rhs,
                                          anvil_type_t *type)
{
    /* Both operands must be constants for full folding */
    if (is_const_int(lhs) && is_const_int(rhs)) {
        int64_t a = get_const_int(lhs);
        int64_t b = get_const_int(rhs);
        int64_t result;
        unsigned bits = type_int_bits(type);
        uint64_t mask = mask_for_bits(bits);
        
        switch (op) {
            case ANVIL_OP_ADD:
                result = (int64_t)((((uint64_t)a) + ((uint64_t)b)) & mask);
                break;
            case ANVIL_OP_SUB:
                result = (int64_t)((((uint64_t)a) - ((uint64_t)b)) & mask);
                break;
            case ANVIL_OP_MUL:
                result = (int64_t)((((uint64_t)a) * ((uint64_t)b)) & mask);
                break;
            case ANVIL_OP_SDIV:
                a = sign_extend_to_bits((uint64_t)a, bits);
                b = sign_extend_to_bits((uint64_t)b, bits);
                if (b == 0 || (a == signed_min_for_bits(bits) && b == -1)) return NULL;
                result = a / b;
                break;
            case ANVIL_OP_UDIV:
                if ((((uint64_t)b) & mask) == 0) return NULL;
                result = (int64_t)((((uint64_t)a) & mask) / (((uint64_t)b) & mask));
                break;
            case ANVIL_OP_SMOD:
                a = sign_extend_to_bits((uint64_t)a, bits);
                b = sign_extend_to_bits((uint64_t)b, bits);
                if (b == 0 || (a == signed_min_for_bits(bits) && b == -1)) return NULL;
                result = a % b;
                break;
            case ANVIL_OP_UMOD:
                if ((((uint64_t)b) & mask) == 0) return NULL;
                result = (int64_t)((((uint64_t)a) & mask) % (((uint64_t)b) & mask));
                break;
            case ANVIL_OP_AND:  result = a & b; break;
            case ANVIL_OP_OR:   result = a | b; break;
            case ANVIL_OP_XOR:  result = a ^ b; break;
            case ANVIL_OP_SHL:
            case ANVIL_OP_SHR:
            case ANVIL_OP_SAR:
                return fold_shift(ctx, op, a, b, type);
            default: return NULL;
        }
        
        return make_const_int(ctx, type, result);
    }
    
    /* Algebraic identities with one constant */
    switch (op) {
        case ANVIL_OP_ADD:
            /* x + 0 = x */
            if (is_zero(rhs)) return lhs;
            if (is_zero(lhs)) return rhs;
            break;
            
        case ANVIL_OP_SUB:
            /* x - 0 = x */
            if (is_zero(rhs)) return lhs;
            /* x - x = 0 */
            if (lhs == rhs) return make_const_int(ctx, type, 0);
            break;
            
        case ANVIL_OP_MUL:
            /* x * 0 = 0 */
            if (is_zero(lhs) || is_zero(rhs)) return make_const_int(ctx, type, 0);
            /* x * 1 = x */
            if (is_one(rhs)) return lhs;
            if (is_one(lhs)) return rhs;
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            /* x / 1 = x */
            if (is_one(rhs)) return lhs;
            /* 0 / x is only safe when x is known non-zero. */
            if (is_zero(lhs) && is_const_int(rhs) && get_const_int(rhs) != 0)
                return make_const_int(ctx, type, 0);
            break;
            
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
            /* x % 1 = 0 */
            if (is_one(rhs)) return make_const_int(ctx, type, 0);
            /* 0 % x is only safe when x is known non-zero. */
            if (is_zero(lhs) && is_const_int(rhs) && get_const_int(rhs) != 0)
                return make_const_int(ctx, type, 0);
            break;
            
        case ANVIL_OP_AND:
            /* x & 0 = 0 */
            if (is_zero(lhs) || is_zero(rhs)) return make_const_int(ctx, type, 0);
            /* x & -1 = x */
            if (is_all_ones(rhs)) return lhs;
            if (is_all_ones(lhs)) return rhs;
            /* x & x = x */
            if (lhs == rhs) return lhs;
            break;
            
        case ANVIL_OP_OR:
            /* x | 0 = x */
            if (is_zero(rhs)) return lhs;
            if (is_zero(lhs)) return rhs;
            /* x | -1 = -1 */
            if (is_all_ones(rhs)) return rhs;
            if (is_all_ones(lhs)) return lhs;
            /* x | x = x */
            if (lhs == rhs) return lhs;
            break;
            
        case ANVIL_OP_XOR:
            /* x ^ 0 = x */
            if (is_zero(rhs)) return lhs;
            if (is_zero(lhs)) return rhs;
            /* x ^ x = 0 */
            if (lhs == rhs) return make_const_int(ctx, type, 0);
            break;
            
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            /* x << 0 = x, x >> 0 = x */
            if (is_zero(rhs)) return lhs;
            /* 0 << x = 0, 0 >> x = 0 only if the shift amount is valid. */
            if (is_zero(lhs) && is_const_int(rhs) &&
                valid_shift_amount(get_const_int(rhs), type_int_bits(type))) {
                return make_const_int(ctx, type, 0);
            }
            break;
            
        default:
            break;
    }
    
    return NULL;
}

/* Try to fold a binary float operation */
static anvil_value_t *try_fold_binop_float(anvil_ctx_t *ctx, anvil_op_t op,
                                            anvil_value_t *lhs, anvil_value_t *rhs,
                                            anvil_type_t *type)
{
    /* Both operands must be constants for full folding */
    if (is_const_float(lhs) && is_const_float(rhs)) {
        double a = get_const_float(lhs);
        double b = get_const_float(rhs);
        double result;
        
        switch (op) {
            case ANVIL_OP_FADD: result = a + b; break;
            case ANVIL_OP_FSUB: result = a - b; break;
            case ANVIL_OP_FMUL: result = a * b; break;
            case ANVIL_OP_FDIV:
                if (b == 0.0) return NULL;
                result = a / b;
                break;
            default: return NULL;
        }
        
        return make_const_float(ctx, type, result);
    }
    
    /* Algebraic identities */
    switch (op) {
        case ANVIL_OP_FADD:
            break;
            
        case ANVIL_OP_FSUB:
            if (is_zero(rhs)) return lhs;
            break;
            
        case ANVIL_OP_FMUL:
            if (is_one(rhs)) return lhs;
            if (is_one(lhs)) return rhs;
            break;
            
        case ANVIL_OP_FDIV:
            if (is_one(rhs)) return lhs;
            break;
            
        default:
            break;
    }
    
    return NULL;
}

/* Try to fold comparison operations */
static anvil_value_t *try_fold_cmp(anvil_ctx_t *ctx, anvil_op_t op,
                                    anvil_value_t *lhs, anvil_value_t *rhs)
{
    /* x cmp x */
    if (lhs == rhs) {
        switch (op) {
            case ANVIL_OP_CMP_EQ:
            case ANVIL_OP_CMP_LE:
            case ANVIL_OP_CMP_GE:
            case ANVIL_OP_CMP_ULE:
            case ANVIL_OP_CMP_UGE:
                return anvil_const_i8(ctx, 1);  /* true */
                
            case ANVIL_OP_CMP_NE:
            case ANVIL_OP_CMP_LT:
            case ANVIL_OP_CMP_GT:
            case ANVIL_OP_CMP_ULT:
            case ANVIL_OP_CMP_UGT:
                return anvil_const_i8(ctx, 0);  /* false */
                
            default:
                break;
        }
    }
    
    /* Constant comparison */
    if (is_const_int(lhs) && is_const_int(rhs)) {
        int64_t a = get_const_int(lhs);
        int64_t b = get_const_int(rhs);
        uint64_t ua = (uint64_t)a;
        uint64_t ub = (uint64_t)b;
        bool result;
        
        switch (op) {
            case ANVIL_OP_CMP_EQ:  result = (a == b); break;
            case ANVIL_OP_CMP_NE:  result = (a != b); break;
            case ANVIL_OP_CMP_LT:  result = (a < b); break;
            case ANVIL_OP_CMP_LE:  result = (a <= b); break;
            case ANVIL_OP_CMP_GT:  result = (a > b); break;
            case ANVIL_OP_CMP_GE:  result = (a >= b); break;
            case ANVIL_OP_CMP_ULT: result = (ua < ub); break;
            case ANVIL_OP_CMP_ULE: result = (ua <= ub); break;
            case ANVIL_OP_CMP_UGT: result = (ua > ub); break;
            case ANVIL_OP_CMP_UGE: result = (ua >= ub); break;
            default: return NULL;
        }
        
        return anvil_const_i8(ctx, result ? 1 : 0);
    }
    
    return NULL;
}

/* Try to fold unary operations */
static anvil_value_t *try_fold_unop(anvil_ctx_t *ctx, anvil_op_t op,
                                     anvil_value_t *val, anvil_type_t *type)
{
    if (is_const_int(val)) {
        int64_t v = get_const_int(val);
        
        switch (op) {
            case ANVIL_OP_NEG: return make_const_int(ctx, type, -v);
            case ANVIL_OP_NOT: return make_const_int(ctx, type, ~v);
            default: break;
        }
    }
    
    if (is_const_float(val)) {
        double v = get_const_float(val);
        
        switch (op) {
            case ANVIL_OP_FNEG: return make_const_float(ctx, type, -v);
            case ANVIL_OP_FABS: return make_const_float(ctx, type, v < 0 ? -v : v);
            default: break;
        }
    }
    
    return NULL;
}

/* Main constant folding pass */
bool anvil_pass_const_fold(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx) return false;
    
    anvil_ctx_t *ctx = func->parent->ctx;
    bool changed = false;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            if (!instr->result) continue;
            
            anvil_value_t *folded = NULL;
            
            /* Binary operations */
            if (instr->num_operands == 2) {
                anvil_value_t *lhs = instr->operands[0];
                anvil_value_t *rhs = instr->operands[1];
                
                switch (instr->op) {
                    case ANVIL_OP_ADD:
                    case ANVIL_OP_SUB:
                    case ANVIL_OP_MUL:
                    case ANVIL_OP_SDIV:
                    case ANVIL_OP_UDIV:
                    case ANVIL_OP_SMOD:
                    case ANVIL_OP_UMOD:
                    case ANVIL_OP_AND:
                    case ANVIL_OP_OR:
                    case ANVIL_OP_XOR:
                    case ANVIL_OP_SHL:
                    case ANVIL_OP_SHR:
                    case ANVIL_OP_SAR:
                        folded = try_fold_binop_int(ctx, instr->op, lhs, rhs, instr->result->type);
                        break;
                        
                    case ANVIL_OP_FADD:
                    case ANVIL_OP_FSUB:
                    case ANVIL_OP_FMUL:
                    case ANVIL_OP_FDIV:
                        folded = try_fold_binop_float(ctx, instr->op, lhs, rhs, instr->result->type);
                        break;
                        
                    case ANVIL_OP_CMP_EQ:
                    case ANVIL_OP_CMP_NE:
                    case ANVIL_OP_CMP_LT:
                    case ANVIL_OP_CMP_LE:
                    case ANVIL_OP_CMP_GT:
                    case ANVIL_OP_CMP_GE:
                    case ANVIL_OP_CMP_ULT:
                    case ANVIL_OP_CMP_ULE:
                    case ANVIL_OP_CMP_UGT:
                    case ANVIL_OP_CMP_UGE:
                        folded = try_fold_cmp(ctx, instr->op, lhs, rhs);
                        break;
                        
                    default:
                        break;
                }
            }
            /* Unary operations */
            else if (instr->num_operands == 1) {
                anvil_value_t *val = instr->operands[0];
                
                switch (instr->op) {
                    case ANVIL_OP_NEG:
                    case ANVIL_OP_NOT:
                    case ANVIL_OP_FNEG:
                    case ANVIL_OP_FABS:
                        folded = try_fold_unop(ctx, instr->op, val, instr->result->type);
                        break;
                        
                    default:
                        break;
                }
            }
            
            /* Replace uses if we folded something */
            if (folded) {
                anvil_opt_replace_uses_in_func(func, instr->result, folded);
                mark_dead(instr);
                changed = true;
            }
        }
    }
    
    return changed;
}
