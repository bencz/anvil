#include "wide_mir.h"

static anvil_mir_vreg_t word(anvil_mir_func_t *mir, bool is_signed)
{
    return anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, 32, is_signed);
}

static anvil_mir_vreg_t boolean(anvil_mir_func_t *mir)
{
    return anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, 8, false);
}

static bool op2(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t a, anvil_mir_vreg_t b)
{
    anvil_mir_vreg_t uses[] = {a, b};
    return def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(mir, op, def, uses, 2);
}

static bool op1(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t a)
{
    anvil_mir_vreg_t uses[] = {a};
    return def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(mir, op, def, uses, 1);
}

static anvil_mir_vreg_t imm(anvil_mir_func_t *mir, int64_t value)
{
    anvil_mir_vreg_t vreg = word(mir, false);
    return vreg != ANVIL_MIR_NO_VREG && anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, vreg, value) ? vreg : ANVIL_MIR_NO_VREG;
}

bool anvil_wide_new(anvil_mir_func_t *mir, bool is_unsigned, anvil_wide_pair_t *out)
{
    out->hi = word(mir, !is_unsigned);
    out->lo = word(mir, false);
    return out->hi != ANVIL_MIR_NO_VREG && out->lo != ANVIL_MIR_NO_VREG;
}

bool anvil_wide_constant(anvil_mir_func_t *mir, int64_t value, bool is_unsigned, anvil_wide_pair_t *out)
{
    uint64_t bits = (uint64_t)value;
    return anvil_wide_new(mir, is_unsigned, out) &&
           anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, out->hi, (int32_t)(uint32_t)(bits >> 32)) &&
           anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, out->lo, (int32_t)(uint32_t)bits);
}

/* lo = a.lo +/- b.lo; hi = a.hi +/- b.hi +/- carry, the carry (borrow)
 * being the unsigned overflow of the low halves. */
static bool add_sub(anvil_mir_func_t *mir, bool subtract, anvil_wide_pair_t a, anvil_wide_pair_t b, anvil_wide_pair_t *out)
{
    anvil_mir_vreg_t carry = boolean(mir);
    anvil_mir_vreg_t carry_word = word(mir, false);
    anvil_mir_vreg_t partial = word(mir, false);
    anvil_mir_opcode_t op = subtract ? ANVIL_MIR_OP_SUB : ANVIL_MIR_OP_ADD;
    if (!op2(mir, op, out->lo, a.lo, b.lo))
        return false;
    bool carry_ok = subtract ? op2(mir, ANVIL_MIR_OP_CMP_ULT, carry, a.lo, b.lo)
                             : op2(mir, ANVIL_MIR_OP_CMP_ULT, carry, out->lo, a.lo);
    return carry_ok && op1(mir, ANVIL_MIR_OP_ZEXT, carry_word, carry) &&
           op2(mir, op, partial, a.hi, b.hi) && op2(mir, op, out->hi, partial, carry_word);
}

static const char *binary_helper(anvil_op_t op)
{
    switch (op) {
    case ANVIL_OP_MUL: return "__muldi3";
    case ANVIL_OP_SDIV: return "__divdi3";
    case ANVIL_OP_UDIV: return "__udivdi3";
    case ANVIL_OP_SMOD: return "__moddi3";
    case ANVIL_OP_UMOD: return "__umoddi3";
    default: return NULL;
    }
}

bool anvil_wide_binary(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_op_t op,
                       anvil_wide_pair_t a, anvil_wide_pair_t b, bool is_unsigned, anvil_wide_pair_t *out)
{
    if (!anvil_wide_new(mir, is_unsigned, out))
        return false;
    switch (op) {
    case ANVIL_OP_ADD:
        return add_sub(mir, false, a, b, out);
    case ANVIL_OP_SUB:
        return add_sub(mir, true, a, b, out);
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR: {
        anvil_mir_opcode_t mop = op == ANVIL_OP_AND ? ANVIL_MIR_OP_AND : op == ANVIL_OP_OR ? ANVIL_MIR_OP_OR : ANVIL_MIR_OP_XOR;
        return op2(mir, mop, out->hi, a.hi, b.hi) && op2(mir, mop, out->lo, a.lo, b.lo);
    }
    default: {
        const char *helper = binary_helper(op);
        anvil_wide_pair_t args[] = {a, b};
        return helper && target && target->call_helper(target->ctx, helper, args, 2, *out);
    }
    }
}

/* Shift of one half by a constant amount 1..31. */
static anvil_mir_vreg_t shift_word(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t value, int64_t amount, bool is_signed)
{
    anvil_mir_vreg_t result = word(mir, is_signed);
    anvil_mir_vreg_t count = imm(mir, amount);
    return count != ANVIL_MIR_NO_VREG && op2(mir, op, result, value, count) ? result : ANVIL_MIR_NO_VREG;
}

static bool copy(anvil_mir_func_t *mir, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    return op1(mir, ANVIL_MIR_OP_COPY, dst, src);
}

