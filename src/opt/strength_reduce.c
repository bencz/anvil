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
#include "opt_utils.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Check if a value is a constant power of 2 */
static bool is_power_of_2(anvil_value_t *val, int *shift)
{
    if (!val || val->kind != ANVIL_VAL_CONST_INT || !val->type || !shift) {
        return false;
    }

    unsigned bits;
    switch (val->type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:  bits = 8;  break;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16: bits = 16; break;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32: bits = 32; break;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64: bits = 64; break;
        default: return false;
    }

    uint64_t mask = bits == 64 ? UINT64_MAX
                               : ((UINT64_C(1) << bits) - 1);
    uint64_t v = anvil_const_int_unsigned_value(val) & mask;
    if (v == 0) return false;

    /* Check if only one bit is set */
    if ((v & (v - 1)) != 0) return false;
    
    /* Calculate shift amount */
    int s = 0;
    while ((v >> s) != 1) s++;
    
    *shift = s;
    return true;
}

/* Create a constant for shift amount */
static anvil_value_t *make_shift_const(anvil_ctx_t *ctx, anvil_type_t *type,
                                       int shift)
{
    return anvil_opt_make_const_int(ctx, type, shift);
}

/* Create a mask constant for modulo (2^n - 1) */
static anvil_value_t *make_mask_const(anvil_ctx_t *ctx, anvil_type_t *type, int shift)
{
    uint64_t mask = (UINT64_C(1) << (unsigned)shift) - 1;
    return anvil_opt_make_const_int(ctx, type, (int64_t)mask);
}

static anvil_instr_t *prepare_binary(anvil_ctx_t *ctx, anvil_op_t op, anvil_type_t *type, anvil_value_t *left, anvil_value_t *right)
{
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, "division.reduce");
    if (!instr || !anvil_instr_add_operand(instr, left) || !anvil_instr_add_operand(instr, right))
        return NULL;

    return instr;
}

static void insert_before(anvil_instr_t *position, anvil_instr_t *instr)
{
    instr->parent = position->parent;
    instr->owner_module = position->owner_module;
    instr->result->owner_module = position->owner_module;
    instr->prev = position->prev;
    instr->next = position;
    if (position->prev)
        position->prev->next = instr;
    else
        position->parent->first = instr;

    position->prev = instr;
}

static bool reduce_signed_power(anvil_ctx_t *ctx, anvil_instr_t *instr, unsigned shift)
{
    anvil_value_t *value = instr->operands[0];
    anvil_type_t *type = value->type;
    unsigned width = anvil_type_bit_width(type);
    if (!shift || shift >= width - 1)
        return false;

    anvil_value_t *sign_shift = make_shift_const(ctx, type, (int)(width - 1));
    anvil_value_t *amount = make_shift_const(ctx, type, (int)shift);
    anvil_value_t *mask = make_mask_const(ctx, type, (int)shift);
    if (!sign_shift || !amount || !mask)
        return false;

    /* Bias only negative dividends, preserving C-style truncation toward zero.
     * All nodes are prepared before touching the existing instruction chain. */
    anvil_instr_t *sign = prepare_binary(ctx, ANVIL_OP_SAR, type, value, sign_shift);
    if (!sign)
        return false;

    anvil_instr_t *bias = prepare_binary(ctx, ANVIL_OP_AND, type, sign->result, mask);
    if (!bias)
        return false;

    anvil_instr_t *biased = prepare_binary(ctx, ANVIL_OP_ADD, type, value, bias->result);
    if (!biased)
        return false;

    anvil_instr_t *quotient = NULL;
    anvil_instr_t *product = NULL;
    if (instr->op == ANVIL_OP_SMOD)
    {
        quotient = prepare_binary(ctx, ANVIL_OP_SAR, type, biased->result, amount);
        if (!quotient)
            return false;

        product = prepare_binary(ctx, ANVIL_OP_SHL, type, quotient->result, amount);
        if (!product)
            return false;
    }

    insert_before(instr, sign);
    insert_before(instr, bias);
    insert_before(instr, biased);
    if (product)
    {
        insert_before(instr, quotient);
        insert_before(instr, product);
        instr->op = ANVIL_OP_SUB;
        instr->operands[1] = product->result;
    }
    else
    {
        instr->op = ANVIL_OP_SAR;
        instr->operands[0] = biased->result;
        instr->operands[1] = amount;
    }

    return true;
}

/* Strength reduction pass */
anvil_pass_result_t anvil_pass_strength_reduce(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
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
                        anvil_value_t *amount = make_shift_const(
                            ctx, lhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHL;
                        instr->operands[1] = amount;
                        changed = true;
                    } else if (is_power_of_2(lhs, &shift)) {
                        /* 2^n * x -> x << n */
                        anvil_value_t *amount = make_shift_const(
                            ctx, rhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHL;
                        instr->operands[0] = rhs;
                        instr->operands[1] = amount;
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_UDIV:
                    /* x / 2^n -> x >> n (unsigned) */
                    if (is_power_of_2(rhs, &shift)) {
                        anvil_value_t *amount = make_shift_const(
                            ctx, lhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHR;
                        instr->operands[1] = amount;
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_SDIV:
                case ANVIL_OP_SMOD:
                    if (is_power_of_2(rhs, &shift) && reduce_signed_power(ctx, instr, (unsigned)shift))
                        changed = true;

                    break;
                    
                case ANVIL_OP_UMOD:
                    /* x % 2^n -> x & (2^n - 1) (unsigned) */
                    if (is_power_of_2(rhs, &shift)) {
                        anvil_value_t *mask = make_mask_const(
                            ctx, lhs->type, shift);
                        if (!mask) break;
                        instr->op = ANVIL_OP_AND;
                        instr->operands[1] = mask;
                        changed = true;
                    }
                    break;
                    
                default:
                    break;
            }
        }
    }
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
