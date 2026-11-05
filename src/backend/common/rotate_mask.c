#include "rotate_mask.h"

static uint64_t low_bits(unsigned bits)
{
    return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
}

static uint64_t rotate_left(uint64_t value, unsigned amount, unsigned width)
{
    value &= low_bits(width);
    amount %= width;
    return amount ? ((value << amount) | (value >> (width - amount))) & low_bits(width) : value;
}

bool anvil_rotate_mask_of(anvil_mir_opcode_t op, int64_t imm, unsigned bits, unsigned width, anvil_rotate_mask_t *out)
{
    if ((bits != 32 && bits != 64) || (width != 32 && width != 64) || bits > width)
        return false;
    uint64_t operand = low_bits(bits);
    out->width = width;
    switch (op) {
    case ANVIL_MIR_OP_MUL: {
        uint64_t value = (uint64_t)imm & operand;
        if (!value || (value & (value - 1)))
            return false;
        unsigned shift = 0;
        while ((UINT64_C(1) << shift) != value)
            shift++;
        imm = shift;
    }
        /* fall through */
    case ANVIL_MIR_OP_SHL:
        if (imm < 0 || (uint64_t)imm >= bits)
            return false;
        /* The bits rotated in at the bottom are masked off; with bits <
         * width the register's upper bits rotate in there too. */
        out->rotate = (unsigned)imm;
        out->mask = operand & ~low_bits((unsigned)imm);
        return true;
    case ANVIL_MIR_OP_SHR:
        if (imm < 0 || (uint64_t)imm >= bits)
            return false;
        out->rotate = (width - (unsigned)imm) % width;
        out->mask = low_bits(bits - (unsigned)imm);
        return true;
    case ANVIL_MIR_OP_AND:
        out->rotate = 0;
        out->mask = (uint64_t)imm & operand;
        return out->mask != 0;
    default:
        return false;
    }
}

anvil_rotate_mask_t anvil_rotate_mask_then(const anvil_rotate_mask_t *first, const anvil_rotate_mask_t *second)
{
    /* rotl(rotl(x, r1) & m1, r2) & m2 = rotl(x, r1 + r2) & rotl(m1, r2) & m2 */
    anvil_rotate_mask_t out;
    out.width = first->width;
    out.rotate = (first->rotate + second->rotate) % first->width;
    out.mask = rotate_left(first->mask, second->rotate, first->width) & second->mask;
    return out;
}

bool anvil_rotate_mask_run(const anvil_rotate_mask_t *rm, unsigned *begin, unsigned *end)
{
    uint64_t mask = rm->mask & low_bits(rm->width);
    if (!mask)
        return false;
    unsigned low = 0, high = rm->width - 1;
    while (!((mask >> low) & 1))
        low++;
    while (!((mask >> high) & 1))
        high--;
    /* One run: every bit from the lowest set one to the highest is set. */
    if (mask != (low_bits(high + 1) & ~low_bits(low)))
        return false;
    *begin = rm->width - 1 - high;
    *end = rm->width - 1 - low;
    return true;
}

/* The operand width (32 or 64; 0: neither) of GPR vreg `vreg`. */
static unsigned gpr_bits(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = vreg == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, vreg);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return 0;
    return info->size_bits == 32 || info->size_bits == 64 ? info->size_bits : 0;
}

/* The instruction at `index` on `bits`-bit operands: its constant and
 * the operand it applies to (the first; either for an AND). */
static bool instr_operand(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info,
                          int64_t *imm, anvil_mir_vreg_t *operand, unsigned *bits)
{
    if (info->num_uses != 2 || info->def == ANVIL_MIR_NO_VREG ||
        (info->op != ANVIL_MIR_OP_SHL && info->op != ANVIL_MIR_OP_SHR && info->op != ANVIL_MIR_OP_AND && info->op != ANVIL_MIR_OP_MUL))
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, index, 0), rhs = anvil_mir_get_instr_use(mir, index, 1);
    if (anvil_mir_isel_constant(facts, rhs, imm))
        *operand = lhs;
    else if (info->op == ANVIL_MIR_OP_AND && anvil_mir_isel_constant(facts, lhs, imm))
        *operand = rhs;
    else
        return false;
    *bits = gpr_bits(mir, info->def);
    return *bits && gpr_bits(mir, *operand) == *bits;
}

bool anvil_mir_rotate_mask_pair(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, unsigned min_width,
                                anvil_rotate_mask_t *out, size_t *reader, size_t *between, anvil_mir_vreg_t *source)
{
    anvil_mir_instr_info_t info, next;
    int64_t first_imm, second_imm;
    unsigned first_bits, second_bits;
    anvil_mir_vreg_t operand;
    if (!anvil_mir_get_instr_info(mir, index, &info) || !instr_operand(mir, facts, index, &info, &first_imm, source, &first_bits))
        return false;
    size_t at = anvil_mir_isel_fold_reader(mir, facts, index, 8, false);
    if (at == SIZE_MAX || !anvil_mir_get_instr_info(mir, at, &next))
        return false;
    *between = SIZE_MAX;
    anvil_mir_vreg_t read = info.def;
    if (next.op == ANVIL_MIR_OP_ZEXT && first_bits == 32 && gpr_bits(mir, next.def) == 64) {
        /* The zero extension's reader, the first operand's register
         * still holding it there. */
        size_t extension = at;
        read = next.def;
        at = anvil_mir_isel_only_reader(mir, facts, extension, 8);
        if (at == SIZE_MAX || !anvil_mir_isel_keeps(mir, facts, extension, at, *source) || !anvil_mir_get_instr_info(mir, at, &next))
            return false;
        *between = extension;
    }
    if (!instr_operand(mir, facts, at, &next, &second_imm, &operand, &second_bits) || operand != read ||
        (*between == SIZE_MAX ? second_bits != first_bits : second_bits != 64))
        return false;
    unsigned width = second_bits > min_width ? second_bits : min_width;
    anvil_rotate_mask_t first, second;
    if (!anvil_rotate_mask_of(info.op, first_imm, first_bits, width, &first) || !anvil_rotate_mask_of(next.op, second_imm, second_bits, width, &second))
        return false;
    *out = anvil_rotate_mask_then(&first, &second);
    *reader = at;
    return true;
}
