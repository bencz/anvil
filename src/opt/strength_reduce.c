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
 * - ext(a op b) -> ext(a) op ext(b) for a no-wrap add, sub or mul whose
 *   extension (sign for a signed type, zero for an unsigned one) is its
 *   only reader: the wide operation is exact as well, and the pieces it
 *   exposes (an invariant part of an index, an induction variable's own
 *   extension) are what loop passes and address modes take apart
 * - compares and extensions of exact truncations -> the wide values
 * - gep p, x + c -> gep (gep p, c), x, and exact truncations out of GEP
 *   indices
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "anvil/anvil_analysis.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* An exact division (no-wrap: no remainder) by a constant d = 2^k * o,
 * o odd: shift right by k (arithmetically for SDIV: the quotient's sign),
 * then multiply by o's inverse modulo 2^width. */
static bool reduce_exact_division(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    anvil_value_t *n = instr->operands[0], *divisor = instr->operands[1];
    anvil_type_t *type = n->type;
    unsigned bits = type && anvil_type_is_integer(type) ? anvil_type_bit_width(type) : 0;
    if (!instr->no_wrap || divisor->kind != ANVIL_VAL_CONST_INT || bits < 8 || bits > 64)
        return false;
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t d = anvil_const_int_unsigned_value(divisor) & mask;
    bool is_signed = instr->op == ANVIL_OP_SDIV;
    /* Negative divisors keep the general forms. */
    if (d == 0 || (is_signed && (d >> (bits - 1)) & 1))
        return false;
    unsigned k = 0;
    while (!((d >> k) & 1))
        k++;
    uint64_t odd = d >> k;
    if (odd == 1 && k == 0)
        return false;
    uint64_t inverse = odd;  /* Newton: each step doubles the correct bits */
    for (int step = 0; step < 6; step++)
        inverse *= 2 - odd * inverse;
    inverse &= mask;
    anvil_value_t *value = n;
    anvil_instr_t *shift = NULL;
    if (k) {
        anvil_value_t *amount = make_shift_const(ctx, type, (int)k);
        if (!amount || !(shift = anvil_opt_make_binary(ctx, is_signed ? ANVIL_OP_SAR : ANVIL_OP_SHR, type, n, amount, "division.reduce")))
            return false;
    }
    anvil_value_t *factor = odd == 1 ? NULL : anvil_opt_make_const_int(ctx, type, (int64_t)inverse);
    if (odd != 1 && !factor)
        return false;
    if (shift) {
        if (odd == 1) {
            instr->op = shift->op;
            instr->operands[1] = shift->operands[1];
            instr->no_wrap = false;
            return true;
        }
        anvil_opt_insert_before(instr, shift);
        value = shift->result;
    }
    instr->op = ANVIL_OP_MUL;
    instr->operands[0] = value;
    instr->operands[1] = factor;
    instr->no_wrap = false;
    return true;
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
    anvil_instr_t *sign = anvil_opt_make_binary(ctx, ANVIL_OP_SAR, type, value, sign_shift, "division.reduce");
    if (!sign)
        return false;

    anvil_instr_t *bias = anvil_opt_make_binary(ctx, ANVIL_OP_AND, type, sign->result, mask, "division.reduce");
    if (!bias)
        return false;

    anvil_instr_t *biased = anvil_opt_make_binary(ctx, ANVIL_OP_ADD, type, value, bias->result, "division.reduce");
    if (!biased)
        return false;

    anvil_instr_t *quotient = NULL;
    anvil_instr_t *product = NULL;
    if (instr->op == ANVIL_OP_SMOD)
    {
        quotient = anvil_opt_make_binary(ctx, ANVIL_OP_SAR, type, biased->result, amount, "division.reduce");
        if (!quotient)
            return false;

        product = anvil_opt_make_binary(ctx, ANVIL_OP_SHL, type, quotient->result, amount, "division.reduce");
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
    anvil_instr_t *step = left && right && steps->count < 8 ? anvil_opt_make_binary(steps->ctx, op, steps->type, left, right, "division.reduce") : NULL;
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
        if (anvil_def_use_count(uses, product) == 1)
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
    add->no_wrap = false;
    add->operands[0] = multiply->operands[0];
    add->operands[1] = multiply->operands[1];
    add->operands[2] = addend;
    add->num_operands = 3;
    anvil_opt_erase_instr(multiply);
    return true;
}

enum { SR_EXTEND_DEPTH = 4 };

/* `value` extended by `ext` to `wide` ahead of `position`: a constant
 * directly, an extension of a single-reader no-wrap operation distributed
 * in turn. */
static anvil_value_t *extend_operand(anvil_ctx_t *ctx, const anvil_def_use_t *uses, anvil_instr_t *position, anvil_op_t ext,
                                     anvil_type_t *wide, anvil_value_t *value, unsigned depth);

/* The no-wrap operation `ext` (an extension) may distribute over. */
static anvil_instr_t *distributable(const anvil_def_use_t *uses, anvil_op_t ext, const anvil_value_t *value)
{
    anvil_instr_t *narrow = value->kind == ANVIL_VAL_INSTR ? value->data.instr : NULL;
    if (!narrow || !narrow->no_wrap || narrow->num_operands != 2 || !narrow->result || !anvil_type_is_integer(narrow->result->type) ||
        (narrow->op != ANVIL_OP_ADD && narrow->op != ANVIL_OP_SUB && narrow->op != ANVIL_OP_MUL) ||
        narrow->result->type->is_signed != (ext == ANVIL_OP_SEXT))
        return NULL;
    return anvil_def_use_count(uses, value) == 1 ? narrow : NULL;
}

/* Rewrites `instr` (ext of a distributable operation) in place into the
 * wide operation over extended operands; the narrow one goes. */
static bool distribute_extension(anvil_ctx_t *ctx, const anvil_def_use_t *uses, anvil_instr_t *instr, unsigned depth)
{
    anvil_instr_t *narrow = instr->operands[0]->data.instr;
    anvil_type_t *wide = instr->result->type;
    anvil_value_t *left = extend_operand(ctx, uses, instr, instr->op, wide, narrow->operands[0], depth + 1);
    anvil_value_t *right = left ? extend_operand(ctx, uses, instr, instr->op, wide, narrow->operands[1], depth + 1) : NULL;
    if (!right || !anvil_instr_reserve_operands(instr, 2))
        return false;
    instr->op = narrow->op;
    instr->no_wrap = true;
    instr->operands[0] = left;
    instr->operands[1] = right;
    instr->num_operands = 2;
    anvil_opt_erase_instr(narrow);
    return true;
}

static anvil_value_t *extend_operand(anvil_ctx_t *ctx, const anvil_def_use_t *uses, anvil_instr_t *position, anvil_op_t ext,
                                     anvil_type_t *wide, anvil_value_t *value, unsigned depth)
{
    if (value->kind == ANVIL_VAL_CONST_INT)
        return anvil_opt_make_const_int(ctx, wide, ext == ANVIL_OP_SEXT ? value->data.i : (int64_t)anvil_const_int_unsigned_value(value));
    anvil_instr_t *extension = anvil_instr_create(ctx, ext, wide, value->name);
    if (!extension || !anvil_instr_add_operand(extension, value))
        return NULL;
    anvil_opt_insert_before(position, extension);
    if (depth < SR_EXTEND_DEPTH && distributable(uses, ext, value) && !distribute_extension(ctx, uses, extension, depth))
        return NULL;
    return extension->result;
}

/* Every extension of a single-reader no-wrap operation, distributed. */
static anvil_pass_result_t distribute_extensions(anvil_func_t *func, anvil_ctx_t *ctx)
{
    bool any = false;
    for (anvil_block_t *block = func->blocks; block && !any; block = block->next)
        for (anvil_instr_t *instr = block->first; instr && !any; instr = instr->next)
            any = instr->no_wrap;
    if (!any)
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_def_use_t uses;
    if (!anvil_def_use_build(func, &uses))
        return ANVIL_PASS_RUN_ERROR;
    bool changed = false, ok = true;
    for (anvil_block_t *block = func->blocks; block && ok; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr && ok; instr = instr->next) {
            if ((instr->op != ANVIL_OP_SEXT && instr->op != ANVIL_OP_ZEXT) || instr->num_operands != 1 || !instr->result ||
                !anvil_type_is_integer(instr->result->type) || !distributable(&uses, instr->op, instr->operands[0]))
                continue;
            ok = distribute_extension(ctx, &uses, instr, 0);
            changed = true;
        }
    }
    anvil_def_use_destroy(&uses);
    return !ok ? ANVIL_PASS_RUN_ERROR : changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

/* `value` as an exact truncation (no-wrap TRUNC): its wide source. */
static anvil_value_t *exact_source(const anvil_value_t *value)
{
    const anvil_instr_t *trunc = value->kind == ANVIL_VAL_INSTR ? value->data.instr : NULL;
    return trunc && trunc->op == ANVIL_OP_TRUNC && trunc->no_wrap && trunc->num_operands == 1 ? trunc->operands[0] : NULL;
}

/* Single-index GEPs give their index's constant parts to the address: an
 * exact truncation the GEP would extend back indexes as its wide source
 * (pointer-wide, or extended alike), and a constant term
 * (anvil_memory_index_term) moves into a base of its own,
 * gep p, x + c -> gep (gep p, c), x, as LICM splits an invariant term.
 * Accesses at constant distances from one index then share that index
 * (t[k] and t[k - 1]): one stream for loop strength reduction, the
 * distance a displacement or a base computed once. */
static anvil_pass_result_t peel_gep_indices(anvil_func_t *func, anvil_ctx_t *ctx)
{
    size_t pointer_size = ctx->data_layout.pointer.size;
    anvil_type_t *offset_type = pointer_size == 8 ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
    bool changed = false;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *gep = block->first; gep; gep = gep->next) {
            if (gep->op != ANVIL_OP_GEP || gep->num_operands != 2 || !gep->result)
                continue;
            anvil_value_t *index = gep->operands[1];
            anvil_value_t *wide = exact_source(index);
            if (wide && anvil_type_is_integer(wide->type) &&
                (wide->type->size == pointer_size || (wide->type->size < pointer_size && wide->type->is_signed == index->type->is_signed))) {
                gep->operands[1] = wide;
                changed = true;
                index = wide;
            }
            anvil_instr_t *term = index->kind == ANVIL_VAL_INSTR ? index->data.instr : NULL;
            anvil_type_t *element = gep->aux_type;
            anvil_gep_step_t step;
            int64_t amount;
            if (!term || term->num_operands != 2 || term->operands[0]->kind == ANVIL_VAL_CONST_INT ||
                !anvil_gep_analyze_step(&element, index, 0, &step) || step.kind != ANVIL_GEP_STEP_SCALE ||
                !anvil_memory_index_term(term, step.amount, pointer_size, &amount))
                continue;
            anvil_value_t *offset = anvil_opt_make_const_int(ctx, offset_type, amount);
            if (!offset || !anvil_opt_split_gep(ctx, gep, gep, offset, term->operands[0]))
                return ANVIL_PASS_RUN_ERROR;
            changed = true;
        }
    }
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

/* `value` extended to `wide` (sign or zero) right where it is defined (a
 * parameter: at the function's start), so the extension runs once, outside
 * any loop the value is invariant in. */
static anvil_value_t *extended_once(anvil_func_t *func, anvil_ctx_t *ctx, anvil_value_t *value, anvil_type_t *wide, bool sign)
{
    anvil_instr_t *position = NULL;
    if (value->kind == ANVIL_VAL_PARAM) {
        position = func->blocks ? func->blocks->first : NULL;
        while (position && position->op == ANVIL_OP_PHI)
            position = position->next;
    } else if (value->kind == ANVIL_VAL_INSTR && value->data.instr->parent) {
        position = value->data.instr->next;
        while (position && position->op == ANVIL_OP_PHI)
            position = position->next;
    }
    if (!position)
        return NULL;
    anvil_instr_t *extension = anvil_instr_create(ctx, sign ? ANVIL_OP_SEXT : ANVIL_OP_ZEXT, wide, value->name);
    if (!extension || !anvil_instr_add_operand(extension, value))
        return NULL;
    anvil_opt_insert_before(position, extension);
    return extension->result;
}

/* Exact truncations fold away where the wide value serves: a compare of
 * two, or of one and another value (a constant, or a value extended once
 * where it is defined), compares the wide values, the narrow
 * values being their extensions by the narrow type's signedness, which is
 * the compare's; an extension of one by that signedness back to the wide
 * type is the wide value. */
static bool fold_exact_truncations(anvil_func_t *func, anvil_ctx_t *ctx)
{
    bool changed = false;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            bool compare = instr->op >= ANVIL_OP_CMP_EQ && instr->op <= ANVIL_OP_CMP_UGE && instr->num_operands == 2;
            if (compare) {
                anvil_value_t *left = exact_source(instr->operands[0]), *right = exact_source(instr->operands[1]);
                anvil_value_t *wide = left ? left : right;
                if (!wide || !anvil_type_is_integer(instr->operands[0]->type))
                    continue;
                bool is_signed = instr->operands[0]->type->is_signed;
                anvil_value_t *sides[2] = {left, right};
                for (size_t side = 0; side < 2; side++) {
                    anvil_value_t *operand = instr->operands[side];
                    if (!sides[side] && operand->kind == ANVIL_VAL_CONST_INT)
                        sides[side] = anvil_opt_make_const_int(ctx, wide->type, is_signed ? operand->data.i : (int64_t)anvil_const_int_unsigned_value(operand));
                    else if (!sides[side])
                        sides[side] = extended_once(func, ctx, operand, wide->type, is_signed);
                }
                if (!sides[0] || !sides[1] || !anvil_types_equal(sides[0]->type, sides[1]->type))
                    continue;
                instr->operands[0] = sides[0];
                instr->operands[1] = sides[1];
                changed = true;
                continue;
            }
            if ((instr->op == ANVIL_OP_SEXT || instr->op == ANVIL_OP_ZEXT) && instr->num_operands == 1 && instr->result) {
                anvil_value_t *wide = exact_source(instr->operands[0]);
                if (wide && anvil_types_equal(wide->type, instr->result->type) &&
                    instr->operands[0]->type->is_signed == (instr->op == ANVIL_OP_SEXT) &&
                    anvil_opt_replace_uses_in_func(func, instr->result, wide) > 0)
                    changed = true;
            }
        }
    }
    return changed;
}

