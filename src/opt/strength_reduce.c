/*
 * ANVIL - Strength Reduction Pass
 * 
 * Replaces expensive operations with cheaper equivalents:
 * - Multiplication by power of 2 -> shift left
 * - Division by power of 2 -> shift right (unsigned) or special handling (signed)
 * - Modulo by power of 2 -> bitwise AND (unsigned)
 * - Division and modulo by other constants -> multiply-high by the
 *   reciprocal, where the target has it
 * - (x << k) | (x >> (width - k)) -> rotate left, where the target has it
 * - a*b + c -> fused multiply-add, where the function permits contraction
 *   and the target has it
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "anvil/anvil_analysis.h"
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

    anvil_opt_insert_before(instr, sign);
    anvil_opt_insert_before(instr, bias);
    anvil_opt_insert_before(instr, biased);
    if (product)
    {
        anvil_opt_insert_before(instr, quotient);
        anvil_opt_insert_before(instr, product);
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

/* Division by a constant d that is no power of two, as a multiply by its
 * scaled reciprocal (Granlund and Montgomery; the magic numbers are those
 * of Hacker's Delight 10-1 and 10-2 at the operand width N).  Unsigned:
 * q = UMULH(n, m) >> s or, when m needs N + 1 bits ("add"),
 * q = (((n - t) >> 1) + t) >> (s - 1) with t = UMULH(n, m). */
typedef struct {
    uint64_t multiplier;
    unsigned shift;
    bool add;
} division_magic_t;

static division_magic_t unsigned_magic(uint64_t d, unsigned bits)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t signed_max = mask >> 1, signed_min = signed_max + 1;
    division_magic_t magic = {0};
    unsigned p = bits - 1;
    uint64_t q = signed_max / d, r = signed_max - q * d, scale = 0, delta;
    do {
        p++;
        scale = p == bits ? 1 : scale * 2;
        if (r + 1 >= d - r) {
            magic.add |= q >= signed_max;
            q = (2 * q + 1) & mask;
            r = 2 * r + 1 - d;
        } else {
            magic.add |= q >= signed_min;
            q = (2 * q) & mask;
            r = 2 * r + 1;
        }
        delta = d - 1 - r;
    } while (p < 2 * bits && scale < delta);
    magic.multiplier = (q + 1) & mask;
    magic.shift = p - bits;
    return magic;
}

/* Signed (|d| >= 2): q = SMULH(n, m), plus n when d > 0 and m reads
 * negative, minus n when d < 0 and m reads positive, shifted arithmetically
 * by s, plus one when negative (C truncates toward zero). */
static division_magic_t signed_magic(int64_t d, unsigned bits)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t two = UINT64_C(1) << (bits - 1);
    uint64_t ad = d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
    uint64_t t = two + (d < 0);
    uint64_t anc = t - 1 - t % ad;
    unsigned p = bits - 1;
    uint64_t q1 = two / anc, r1 = two - q1 * anc, q2 = two / ad, r2 = two - q2 * ad, delta;
    do {
        p++;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) {
            q1++;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) {
            q2++;
            r2 -= ad;
        }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    division_magic_t magic = {.multiplier = (d < 0 ? 0 - (q2 + 1) : q2 + 1) & mask, .shift = p - bits};
    return magic;
}

/* The steps of one rewrite, inserted together once all exist. */
typedef struct {
    anvil_ctx_t *ctx;
    anvil_type_t *type;
    anvil_instr_t *steps[8];
    size_t count;
} division_steps_t;

static anvil_value_t *division_step(division_steps_t *steps, anvil_op_t op, anvil_value_t *left, anvil_value_t *right)
{
    anvil_instr_t *step = left && right && steps->count < 8 ? prepare_binary(steps->ctx, op, steps->type, left, right) : NULL;
    if (!step)
        return NULL;
    steps->steps[steps->count++] = step;
    return step->result;
}

static anvil_value_t *division_constant(division_steps_t *steps, uint64_t value)
{
    return anvil_opt_make_const_int(steps->ctx, steps->type, (int64_t)value);
}