static bool shift_known(anvil_mir_func_t *mir, anvil_op_t op, anvil_wide_pair_t a, int64_t n, anvil_wide_pair_t *out)
{
    if (n == 0)
        return copy(mir, out->hi, a.hi) && copy(mir, out->lo, a.lo);

    if (op == ANVIL_OP_SHL) {
        if (n >= 32) {
            anvil_mir_vreg_t moved = n == 32 ? a.lo : shift_word(mir, ANVIL_MIR_OP_SHL, a.lo, n - 32, false);
            return moved != ANVIL_MIR_NO_VREG && copy(mir, out->hi, moved) &&
                   anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, out->lo, 0);
        }
        anvil_mir_vreg_t high = shift_word(mir, ANVIL_MIR_OP_SHL, a.hi, n, false);
        anvil_mir_vreg_t spill = shift_word(mir, ANVIL_MIR_OP_SHR, a.lo, 32 - n, false);
        anvil_mir_vreg_t low = shift_word(mir, ANVIL_MIR_OP_SHL, a.lo, n, false);
        return high != ANVIL_MIR_NO_VREG && spill != ANVIL_MIR_NO_VREG && low != ANVIL_MIR_NO_VREG &&
               op2(mir, ANVIL_MIR_OP_OR, out->hi, high, spill) && copy(mir, out->lo, low);
    }

    /* SHR / SAR: the high half shifts in zeros or copies of its sign. */
    bool arithmetic = op == ANVIL_OP_SAR;
    anvil_mir_opcode_t hi_op = arithmetic ? ANVIL_MIR_OP_SAR : ANVIL_MIR_OP_SHR;
    if (n >= 32) {
        anvil_mir_vreg_t moved = n == 32 ? a.hi : shift_word(mir, hi_op, a.hi, n - 32, arithmetic);
        if (moved == ANVIL_MIR_NO_VREG || !copy(mir, out->lo, moved))
            return false;
        if (!arithmetic)
            return anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, out->hi, 0);
        anvil_mir_vreg_t sign = shift_word(mir, ANVIL_MIR_OP_SAR, a.hi, 31, true);
        return sign != ANVIL_MIR_NO_VREG && copy(mir, out->hi, sign);
    }
    anvil_mir_vreg_t low = shift_word(mir, ANVIL_MIR_OP_SHR, a.lo, n, false);
    anvil_mir_vreg_t spill = shift_word(mir, ANVIL_MIR_OP_SHL, a.hi, 32 - n, false);
    anvil_mir_vreg_t high = shift_word(mir, hi_op, a.hi, n, arithmetic);
    return low != ANVIL_MIR_NO_VREG && spill != ANVIL_MIR_NO_VREG && high != ANVIL_MIR_NO_VREG &&
           op2(mir, ANVIL_MIR_OP_OR, out->lo, low, spill) && copy(mir, out->hi, high);
}

bool anvil_wide_shift(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_op_t op,
                      anvil_wide_pair_t a, anvil_mir_vreg_t count, bool count_known, int64_t known_count,
                      bool is_unsigned, anvil_wide_pair_t *out)
{
    if (op != ANVIL_OP_SHL && op != ANVIL_OP_SHR && op != ANVIL_OP_SAR)
        return false;
    if (!anvil_wide_new(mir, is_unsigned, out))
        return false;
    if (count_known && known_count >= 0 && known_count < 64)
        return shift_known(mir, op, a, known_count, out);

    const char *helper = op == ANVIL_OP_SHL ? "__ashldi3" : op == ANVIL_OP_SHR ? "__lshrdi3" : "__ashrdi3";
    anvil_wide_pair_t args[] = {a, {ANVIL_MIR_NO_VREG, count}};
    return target && count != ANVIL_MIR_NO_VREG && target->call_helper(target->ctx, helper, args, 2, *out);
}

bool anvil_wide_neg(anvil_mir_func_t *mir, anvil_wide_pair_t a, bool is_unsigned, anvil_wide_pair_t *out)
{
    anvil_wide_pair_t zero;
    return anvil_wide_constant(mir, 0, is_unsigned, &zero) && anvil_wide_new(mir, is_unsigned, out) &&
           add_sub(mir, true, zero, a, out);
}

bool anvil_wide_not(anvil_mir_func_t *mir, anvil_wide_pair_t a, bool is_unsigned, anvil_wide_pair_t *out)
{
    return anvil_wide_new(mir, is_unsigned, out) && op1(mir, ANVIL_MIR_OP_NOT, out->hi, a.hi) &&
           op1(mir, ANVIL_MIR_OP_NOT, out->lo, a.lo);
}