/* A compare of extensions of one narrow type (or of one and a constant it
 * represents) compares the narrow values: ext(a) == ext(b) is a == b,
 * sext(c) != 0 is c != 0, sext(a) < sext(b) is a < b (zext and unsigned
 * orders alike).  The extensions often go then, and the narrow values'
 * loads with them (a C char promoted to int only to be compared).  Exact
 * truncations are left to fold_exact_truncations, which widens them. */
static bool narrow_extended_compare(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (instr->op < ANVIL_OP_CMP_EQ || instr->op > ANVIL_OP_CMP_UGE || instr->num_operands != 2)
        return false;
    bool equality = instr->op == ANVIL_OP_CMP_EQ || instr->op == ANVIL_OP_CMP_NE;
    bool unsigned_order = instr->op >= ANVIL_OP_CMP_ULT && instr->op <= ANVIL_OP_CMP_UGE;
    anvil_op_t kind = ANVIL_OP_NOP;
    anvil_type_t *narrow = NULL;
    anvil_value_t *sides[2] = {NULL, NULL};
    for (size_t side = 0; side < 2; side++) {
        anvil_value_t *operand = instr->operands[side];
        if (operand->kind != ANVIL_VAL_INSTR || !operand->data.instr)
            continue;
        const anvil_instr_t *ext = operand->data.instr;
        if ((ext->op != ANVIL_OP_SEXT && ext->op != ANVIL_OP_ZEXT) || ext->num_operands != 1)
            continue;
        anvil_value_t *source = ext->operands[0];
        if (!anvil_type_is_integer(source->type) || source->type->kind == ANVIL_TYPE_I1 || exact_source(source))
            return false;
        if ((kind != ANVIL_OP_NOP && kind != ext->op) || (narrow && !anvil_types_equal(narrow, source->type)))
            return false;
        kind = ext->op;
        narrow = source->type;
        sides[side] = source;
    }
    if (!narrow)
        return false;
    /* An order keeps its meaning on narrow values of its own signedness
     * (sext of signed, zext of unsigned); a signed order of zero-extended
     * values is the unsigned order of the narrow ones. */
    anvil_op_t op = instr->op;
    if (!equality && kind == ANVIL_OP_ZEXT && !unsigned_order && !narrow->is_signed)
        op = (anvil_op_t)(ANVIL_OP_CMP_ULT + (op - ANVIL_OP_CMP_LT));
    else if (!equality && narrow->is_signed == unsigned_order)
        return false;
    unsigned bits = anvil_type_bit_width(narrow);
    for (size_t side = 0; side < 2; side++) {
        if (sides[side])
            continue;
        anvil_value_t *operand = instr->operands[side];
        if (operand->kind != ANVIL_VAL_CONST_INT || bits >= 64)
            return false;
        int64_t value = operand->data.i;
        bool fits = kind == ANVIL_OP_SEXT ? value >= -(INT64_C(1) << (bits - 1)) && value < (INT64_C(1) << (bits - 1))
                                          : (uint64_t)anvil_const_int_unsigned_value(operand) < (UINT64_C(1) << bits);
        if (!fits || !(sides[side] = anvil_opt_make_const_int(ctx, narrow, value)))
            return false;
    }
    instr->op = op;
    instr->operands[0] = sides[0];
    instr->operands[1] = sides[1];
    return true;
}