static bool reduce_constant_division(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    anvil_value_t *n = instr->operands[0];
    anvil_type_t *type = n->type;
    bool is_signed = instr->op == ANVIL_OP_SDIV || instr->op == ANVIL_OP_SMOD;
    bool remainder = instr->op == ANVIL_OP_SMOD || instr->op == ANVIL_OP_UMOD;
    unsigned bits = type ? anvil_type_bit_width(type) : 0;
    if (!type || type->kind == ANVIL_TYPE_VECTOR || instr->operands[1]->kind != ANVIL_VAL_CONST_INT || bits < 8 || bits > 64 ||
        !anvil_vector_shape_cost(ctx, is_signed ? ANVIL_OP_SMULH : ANVIL_OP_UMULH, type, ANVIL_VECTOR_SHAPE_SCALAR))
        return false;
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    uint64_t d = anvil_const_int_unsigned_value(instr->operands[1]) & mask;
    int64_t signed_d = (int64_t)(d & sign ? d | ~mask : d);
    if (is_signed ? signed_d >= -1 && signed_d <= 1 : d <= 1)
        return false;

    division_steps_t steps = {.ctx = ctx, .type = type};
    anvil_value_t *q;
    if (is_signed) {
        division_magic_t magic = signed_magic(signed_d, bits);
        bool negative = (magic.multiplier & sign) != 0;
        q = division_step(&steps, ANVIL_OP_SMULH, n, division_constant(&steps, magic.multiplier));
        if (signed_d > 0 && negative)
            q = division_step(&steps, ANVIL_OP_ADD, q, n);
        else if (signed_d < 0 && !negative)
            q = division_step(&steps, ANVIL_OP_SUB, q, n);
        if (magic.shift)
            q = division_step(&steps, ANVIL_OP_SAR, q, division_constant(&steps, magic.shift));
        q = division_step(&steps, ANVIL_OP_ADD, q, division_step(&steps, ANVIL_OP_SHR, q, division_constant(&steps, bits - 1)));
    } else {
        division_magic_t magic = unsigned_magic(d, bits);
        anvil_value_t *t = division_step(&steps, ANVIL_OP_UMULH, n, division_constant(&steps, magic.multiplier));
        q = t;
        if (magic.add) {
            q = division_step(&steps, ANVIL_OP_SUB, n, t);
            q = division_step(&steps, ANVIL_OP_SHR, q, division_constant(&steps, 1));
            q = division_step(&steps, ANVIL_OP_ADD, q, t);
            if (magic.shift > 1)
                q = division_step(&steps, ANVIL_OP_SHR, q, division_constant(&steps, magic.shift - 1));
        } else if (magic.shift) {
            q = division_step(&steps, ANVIL_OP_SHR, q, division_constant(&steps, magic.shift));
        }
    }
    /* A remainder is n - q * d.  The last step becomes `instr` itself. */
    anvil_value_t *product = remainder ? division_step(&steps, ANVIL_OP_MUL, q, instr->operands[1]) : q;
    if (!q || !product || !steps.count)
        return false;
    anvil_instr_t *last = remainder ? NULL : steps.steps[--steps.count];
    for (size_t index = 0; index < steps.count; index++)
        anvil_opt_insert_before(instr, steps.steps[index]);
    instr->op = remainder ? ANVIL_OP_SUB : last->op;
    instr->operands[0] = remainder ? n : last->operands[0];
    instr->operands[1] = remainder ? product : last->operands[1];
    return true;
}

/* (x << a) | (x >> b), the shift right logical, with a + b the width (two
 * constants) or b = width - a: a rotate left by a, where the target has
 * one.  The shifts stay for any other reader. */
static bool reduce_rotate(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    anvil_type_t *type = instr->result ? instr->result->type : NULL;
    unsigned bits = type ? anvil_type_bit_width(type) : 0;
    if (!type || type->kind == ANVIL_TYPE_VECTOR || !anvil_type_is_integer(type) || bits < 8 || bits > 64 || instr->num_operands != 2 ||
        !anvil_vector_shape_cost(ctx, ANVIL_OP_ROTL, type, ANVIL_VECTOR_SHAPE_SCALAR))
        return false;
    for (int side = 0; side < 2; side++) {
        const anvil_value_t *left = instr->operands[side], *right = instr->operands[1 - side];
        if (left->kind != ANVIL_VAL_INSTR || right->kind != ANVIL_VAL_INSTR || left->data.instr->op != ANVIL_OP_SHL ||
            right->data.instr->op != ANVIL_OP_SHR || left->data.instr->operands[0] != right->data.instr->operands[0])
            continue;
        anvil_value_t *a = left->data.instr->operands[1], *b = right->data.instr->operands[1];
        bool constants = a->kind == ANVIL_VAL_CONST_INT && b->kind == ANVIL_VAL_CONST_INT &&
                         anvil_const_int_unsigned_value(a) + anvil_const_int_unsigned_value(b) == bits && anvil_const_int_unsigned_value(a) < bits;
        const anvil_instr_t *difference = b->kind == ANVIL_VAL_INSTR ? b->data.instr : NULL;
        bool complement = difference && difference->op == ANVIL_OP_SUB && difference->operands[1] == a &&
                          difference->operands[0]->kind == ANVIL_VAL_CONST_INT && anvil_const_int_unsigned_value(difference->operands[0]) == bits;
        if (!constants && !complement)
            continue;
        instr->op = ANVIL_OP_ROTL;
        instr->operands[0] = left->data.instr->operands[0];
        instr->operands[1] = a;
        return true;
    }
    return false;
}

