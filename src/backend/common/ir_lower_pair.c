/* Lowering shared by every back end of SELECT and, on targets whose general
 * registers hold 32 bits, of the operations, conversions and accesses on
 * 64-bit values, which travel as word pairs (ir_lower.h). */
#include "ir_lower.h"

static bool is_shift(anvil_op_t op)
{
    return op == ANVIL_OP_SHL || op == ANVIL_OP_SHR || op == ANVIL_OP_SAR;
}

static bool copy(anvil_ir_lower_t *lower, anvil_mir_vreg_t def, anvil_mir_vreg_t value)
{
    anvil_mir_vreg_t uses[] = {value};
    return anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_COPY, def, uses, 1);
}

/* Each word picks between `then_regs` and `else_regs` by branching: the
 * arms copy into `defs`, and lowering continues in the join block. */
static bool select_by_branch(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_vreg_t cond, const anvil_mir_vreg_t *then_regs,
                             const anvil_mir_vreg_t *else_regs, const anvil_mir_vreg_t *defs, size_t words)
{
    anvil_mir_block_t source = anvil_mir_current_block(lower->mir);
    anvil_mir_block_t arms[2] = {anvil_ir_new_block(lower, instr->parent, "select"), anvil_ir_new_block(lower, instr->parent, "select")};
    anvil_mir_block_t join = anvil_ir_new_block(lower, instr->parent, "select");
    if (arms[0] == ANVIL_MIR_NO_BLOCK || arms[1] == ANVIL_MIR_NO_BLOCK || join == ANVIL_MIR_NO_BLOCK || !anvil_mir_set_current_block(lower->mir, source) ||
        !anvil_mir_add_cond_branch(lower->mir, cond, arms[0], arms[1]))
        return false;
    for (size_t arm = 0; arm < 2; arm++) {
        const anvil_mir_vreg_t *values = arm == 0 ? then_regs : else_regs;
        if (!anvil_mir_set_current_block(lower->mir, arms[arm]))
            return false;
        for (size_t word = 0; word < words; word++)
            if (!copy(lower, defs[word], values[word]))
                return false;
        if (!anvil_mir_add_branch(lower->mir, join))
            return false;
    }
    return anvil_mir_set_current_block(lower->mir, join);
}

bool anvil_ir_lower_select(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    if (instr->num_operands != 3 || !instr->result)
        return false;
    anvil_mir_vreg_t then_regs[2], else_regs[2], defs[2];
    size_t words = 0, else_words = 0, def_words = 0;
    anvil_mir_vreg_t cond = anvil_ir_lower_value(lower, instr->operands[0]);
    if (cond == ANVIL_MIR_NO_VREG || !anvil_ir_value_regs(lower, instr->operands[1], then_regs, &words) ||
        !anvil_ir_value_regs(lower, instr->operands[2], else_regs, &else_words) || !anvil_ir_result_regs(lower, instr->result, defs, &def_words) ||
        words != else_words || words != def_words)
        return false;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, defs[0]);
    if (!info)
        return false;
    if (lower->select_classes & (1u << info->reg_class)) {
        for (size_t word = 0; word < words; word++) {
            anvil_mir_vreg_t uses[] = {cond, then_regs[word], else_regs[word]};
            if (!anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_SELECT, defs[word], uses, 3))
                return false;
        }
    } else if (!select_by_branch(lower, instr, cond, then_regs, else_regs, defs, words)) {
        return false;
    }
    return anvil_ir_record_regs(lower, instr->result, defs, words);
}

static bool record_pair(anvil_ir_lower_t *lower, const anvil_value_t *value, anvil_wide_pair_t pair)
{
    return anvil_lower_wide_put(&lower->pairs, value, pair);
}