/* Strength reduction pass *//* Strength reduction pass */
/* One side of a range test on x: x >= lo (lower) or x <= hi, from x op c
 * or c op x with a constant c, in the signedness of the compare. */
typedef struct {
    anvil_value_t *x;
    int64_t bound;
    bool lower, is_unsigned;
} range_side_t;

static bool range_side(const anvil_value_t *value, range_side_t *side)
{
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr || value->data.instr->num_operands != 2)
        return false;
    const anvil_instr_t *cmp = value->data.instr;
    anvil_op_t op = cmp->op;
    anvil_value_t *a = cmp->operands[0], *b = cmp->operands[1];
    bool constant_left = a->kind == ANVIL_VAL_CONST_INT && b->kind != ANVIL_VAL_CONST_INT;
    if (constant_left) {
        /* c op x is x op' c. */
        switch (op) {
        case ANVIL_OP_CMP_LT: op = ANVIL_OP_CMP_GT; break;
        case ANVIL_OP_CMP_LE: op = ANVIL_OP_CMP_GE; break;
        case ANVIL_OP_CMP_GT: op = ANVIL_OP_CMP_LT; break;
        case ANVIL_OP_CMP_GE: op = ANVIL_OP_CMP_LE; break;
        case ANVIL_OP_CMP_ULT: op = ANVIL_OP_CMP_UGT; break;
        case ANVIL_OP_CMP_ULE: op = ANVIL_OP_CMP_UGE; break;
        case ANVIL_OP_CMP_UGT: op = ANVIL_OP_CMP_ULT; break;
        case ANVIL_OP_CMP_UGE: op = ANVIL_OP_CMP_ULE; break;
        default: return false;
        }
        anvil_value_t *t = a;
        a = b;
        b = t;
    }
    if (b->kind != ANVIL_VAL_CONST_INT || a->kind == ANVIL_VAL_CONST_INT || !anvil_type_is_integer(a->type) || a->type->kind == ANVIL_TYPE_I1)
        return false;
    unsigned bits = anvil_type_bit_width(a->type);
    if (bits == 0 || bits > 64)
        return false;
    side->x = a;
    side->is_unsigned = op >= ANVIL_OP_CMP_ULT && op <= ANVIL_OP_CMP_UGE;
    if (side->is_unsigned == a->type->is_signed)
        return false;
    /* The constant as a value of x's width and signedness. */
    uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    int64_t c = side->is_unsigned ? (int64_t)(b->data.u & mask)
                                  : bits == 64 ? b->data.i : (int64_t)(((b->data.u & mask) ^ (UINT64_C(1) << (bits - 1))) - (UINT64_C(1) << (bits - 1)));
    int64_t smin = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1)), smax = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
    switch (op) {
    case ANVIL_OP_CMP_GE: case ANVIL_OP_CMP_UGE: side->lower = true; side->bound = c; return true;
    case ANVIL_OP_CMP_LE: case ANVIL_OP_CMP_ULE: side->lower = false; side->bound = c; return true;
    case ANVIL_OP_CMP_GT:
        side->lower = true; side->bound = c + 1; return c < smax;
    case ANVIL_OP_CMP_LT:
        side->lower = false; side->bound = c - 1; return c > smin;
    case ANVIL_OP_CMP_UGT:
        side->lower = true; side->bound = c + 1; return (uint64_t)c < mask;
    case ANVIL_OP_CMP_ULT:
        side->lower = false; side->bound = c - 1; return c != 0;
    default:
        return false;
    }
}

