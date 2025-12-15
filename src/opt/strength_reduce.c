/*
 * ANVIL - Strength Reduction Pass
 * 
 * Replaces expensive operations with cheaper equivalents:
 * - Multiplication by power of 2 -> shift left
 * - Division by power of 2 -> shift right (unsigned) or special handling (signed)
 * - Modulo by power of 2 -> bitwise AND (unsigned)
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Check if a value is a constant power of 2 */
static bool is_power_of_2(anvil_value_t *val, int *shift)
{
    if (!val || val->kind != ANVIL_VAL_CONST_INT) return false;
    
    int64_t v = val->data.i;
    if (v <= 0) return false;
    
    /* Check if only one bit is set */
    if ((v & (v - 1)) != 0) return false;
    
    /* Calculate shift amount */
    int s = 0;
    while ((v >> s) != 1) s++;
    
    *shift = s;
    return true;
}

/* Create a constant for shift amount */
static anvil_value_t *make_shift_const(anvil_ctx_t *ctx, anvil_type_t *type, int shift)
{
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return anvil_const_i8(ctx, (int8_t)shift);
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return anvil_const_i16(ctx, (int16_t)shift);
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
            return anvil_const_i32(ctx, (int32_t)shift);
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
            return anvil_const_i64(ctx, (int64_t)shift);
        default:
            return anvil_const_i32(ctx, (int32_t)shift);
    }
}

/* Create a mask constant for modulo (2^n - 1) */
static anvil_value_t *make_mask_const(anvil_ctx_t *ctx, anvil_type_t *type, int shift)
{
    int64_t mask = (1LL << shift) - 1;
    
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return anvil_const_i8(ctx, (int8_t)mask);
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return anvil_const_i16(ctx, (int16_t)mask);
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
            return anvil_const_i32(ctx, (int32_t)mask);
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
            return anvil_const_i64(ctx, mask);
        default:
            return anvil_const_i32(ctx, (int32_t)mask);
    }
}

/* Check if type is signed */
static bool is_signed_type(anvil_type_t *type)
{
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_I64:
            return true;
        default:
            return false;
    }
}

/* Strength reduction pass */
bool anvil_pass_strength_reduce(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx) return false;
    
    anvil_ctx_t *ctx = func->parent->ctx;
    bool changed = false;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            if (instr->num_operands < 2) continue;
            
            anvil_value_t *lhs = instr->operands[0];
            anvil_value_t *rhs = instr->operands[1];
            int shift;
            
            switch (instr->op) {
                case ANVIL_OP_MUL:
                    /* x * 2^n -> x << n */
                    if (is_power_of_2(rhs, &shift)) {
                        instr->op = ANVIL_OP_SHL;
                        instr->operands[1] = make_shift_const(ctx, lhs->type, shift);
                        changed = true;
                    } else if (is_power_of_2(lhs, &shift)) {
                        /* 2^n * x -> x << n */
                        instr->op = ANVIL_OP_SHL;
                        instr->operands[0] = rhs;
                        instr->operands[1] = make_shift_const(ctx, rhs->type, shift);
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_UDIV:
                    /* x / 2^n -> x >> n (unsigned) */
                    if (is_power_of_2(rhs, &shift)) {
                        instr->op = ANVIL_OP_SHR;
                        instr->operands[1] = make_shift_const(ctx, lhs->type, shift);
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_SDIV:
                    /* For signed division by power of 2, we can only optimize
                     * if we know the dividend is non-negative, which we can't
                     * easily determine. Skip for now. */
                    break;
                    
                case ANVIL_OP_UMOD:
                    /* x % 2^n -> x & (2^n - 1) (unsigned) */
                    if (is_power_of_2(rhs, &shift)) {
                        instr->op = ANVIL_OP_AND;
                        instr->operands[1] = make_mask_const(ctx, lhs->type, shift);
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_SMOD:
                    /* Signed modulo is more complex, skip for now */
                    break;
                    
                default:
                    break;
            }
        }
    }
    
    return changed;
}
