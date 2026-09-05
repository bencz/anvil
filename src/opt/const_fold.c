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
#include <math.h>
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

/* Remove a folded instruction after its result has been replaced. */
static void mark_dead(anvil_instr_t *instr)
{
    anvil_opt_erase_instr(instr);
}

static unsigned type_int_bits(const anvil_type_t *type)
{
    if (!type) return 64;

    switch (type->kind) {
        case ANVIL_TYPE_I1:
            return 1;
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
    (void)ctx;
    (void)op;
    (void)lhs;
    (void)rhs;
    (void)type;

    /* The IR currently has strict FP semantics and no fast-math/no-sNaN or
     * exception flags. Host evaluation would erase target exceptions, compute
     * f32 through double (which can double-round), and is not an evaluator for
     * IBM HFP. Even x*1 and x/1 can observably quiet an sNaN. Leave all binary
     * FP operations intact until the target-format evaluator and formal flags
     * exist. */
    return NULL;
}

/* Try to fold comparison operations */
static anvil_value_t *try_fold_cmp(anvil_ctx_t *ctx, anvil_op_t op,
                                    anvil_value_t *lhs, anvil_value_t *rhs)
{
    if ((op == ANVIL_OP_CMP_NE && is_zero(rhs)) || (op == ANVIL_OP_CMP_EQ && is_one(rhs)))
    {
        if (lhs->kind == ANVIL_VAL_INSTR && lhs->data.instr->op == ANVIL_OP_ZEXT &&
            lhs->data.instr->operands[0]->type->kind == ANVIL_TYPE_I1)
            return lhs->data.instr->operands[0];
    }

    /* x cmp x */
    if (lhs == rhs) {
        switch (op) {
            case ANVIL_OP_CMP_EQ:
            case ANVIL_OP_CMP_LE:
            case ANVIL_OP_CMP_GE:
            case ANVIL_OP_CMP_ULE:
            case ANVIL_OP_CMP_UGE:
                return anvil_const_i1(ctx, true);
                
            case ANVIL_OP_CMP_NE:
            case ANVIL_OP_CMP_LT:
            case ANVIL_OP_CMP_GT:
            case ANVIL_OP_CMP_ULT:
            case ANVIL_OP_CMP_UGT:
                return anvil_const_i1(ctx, false);
                
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
        
        return anvil_const_i1(ctx, result);
    }
    
    return NULL;
}

static anvil_value_t *try_fold_fcmp(anvil_ctx_t *ctx,
                                     anvil_fcmp_pred_t predicate,
                                     anvil_value_t *lhs,
                                     anvil_value_t *rhs)
{
    if (!is_const_float(lhs) || !is_const_float(rhs)) return NULL;
    if (anvil_ctx_get_fp_format(ctx) != ANVIL_FP_IEEE754) return NULL;

    double a = get_const_float(lhs);
    double b = get_const_float(rhs);
    bool unordered = isnan(a) || isnan(b);
    bool ordered = !unordered;
    bool result;

    switch (predicate) {
        case ANVIL_FCMP_FALSE: result = false; break;
        case ANVIL_FCMP_OEQ: result = ordered && a == b; break;
        case ANVIL_FCMP_OGT: result = ordered && a > b; break;
        case ANVIL_FCMP_OGE: result = ordered && a >= b; break;
        case ANVIL_FCMP_OLT: result = ordered && a < b; break;
        case ANVIL_FCMP_OLE: result = ordered && a <= b; break;
        case ANVIL_FCMP_ONE: result = ordered && a != b; break;
        case ANVIL_FCMP_ORD: result = ordered; break;
        case ANVIL_FCMP_UEQ: result = unordered || a == b; break;
        case ANVIL_FCMP_UGT: result = unordered || a > b; break;
        case ANVIL_FCMP_UGE: result = unordered || a >= b; break;
        case ANVIL_FCMP_ULT: result = unordered || a < b; break;
        case ANVIL_FCMP_ULE: result = unordered || a <= b; break;
        case ANVIL_FCMP_UNE: result = unordered || a != b; break;
        case ANVIL_FCMP_UNO: result = unordered; break;
        case ANVIL_FCMP_TRUE: result = true; break;
        default: return NULL;
    }
    return anvil_const_i1(ctx, result);
}

/* Try to fold unary operations */
static anvil_value_t *try_fold_unop(anvil_ctx_t *ctx, anvil_op_t op,
                                     anvil_value_t *val, anvil_type_t *type)
{
    if (is_const_int(val)) {
        int64_t v = get_const_int(val);
        
        switch (op) {
            case ANVIL_OP_NEG: {
                unsigned bits = type_int_bits(type);
                uint64_t result = (UINT64_C(0) - (uint64_t)v) &
                                  mask_for_bits(bits);
                return make_const_int(ctx, type, (int64_t)result);
            }
            case ANVIL_OP_NOT: return make_const_int(ctx, type, ~v);
            default: break;
        }
    }
    
    if (is_const_float(val)) {
        /* HFP is not host IEEE. Sign-only folds are enabled only when the
         * selected target format is unambiguously IEEE 754. */
        if (anvil_ctx_get_fp_format(ctx) != ANVIL_FP_IEEE754) return NULL;
        double v = get_const_float(val);
        
        switch (op) {
            case ANVIL_OP_FNEG: return make_const_float(ctx, type, -v);
            case ANVIL_OP_FABS: return make_const_float(ctx, type, fabs(v));
            default: break;
        }
    }
    
    return NULL;
}

anvil_value_t *anvil_opt_fold_integer(anvil_ctx_t *ctx, const anvil_instr_t *instr, anvil_value_t *left, anvil_value_t *right)
{
    if (!instr || !instr->result || !is_const_int(left))
        return NULL;

    switch (instr->op)
    {
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
            return try_fold_binop_int(ctx, instr->op, left, right, instr->result->type);
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
            return try_fold_cmp(ctx, instr->op, left, right);
        case ANVIL_OP_NEG:
        case ANVIL_OP_NOT:
            return try_fold_unop(ctx, instr->op, left, instr->result->type);
        case ANVIL_OP_TRUNC:
        case ANVIL_OP_ZEXT:
        case ANVIL_OP_SEXT:
        {
            unsigned source_bits = type_int_bits(left->type);
            unsigned target_bits = type_int_bits(instr->result->type);
            if (!source_bits || !target_bits)
                return NULL;

            uint64_t bits = left->data.u & mask_for_bits(source_bits);
            if (instr->op == ANVIL_OP_SEXT)
                bits = (uint64_t)sign_extend_to_bits(bits, source_bits);

            bits &= mask_for_bits(target_bits);
            if (target_bits == 1)
                return anvil_const_i1(ctx, bits != 0);

            return make_const_int(ctx, instr->result->type, (int64_t)bits);
        }
        default:
            return NULL;
    }
}

/* Main constant folding pass */
anvil_pass_result_t anvil_pass_const_fold(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
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

                    case ANVIL_OP_FCMP:
                        folded = try_fold_fcmp(ctx, instr->fcmp_pred, lhs, rhs);
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
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