static anvil_type_t *unsigned_of_width(anvil_ctx_t *ctx, unsigned bits)
{
    return bits == 8 ? anvil_type_u8(ctx) : bits == 16 ? anvil_type_u16(ctx) : bits == 32 ? anvil_type_u32(ctx) : bits == 64 ? anvil_type_u64(ctx) : NULL;
}

/* lo <= x && x <= hi (an `and` of the two tests, or select(t1, t2, false))
 * with constants lo <= hi is one unsigned compare, (x - lo) u<= hi - lo:
 * values below lo wrap above hi - lo. */
static bool reduce_range_check(anvil_ctx_t *ctx, anvil_instr_t *instr)
{
    if (!instr->result)
        return false;
    anvil_value_t *first = NULL, *second = NULL;
    /* select(t1, zext(t2), 0): the C value of t1 && t2. */
    bool widened = false;
    if (instr->op == ANVIL_OP_SELECT && instr->num_operands == 3 && instr->result->type->kind != ANVIL_TYPE_I1 &&
        anvil_type_is_integer(instr->result->type) && anvil_opt_is_zero(instr->operands[2]) && instr->operands[1]->kind == ANVIL_VAL_INSTR &&
        instr->operands[1]->data.instr->op == ANVIL_OP_ZEXT && instr->operands[1]->data.instr->num_operands == 1 &&
        instr->operands[1]->data.instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
        first = instr->operands[0];
        second = instr->operands[1]->data.instr->operands[0];
        widened = true;
    } else if (instr->result->type->kind != ANVIL_TYPE_I1) {
        return false;
    } else if (instr->op == ANVIL_OP_AND && instr->num_operands == 2) {
        first = instr->operands[0];
        second = instr->operands[1];
    } else if (instr->op == ANVIL_OP_SELECT && instr->num_operands == 3 && instr->operands[2]->kind == ANVIL_VAL_CONST_INT &&
               instr->operands[2]->data.i == 0) {
        first = instr->operands[0];
        second = instr->operands[1];
    } else {
        return false;
    }
    range_side_t a, b;
    if (!range_side(first, &a) || !range_side(second, &b) || a.x != b.x || a.lower == b.lower || a.is_unsigned != b.is_unsigned)
        return false;
    const range_side_t *low = a.lower ? &a : &b, *high = a.lower ? &b : &a;
    if (low->is_unsigned ? (uint64_t)low->bound > (uint64_t)high->bound : low->bound > high->bound)
        return false;
    anvil_type_t *type = low->x->type, *unsigned_type = unsigned_of_width(ctx, anvil_type_bit_width(type));
    if (!unsigned_type)
        return false;
    anvil_value_t *lo = anvil_opt_make_const_int(ctx, type, low->bound);
    anvil_value_t *span = anvil_opt_make_const_int(ctx, unsigned_type, (int64_t)((uint64_t)high->bound - (uint64_t)low->bound));
    anvil_instr_t *offset = lo && span ? anvil_opt_make_binary(ctx, ANVIL_OP_SUB, type, low->x, lo, "division.reduce") : NULL;
    if (!offset)
        return false;
    anvil_opt_insert_before(instr, offset);
    anvil_value_t *distance = offset->result;
    if (type != unsigned_type && !anvil_types_equal(type, unsigned_type)) {
        anvil_instr_t *cast = anvil_instr_create(ctx, ANVIL_OP_BITCAST, unsigned_type, "range");
        if (!cast || !anvil_instr_add_operand(cast, offset->result))
            return false;
        anvil_opt_insert_before(instr, cast);
        distance = cast->result;
    }
    if (widened) {
        anvil_instr_t *inside = anvil_instr_create(ctx, ANVIL_OP_CMP_ULE, anvil_type_i1(ctx), "range");
        if (!inside || !anvil_instr_add_operand(inside, distance) || !anvil_instr_add_operand(inside, span))
            return false;
        anvil_opt_insert_before(instr, inside);
        instr->op = ANVIL_OP_ZEXT;
        instr->operands[0] = inside->result;
        instr->num_operands = 1;
        instr->no_wrap = false;
        return true;
    }
    instr->op = ANVIL_OP_CMP_ULE;
    instr->operands[0] = distance;
    instr->operands[1] = span;
    instr->num_operands = 2;
    instr->no_wrap = false;
    return true;
}