/* Binary operations, shifts (by a count of any width) and compares. */
static bool pair_binary(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_wide_pair_t a, b, out;
    anvil_value_t *count = instr->operands[1];
    bool shift = is_shift(instr->op);
    if (!instr->result || !anvil_ir_value_pair(lower, instr->operands[0], &a))
        return false;
    if (shift && !anvil_ir_is_pair(lower, count->type)) {
        b.hi = ANVIL_MIR_NO_VREG;
        b.lo = anvil_ir_lower_value(lower, count);
        b.lo = b.lo != ANVIL_MIR_NO_VREG ? anvil_ir_pointer_width(lower, b.lo, false) : ANVIL_MIR_NO_VREG;
        if (b.lo == ANVIL_MIR_NO_VREG)
            return false;
    } else if (!anvil_ir_value_pair(lower, count, &b)) {
        return false;
    }
    if (anvil_ir_compare_op(mir_op)) {
        anvil_mir_vreg_t result = anvil_ir_bool_vreg(lower);
        return result != ANVIL_MIR_NO_VREG && anvil_wide_compare(lower->mir, mir_op, a, b, result) && anvil_lower_vreg_put(&lower->values, instr->result, result);
    }
    bool is_unsigned = anvil_wide_value_unsigned(instr->result);
    bool known = count->kind == ANVIL_VAL_CONST_INT;
    bool ok = shift ? anvil_wide_shift(lower->mir, &lower->wide, instr->op, a, b.lo, known, known ? count->data.i : 0, is_unsigned, &out)
                    : anvil_wide_binary(lower->mir, &lower->wide, instr->op, a, b, is_unsigned, &out);
    return ok && record_pair(lower, instr->result, out);
}

static bool pair_unary(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_wide_pair_t a, out;
    bool is_unsigned = anvil_wide_value_unsigned(instr->result);
    if (!anvil_ir_value_pair(lower, instr->operands[0], &a))
        return false;
    bool ok = instr->op == ANVIL_OP_NEG ? anvil_wide_neg(lower->mir, a, is_unsigned, &out) : instr->op == ANVIL_OP_NOT && anvil_wide_not(lower->mir, a, is_unsigned, &out);
    return ok && record_pair(lower, instr->result, out);
}

/* Conversions to, from or between word pairs. */
static bool pair_cast(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_value_t *source = instr->operands[0];
    anvil_type_t *to = instr->result->type;
    bool from_fp = mir_op == ANVIL_MIR_OP_FPTOSI || mir_op == ANVIL_MIR_OP_FPTOUI;
    bool to_fp = mir_op == ANVIL_MIR_OP_SITOFP || mir_op == ANVIL_MIR_OP_UITOFP;
    anvil_wide_pair_t a, out;
    if (!anvil_ir_is_pair(lower, source->type)) {
        /* To 64 bits: an extension (a pointer zero-extends) or an FP conversion. */
        anvil_mir_vreg_t value = anvil_ir_lower_value(lower, source);
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, value);
        if (!info || (info->reg_class != ANVIL_MIR_REG_GPR) != from_fp)
            return false;
        bool ok = (from_fp ? anvil_wide_from_fp(lower->mir, &lower->wide, value, mir_op == ANVIL_MIR_OP_FPTOUI, &out)
                                                         : anvil_wide_extend(lower->mir, value, mir_op == ANVIL_MIR_OP_SEXT, anvil_wide_value_unsigned(instr->result), &out));
        return ok && record_pair(lower, instr->result, out);
    }
    if (anvil_ir_is_pair(lower, to)) {
        /* Same width: the words carry over, and a constant stays one. */
        int64_t constant = 0;
        if (mir_op != ANVIL_MIR_OP_BITCAST && mir_op != ANVIL_MIR_OP_ZEXT && mir_op != ANVIL_MIR_OP_SEXT && mir_op != ANVIL_MIR_OP_TRUNC)
            return false;
        if (source->kind == ANVIL_VAL_CONST_INT || anvil_lower_constant_get(&lower->constants, source, &constant))
            return anvil_lower_constant_put(&lower->constants, instr->result, source->kind == ANVIL_VAL_CONST_INT ? source->data.i : constant);
        return anvil_ir_value_pair(lower, source, &a) && record_pair(lower, instr->result, a);
    }
    /* From 64 bits: the low word (a truncation, a pointer) or an FP conversion. */
    uint16_t bits = (uint16_t)(anvil_type_size(to) * 8);
    anvil_mir_vreg_t result = ANVIL_MIR_NO_VREG;
    if (!anvil_ir_value_pair(lower, source, &a))
        return false;
    if (to_fp)
        return anvil_wide_to_fp(lower->mir, &lower->wide, a, mir_op == ANVIL_MIR_OP_UITOFP, bits, &result) && anvil_lower_vreg_put(&lower->values, instr->result, result);
    if (anvil_type_is_floating(to))
        return false;
    if (to->kind == ANVIL_TYPE_I1) {
        result = lower->vreg_for_type(lower, to);
        if (!anvil_ir_low_bit(lower, anvil_wide_truncate(lower->mir, a, 8, false), result))
            return false;
    } else
        result = anvil_wide_truncate(lower->mir, a, bits, to->is_signed);
    return result != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->values, instr->result, result);
}

