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
#include <stdlib.h>
#include <string.h>

/* Check if a value is a constant integer */
static bool is_const_int(anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_INT;
}

/* Check if a value is a constant float */
static bool is_const_float(anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_FLOAT;
}

/* Get integer constant value */
static int64_t get_const_int(anvil_value_t *val)
{
    return val->data.i;
}

/* Get float constant value */
static double get_const_float(anvil_value_t *val)
{
    return val->data.f;
}

/* Check if constant is zero */
static bool is_zero(anvil_value_t *val)
{
    if (is_const_int(val)) return get_const_int(val) == 0;
    if (is_const_float(val)) return get_const_float(val) == 0.0;
    return false;
}

/* Check if constant is one */
static bool is_one(anvil_value_t *val)
{
    if (is_const_int(val)) return get_const_int(val) == 1;
    if (is_const_float(val)) return get_const_float(val) == 1.0;
    return false;
}

/* Check if constant is all ones (for bitwise ops) */
static bool is_all_ones(anvil_value_t *val)
{
    if (!is_const_int(val)) return false;
    int64_t v = get_const_int(val);
    return v == -1 || v == (int64_t)0xFFFFFFFFFFFFFFFFLL;
}

/* Replace all uses of old_val with new_val in the function */
static void replace_uses(anvil_func_t *func, anvil_value_t *old_val, anvil_value_t *new_val)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            for (size_t i = 0; i < instr->num_operands; i++) {
                if (instr->operands[i] == old_val) {
                    instr->operands[i] = new_val;
                }
            }
        }
    }
}

/* Mark instruction for deletion by setting op to NOP */
static void mark_dead(anvil_instr_t *instr)
{
    instr->op = ANVIL_OP_NOP;
}

/* Create a constant value with the same type */
static anvil_value_t *make_const_int(anvil_ctx_t *ctx, anvil_type_t *type, int64_t val)
{
    switch (type->kind) {
        case ANVIL_TYPE_I8:  return anvil_const_i8(ctx, (int8_t)val);
        case ANVIL_TYPE_I16: return anvil_const_i16(ctx, (int16_t)val);
        case ANVIL_TYPE_I32: return anvil_const_i32(ctx, (int32_t)val);
        case ANVIL_TYPE_I64: return anvil_const_i64(ctx, val);
        case ANVIL_TYPE_U8:  return anvil_const_u8(ctx, (uint8_t)val);
        case ANVIL_TYPE_U16: return anvil_const_u16(ctx, (uint16_t)val);
        case ANVIL_TYPE_U32: return anvil_const_u32(ctx, (uint32_t)val);
        case ANVIL_TYPE_U64: return anvil_const_u64(ctx, (uint64_t)val);
        default: return NULL;
    }
}

static anvil_value_t *make_const_float(anvil_ctx_t *ctx, anvil_type_t *type, double val)
{
    switch (type->kind) {
        case ANVIL_TYPE_F32: return anvil_const_f32(ctx, (float)val);
        case ANVIL_TYPE_F64: return anvil_const_f64(ctx, val);
        default: return NULL;
    }
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
        
        switch (op) {
            case ANVIL_OP_ADD:  result = a + b; break;
            case ANVIL_OP_SUB:  result = a - b; break;
            case ANVIL_OP_MUL:  result = a * b; break;
            case ANVIL_OP_SDIV: result = b ? a / b : 0; break;
            case ANVIL_OP_UDIV: result = b ? (int64_t)((uint64_t)a / (uint64_t)b) : 0; break;
            case ANVIL_OP_SMOD: result = b ? a % b : 0; break;
            case ANVIL_OP_UMOD: result = b ? (int64_t)((uint64_t)a % (uint64_t)b) : 0; break;
            case ANVIL_OP_AND:  result = a & b; break;
            case ANVIL_OP_OR:   result = a | b; break;
            case ANVIL_OP_XOR:  result = a ^ b; break;
            case ANVIL_OP_SHL:  result = a << b; break;
            case ANVIL_OP_SHR:  result = (int64_t)((uint64_t)a >> b); break;
            case ANVIL_OP_SAR:  result = a >> b; break;
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
            /* 0 / x = 0 */
            if (is_zero(lhs)) return make_const_int(ctx, type, 0);
            break;
            
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
            /* x % 1 = 0 */
            if (is_one(rhs)) return make_const_int(ctx, type, 0);
            /* 0 % x = 0 */
            if (is_zero(lhs)) return make_const_int(ctx, type, 0);
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
            /* 0 << x = 0, 0 >> x = 0 */
            if (is_zero(lhs)) return make_const_int(ctx, type, 0);
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
            case ANVIL_OP_FDIV: result = b != 0.0 ? a / b : 0.0; break;
            default: return NULL;
        }
        
        return make_const_float(ctx, type, result);
    }
    
    /* Algebraic identities */
    switch (op) {
        case ANVIL_OP_FADD:
            if (is_zero(rhs)) return lhs;
            if (is_zero(lhs)) return rhs;
            break;
            
        case ANVIL_OP_FSUB:
            if (is_zero(rhs)) return lhs;
            break;
            
        case ANVIL_OP_FMUL:
            if (is_zero(lhs) || is_zero(rhs)) return make_const_float(ctx, type, 0.0);
            if (is_one(rhs)) return lhs;
            if (is_one(lhs)) return rhs;
            break;
            
        case ANVIL_OP_FDIV:
            if (is_one(rhs)) return lhs;
            if (is_zero(lhs)) return make_const_float(ctx, type, 0.0);
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
                replace_uses(func, instr->result, folded);
                mark_dead(instr);
                changed = true;
            }
        }
    }
    
    return changed;
}
