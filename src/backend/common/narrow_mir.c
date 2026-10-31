#include "narrow_mir.h"

/* Extension an operand needs: 1 sign, 0 zero, -1 none. */
static int operand_extension(anvil_mir_opcode_t op, size_t operand)
{
    switch (op) {
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_GE:
        return 1;
    case ANVIL_MIR_OP_SAR:
        return operand == 0 ? 1 : 0;
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_UMOD:
    case ANVIL_MIR_OP_CMP_ULT:
    case ANVIL_MIR_OP_CMP_ULE:
    case ANVIL_MIR_OP_CMP_UGT:
    case ANVIL_MIR_OP_CMP_UGE:
    case ANVIL_MIR_OP_CMP_EQ:
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP:
        return 0;
    case ANVIL_MIR_OP_SHL:
        /* Only the count's high bits matter. */
        return operand == 1 ? 0 : -1;
    default:
        return -1;
    }
}

static bool is_compare(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_CMP || (op >= ANVIL_MIR_OP_CMP_EQ && op <= ANVIL_MIR_OP_CMP_UGE);
}

anvil_mir_vreg_t anvil_mir_extend_gpr(anvil_mir_func_t *mir, anvil_mir_vreg_t src, uint16_t bits, bool sign)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, src);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || info->size_bits >= bits)
        return src;
    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, bits, sign);
    anvil_mir_vreg_t uses[] = {src};
    if (wide == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(mir, sign ? ANVIL_MIR_OP_SEXT : ANVIL_MIR_OP_ZEXT, wide, uses, 1))
        return ANVIL_MIR_NO_VREG;
    return wide;
}

static bool narrow_gpr(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, uint16_t min_bits)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    return info && info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits < min_bits;
}

bool anvil_mir_add_binary_extending(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs,
                                    anvil_mir_vreg_t rhs, uint16_t min_bits)
{
    int lhs_ext = operand_extension(op, 0), rhs_ext = operand_extension(op, 1);
    bool needed = (lhs_ext >= 0 && narrow_gpr(mir, lhs, min_bits)) || (rhs_ext >= 0 && narrow_gpr(mir, rhs, min_bits));
    if (needed) {
        /* Both operands move to min_bits; where an operand's high bits do
         * not matter either extension serves. */
        lhs = anvil_mir_extend_gpr(mir, lhs, min_bits, lhs_ext == 1);
        rhs = anvil_mir_extend_gpr(mir, rhs, min_bits, rhs_ext == 1);
        if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG)
            return false;
    }
    anvil_mir_vreg_t uses[] = {lhs, rhs};
    if (!needed || is_compare(op) || !narrow_gpr(mir, def, min_bits))
        return anvil_mir_add_instr(mir, op, def, uses, 2);
    /* Compute at min_bits, then narrow into the result. */
    const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(mir, def);
    bool def_signed = def_info && def_info->is_signed;
    anvil_mir_vreg_t wide = anvil_mir_add_vreg_typed(mir, ANVIL_MIR_REG_GPR, min_bits, def_signed);
    anvil_mir_vreg_t narrow_use[] = {wide};
    return wide != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(mir, op, wide, uses, 2) &&
           anvil_mir_add_instr(mir, ANVIL_MIR_OP_TRUNC, def, narrow_use, 1);
}

anvil_mir_vreg_t anvil_mir_int_to_fp_source(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t src, uint16_t min_bits)
{
    if ((op != ANVIL_MIR_OP_SITOFP && op != ANVIL_MIR_OP_UITOFP) || !narrow_gpr(mir, src, min_bits))
        return src;
    return anvil_mir_extend_gpr(mir, src, min_bits, op == ANVIL_MIR_OP_SITOFP);
}