static bool big_endian(const anvil_ir_lower_t *lower)
{
    const anvil_arch_info_t *info = anvil_ctx_get_arch_info(lower->func->owner_ctx);
    return info && info->endian == ANVIL_ENDIAN_BIG;
}

/* A pair's words in memory order: the high word first on a big-endian
 * target. */
static void memory_order(const anvil_ir_lower_t *lower, anvil_wide_pair_t pair, anvil_mir_vreg_t words[2])
{
    bool high_first = big_endian(lower);
    words[0] = high_first ? pair.hi : pair.lo;
    words[1] = high_first ? pair.lo : pair.hi;
}

enum { PAIR_WORD_BYTES = 4 };

static bool pair_load(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_vreg_t base, words[2];
    int64_t displacement;
    anvil_wide_pair_t pair;
    if (!anvil_ir_access_address(lower, instr->operands[0], PAIR_WORD_BYTES, &base, &displacement) ||
        !anvil_wide_new(lower->mir, anvil_wide_value_unsigned(instr->result), &pair))
        return false;
    memory_order(lower, pair, words);
    for (int word = 0; word < 2; word++) {
        anvil_mir_vreg_t uses[] = {base};
        if (!anvil_ir_add_access(lower, ANVIL_MIR_OP_LOAD, words[word], uses, 1, displacement + word * PAIR_WORD_BYTES))
            return false;
    }
    return record_pair(lower, instr->result, pair);
}

static bool pair_store(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_vreg_t base, words[2];
    int64_t displacement;
    anvil_wide_pair_t pair;
    if (!anvil_ir_access_address(lower, instr->operands[1], PAIR_WORD_BYTES, &base, &displacement) || !anvil_ir_value_pair(lower, instr->operands[0], &pair))
        return false;
    memory_order(lower, pair, words);
    for (int word = 0; word < 2; word++) {
        anvil_mir_vreg_t uses[] = {words[word], base};
        if (!anvil_ir_add_access(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2, displacement + word * PAIR_WORD_BYTES))
            return false;
    }
    return true;
}

bool anvil_ir_pair_instr(const anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_opcode_t mir_op;
    if (!lower->pair_type)
        return false;
    switch (instr->op) {
    case ANVIL_OP_LOAD:
        return instr->num_operands == 1 && instr->result && anvil_ir_is_pair(lower, instr->result->type);
    case ANVIL_OP_STORE:
        return instr->num_operands == 2 && anvil_ir_is_pair(lower, instr->operands[0]->type);
    default:
        break;
    }
    bool operation = (instr->num_operands == 2 && anvil_ir_binary_op(instr->op, &mir_op)) || (instr->num_operands == 1 && anvil_ir_unary_op(instr->op, &mir_op)) ||
                     (instr->num_operands == 1 && anvil_ir_cast_op(instr->op, &mir_op) && !anvil_cast_keeps_value(instr));
    if (!operation)
        return false;
    /* A shift's count may be wider than the value it shifts. */
    size_t operands = is_shift(instr->op) ? 1 : instr->num_operands;
    for (size_t operand = 0; operand < operands; operand++)
        if (anvil_ir_is_pair(lower, instr->operands[operand]->type))
            return true;
    return instr->result && anvil_ir_is_pair(lower, instr->result->type);
}

bool anvil_ir_lower_pair_instr(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_opcode_t mir_op;
    if (instr->op == ANVIL_OP_LOAD)
        return pair_load(lower, instr);
    if (instr->op == ANVIL_OP_STORE)
        return pair_store(lower, instr);
    if (!instr->result)
        return false;
    if (instr->num_operands == 2 && anvil_ir_binary_op(instr->op, &mir_op))
        return pair_binary(lower, instr, mir_op);
    if (instr->num_operands == 1 && anvil_ir_unary_op(instr->op, &mir_op))
        return pair_unary(lower, instr);
    return instr->num_operands == 1 && anvil_ir_cast_op(instr->op, &mir_op) && pair_cast(lower, instr, mir_op);
}