bool anvil_wide_compare(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_wide_pair_t a, anvil_wide_pair_t b,
                        anvil_mir_vreg_t result)
{
    if (op == ANVIL_MIR_OP_CMP_EQ || op == ANVIL_MIR_OP_CMP_NE) {
        anvil_mir_vreg_t hi = boolean(mir), lo = boolean(mir);
        anvil_mir_opcode_t join = op == ANVIL_MIR_OP_CMP_EQ ? ANVIL_MIR_OP_AND : ANVIL_MIR_OP_OR;
        return op2(mir, op, hi, a.hi, b.hi) && op2(mir, op, lo, a.lo, b.lo) && op2(mir, join, result, hi, lo);
    }

    /* (a.hi REL b.hi) || (a.hi == b.hi && a.lo REL' b.lo), the low halves
     * always compared unsigned. */
    bool unsigned_cmp = op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE;
    bool less = op == ANVIL_MIR_OP_CMP_LT || op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE;
    bool equal_ok = op == ANVIL_MIR_OP_CMP_LE || op == ANVIL_MIR_OP_CMP_GE || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGE;
    if (!less && op != ANVIL_MIR_OP_CMP_GT && op != ANVIL_MIR_OP_CMP_GE && op != ANVIL_MIR_OP_CMP_UGT && op != ANVIL_MIR_OP_CMP_UGE)
        return false;
    anvil_mir_opcode_t hi_rel = less ? (unsigned_cmp ? ANVIL_MIR_OP_CMP_ULT : ANVIL_MIR_OP_CMP_LT)
                                     : (unsigned_cmp ? ANVIL_MIR_OP_CMP_UGT : ANVIL_MIR_OP_CMP_GT);
    anvil_mir_opcode_t lo_rel = less ? (equal_ok ? ANVIL_MIR_OP_CMP_ULE : ANVIL_MIR_OP_CMP_ULT)
                                     : (equal_ok ? ANVIL_MIR_OP_CMP_UGE : ANVIL_MIR_OP_CMP_UGT);
    anvil_mir_vreg_t hi_cmp = boolean(mir), hi_eq = boolean(mir), lo_cmp = boolean(mir), eq_and_lo = boolean(mir);
    return op2(mir, hi_rel, hi_cmp, a.hi, b.hi) && op2(mir, ANVIL_MIR_OP_CMP_EQ, hi_eq, a.hi, b.hi) &&
           op2(mir, lo_rel, lo_cmp, a.lo, b.lo) && op2(mir, ANVIL_MIR_OP_AND, eq_and_lo, hi_eq, lo_cmp) &&
           op2(mir, ANVIL_MIR_OP_OR, result, hi_cmp, eq_and_lo);
}

bool anvil_wide_extend(anvil_mir_func_t *mir, anvil_mir_vreg_t src, bool sign_extend, bool is_unsigned,
                       anvil_wide_pair_t *out)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, src);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || info->size_bits > 32 || !anvil_wide_new(mir, is_unsigned, out))
        return false;

    /* Widen to 32 bits first, then derive the high half. */
    bool ok = info->size_bits == 32 ? copy(mir, out->lo, src)
                                    : op1(mir, sign_extend ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT, out->lo, src);
    if (!ok)
        return false;
    if (!sign_extend)
        return anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, out->hi, 0);
    anvil_mir_vreg_t sign = shift_word(mir, ANVIL_MIR_OP_SAR, out->lo, 31, true);
    return sign != ANVIL_MIR_NO_VREG && copy(mir, out->hi, sign);
}

anvil_mir_vreg_t anvil_wide_truncate(anvil_mir_func_t *mir, anvil_wide_pair_t a, uint16_t bits, bool is_signed)
{
    anvil_mir_vreg_t result = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, bits, is_signed);
    bool ok = bits == 32 ? copy(mir, result, a.lo) : op1(mir, ANVIL_MIR_OP_TRUNC, result, a.lo);
    return ok ? result : ANVIL_MIR_NO_VREG;
}

bool anvil_wide_to_fp(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_wide_pair_t a,
                      bool is_unsigned, uint16_t fp_bits, anvil_mir_vreg_t *out)
{
    const char *helper = is_unsigned ? (fp_bits == 32 ? "__floatundisf" : "__floatundidf")
                                     : (fp_bits == 32 ? "__floatdisf" : "__floatdidf");
    *out = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_FPR, fp_bits, false);
    anvil_wide_pair_t result = {ANVIL_MIR_NO_VREG, *out};
    return *out != ANVIL_MIR_NO_VREG && target && target->call_helper(target->ctx, helper, &a, 1, result);
}

bool anvil_wide_from_fp(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_mir_vreg_t src,
                        bool is_unsigned, anvil_wide_pair_t *out)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, src);
    if (!info || info->reg_class != ANVIL_MIR_REG_FPR)
        return false;
    bool single = info->size_bits == 32;
    const char *helper = is_unsigned ? (single ? "__fixunssfdi" : "__fixunsdfdi") : (single ? "__fixsfdi" : "__fixdfdi");
    anvil_wide_pair_t arg = {ANVIL_MIR_NO_VREG, src};
    return anvil_wide_new(mir, is_unsigned, out) && target && target->call_helper(target->ctx, helper, &arg, 1, *out);
}