anvil_pass_result_t anvil_pass_strength_reduce(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    anvil_pass_result_t distributed = distribute_extensions(func, ctx);
    if (distributed == ANVIL_PASS_RUN_ERROR)
        return distributed;
    bool changed = distributed == ANVIL_PASS_RUN_CHANGED;
    changed |= fold_exact_truncations(func, ctx);
    anvil_pass_result_t peeled = peel_gep_indices(func, ctx);
    if (peeled == ANVIL_PASS_RUN_ERROR)
        return peeled;
    changed |= peeled == ANVIL_PASS_RUN_CHANGED;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            if (instr->num_operands < 2) continue;
            
            anvil_value_t *lhs = instr->operands[0];
            anvil_value_t *rhs = instr->operands[1];
            int shift;
            if ((instr->op == ANVIL_OP_AND || instr->op == ANVIL_OP_SELECT) && reduce_range_check(ctx, instr)) {
                changed = true;
                continue;
            }
            if (narrow_extended_compare(ctx, instr)) {
                changed = true;
                continue;
            }
            
            switch (instr->op) {
                case ANVIL_OP_MUL:
                    /* x * 2^n -> x << n */
                    if (anvil_opt_power_of_two(rhs, &shift)) {
                        anvil_value_t *amount = make_shift_const(
                            ctx, lhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHL;
                        instr->no_wrap = false;
                        instr->operands[1] = amount;
                        changed = true;
                    } else if (anvil_opt_power_of_two(lhs, &shift)) {
                        /* 2^n * x -> x << n */
                        anvil_value_t *amount = make_shift_const(
                            ctx, rhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHL;
                        instr->no_wrap = false;
                        instr->operands[0] = rhs;
                        instr->operands[1] = amount;
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_UDIV:
                    /* x / 2^n -> x >> n (unsigned) */
                    if (!anvil_opt_power_of_two(rhs, &shift) && reduce_exact_division(ctx, instr)) {
                        changed = true;
                    } else if (anvil_opt_power_of_two(rhs, &shift)) {
                        instr->no_wrap = false;
                        anvil_value_t *amount = make_shift_const(
                            ctx, lhs->type, shift);
                        if (!amount) break;
                        instr->op = ANVIL_OP_SHR;
                        instr->operands[1] = amount;
                        changed = true;
                    } else if (reduce_constant_division(ctx, instr)) {
                        instr->no_wrap = false;
                        changed = true;
                    }
                    break;
                    
                case ANVIL_OP_SDIV:
                case ANVIL_OP_SMOD:
                    if (reduce_exact_division(ctx, instr) || (anvil_opt_power_of_two(rhs, &shift) && reduce_signed_power(ctx, instr, (unsigned)shift)) ||
                        reduce_constant_division(ctx, instr)) {
                        instr->no_wrap = false;
                        changed = true;
                    }

                    break;
                    
                case ANVIL_OP_OR:
                    if (reduce_rotate(ctx, instr))
                        changed = true;
                    break;

                case ANVIL_OP_UMOD:
                    /* x % 2^n -> x & (2^n - 1) (unsigned) */
                    if (anvil_opt_power_of_two(rhs, &shift)) {
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