/* The product operand of fadd(fmul(a, b), c) (either order) used only
 * there, where the target has FMA for the type: 0 or 1, or -1. */
static int contractible_side(anvil_ctx_t *ctx, const anvil_def_use_t *uses, const anvil_instr_t *add)
{
    anvil_type_t *type = add->result->type;
    anvil_vector_shape_t shape = type->kind == ANVIL_TYPE_VECTOR ? ANVIL_VECTOR_SHAPE_LANES : ANVIL_VECTOR_SHAPE_SCALAR;
    if (add->num_operands != 2 || !anvil_vector_shape_cost(ctx, ANVIL_OP_FMA, type, shape))
        return -1;
    for (int side = 0; side < 2; side++) {
        const anvil_value_t *product = add->operands[side];
        if (product->kind != ANVIL_VAL_INSTR || product->data.instr->op != ANVIL_OP_FMUL || product->data.instr->num_operands != 2)
            continue;
        size_t definition = anvil_def_use_definition(uses, product);
        if (definition != SIZE_MAX && uses->use_offsets[definition + 1] - uses->use_offsets[definition] == 1)
            return side;
    }
    return -1;
}

/* fadd(fmul(a, b), c) into fma(a, b, c); the product goes away. */
static bool contract(anvil_instr_t *add, int side)
{
    anvil_instr_t *multiply = add->operands[side]->data.instr;
    anvil_value_t *addend = add->operands[1 - side];
    if (!anvil_instr_reserve_operands(add, 3))
        return false;
    add->op = ANVIL_OP_FMA;
    add->operands[0] = multiply->operands[0];
    add->operands[1] = multiply->operands[1];
    add->operands[2] = addend;
    add->num_operands = 3;
    anvil_opt_erase_instr(multiply);
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
                    } else if (reduce_constant_division(ctx, instr)) {
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_SDIV:
                case ANVIL_OP_SMOD:
                    if ((is_power_of_2(rhs, &shift) && reduce_signed_power(ctx, instr, (unsigned)shift)) ||
                        reduce_constant_division(ctx, instr))
                        changed = true;

                    break;
                    
                case ANVIL_OP_OR:
                    if (reduce_rotate(ctx, instr))
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
                    } else if (reduce_constant_division(ctx, instr)) {
                        changed = true;
                    }
                    break;
                    
                default:
                    break;
            }
        }
    }
    
    /* Contractions are independent (each single-use product feeds one
     * sum): find them all on one use snapshot, then rewrite. */
    if (func->fp_contraction && anvil_ctx_get_last_error(ctx) == ANVIL_OK) {
        anvil_def_use_t uses;
        if (!anvil_def_use_build(func, &uses))
            return ANVIL_PASS_RUN_ERROR;
        size_t count = 0, capacity = 0;
        struct { anvil_instr_t *add; int side; } *found = NULL;
        bool ok = true;
        for (anvil_block_t *block = func->blocks; block && ok; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr && ok; instr = instr->next) {
                int side = instr->op == ANVIL_OP_FADD ? contractible_side(ctx, &uses, instr) : -1;
                if (side < 0)
                    continue;
                if (count == capacity) {
                    capacity = capacity ? capacity * 2 : 8;
                    void *grown = anvil_ctx_realloc(ctx, found, capacity * sizeof(*found));
                    if (!(ok = grown != NULL))
                        break;
                    found = grown;
                }
                found[count].add = instr;
                found[count++].side = side;
            }
        }
        anvil_def_use_destroy(&uses);
        for (size_t item = 0; ok && item < count; item++) {
            ok = contract(found[item].add, found[item].side);
            changed |= ok;
        }
        free(found);
        if (!ok)
            return ANVIL_PASS_RUN_ERROR;
    }

    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
