#include "x86_gas.h"

#include <string.h>

const char *x86_gas_setcc(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "sete";
    case ANVIL_MIR_OP_CMP_LT:
        return "setl";
    case ANVIL_MIR_OP_CMP_LE:
        return "setle";
    case ANVIL_MIR_OP_CMP_GT:
        return "setg";
    case ANVIL_MIR_OP_CMP_GE:
        return "setge";
    case ANVIL_MIR_OP_CMP_ULT:
        return "setb";
    case ANVIL_MIR_OP_CMP_ULE:
        return "setbe";
    case ANVIL_MIR_OP_CMP_UGT:
        return "seta";
    case ANVIL_MIR_OP_CMP_UGE:
        return "setae";
    default:
        return "setne";
    }
}

const char *x86_gas_jcc(anvil_mir_opcode_t op)
{
    return x86_gas_setcc(op) + 3;
}

const char *x86_gas_inverse_jcc(const char *cc)
{
    static const char *const pairs[][2] = {{"e", "ne"}, {"l", "ge"}, {"le", "g"}, {"b", "ae"}, {"be", "a"}, {"p", "np"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (strcmp(cc, pairs[i][0]) == 0)
            return pairs[i][1];
        if (strcmp(cc, pairs[i][1]) == 0)
            return pairs[i][0];
    }
    return NULL;
}

bool x86_gas_fcmp_branch(anvil_fcmp_pred_t pred, const char **cc, bool *swap)
{
    static const struct {
        const char *cc;
        bool swap;
    } branches[] = {
        [ANVIL_FCMP_OGT] = {"a", false},  [ANVIL_FCMP_OGE] = {"ae", false}, [ANVIL_FCMP_OLT] = {"a", true},
        [ANVIL_FCMP_OLE] = {"ae", true},  [ANVIL_FCMP_ONE] = {"ne", false}, [ANVIL_FCMP_ORD] = {"np", false},
        [ANVIL_FCMP_UEQ] = {"e", false},  [ANVIL_FCMP_UGT] = {"b", true},   [ANVIL_FCMP_UGE] = {"be", true},
        [ANVIL_FCMP_ULT] = {"b", false},  [ANVIL_FCMP_ULE] = {"be", false}, [ANVIL_FCMP_UNO] = {"p", false},
    };
    if ((unsigned)pred > (unsigned)ANVIL_FCMP_TRUE || !branches[pred].cc)
        return false;
    *cc = branches[pred].cc;
    *swap = branches[pred].swap;
    return true;
}

void x86_gas_emit_fcmp_bool(anvil_strbuf_t *code, const char *label_prefix, const char *function, size_t index,
                            anvil_fcmp_pred_t pred, const char *dst8)
{
    /* Per predicate: 1 less (CF), 2 greater, 4 equal (ZF), 8 unordered (PF). */
    static const unsigned masks[] = {0, 4, 2, 6, 1, 5, 3, 7, 12, 10, 14, 9, 13, 11, 8, 15};
    unsigned mask = (unsigned)pred < sizeof(masks) / sizeof(masks[0]) ? masks[pred] : 0;
    anvil_strbuf_appendf(code, "\tmovb $0, %%%s\n", dst8);
    if (mask == 15) {
        anvil_strbuf_appendf(code, "\tmovb $1, %%%s\n", dst8);
        return;
    }
    if (mask == 0)
        return;
    anvil_strbuf_appendf(code, "\tjp %s%s_fcmp_%s_%zu\n", label_prefix, function, mask & 8 ? "true" : "done", index);
    if (mask & 1)
        anvil_strbuf_appendf(code, "\tjb %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    if (mask & 2)
        anvil_strbuf_appendf(code, "\tja %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    if (mask & 4)
        anvil_strbuf_appendf(code, "\tje %s%s_fcmp_true_%zu\n", label_prefix, function, index);
    anvil_strbuf_appendf(code, "\tjmp %s%s_fcmp_done_%zu\n%s%s_fcmp_true_%zu:\n\tmovb $1, %%%s\n%s%s_fcmp_done_%zu:\n", label_prefix, function,
                         index, label_prefix, function, index, dst8, label_prefix, function, index);
}

bool x86_gas_emit_cmov(anvil_strbuf_t *code, const char *cc, const char *suffix, const char *dst, const char *then_reg, const char *else_reg)
{
    const char *inverse = x86_gas_inverse_jcc(cc);
    if (!inverse)
        return false;
    if (strcmp(dst, then_reg) == 0) {
        anvil_strbuf_appendf(code, "\tcmov%s%s %%%s, %%%s\n", inverse, suffix, else_reg, dst);
        return true;
    }
    if (strcmp(dst, else_reg) != 0)
        anvil_strbuf_appendf(code, "\tmov%s %%%s, %%%s\n", suffix, else_reg, dst);
    anvil_strbuf_appendf(code, "\tcmov%s%s %%%s, %%%s\n", cc, suffix, then_reg, dst);
    return true;
}

static bool x86_gas_label_name(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(mir, block, &info))
        return false;
    anvil_strbuf_appendf(code, "%s%s_%s", label_prefix, anvil_mir_func_name(mir), info.name && info.name[0] ? info.name : "block");
    return true;
}

bool x86_gas_emit_label(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    if (!x86_gas_label_name(code, label_prefix, mir, block))
        return false;
    anvil_strbuf_append(code, ":\n");
    return true;
}

bool x86_gas_emit_branch_target(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block)
{
    return x86_gas_label_name(code, label_prefix, mir, block);
}

/* `<mnemonic><cc> <label of target>`. */
static bool x86_gas_emit_jump_insn(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const char *mnemonic,
                                   const char *cc, anvil_mir_block_t target)
{
    anvil_strbuf_appendf(code, "\t%s%s ", mnemonic, cc);
    if (!x86_gas_label_name(code, label_prefix, mir, target))
        return false;
    anvil_strbuf_append(code, "\n");
    return true;
}

bool x86_gas_emit_cond_branch(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                              anvil_mir_block_t current, const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    const char *inverse = x86_gas_inverse_jcc(cc);
    if (anvil_mir_layout_falls_into(layout, current, true_block) && inverse)
        return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", inverse, anvil_mir_layout_target(layout, false_block));
    return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", cc, anvil_mir_layout_target(layout, true_block)) &&
           x86_gas_emit_jump(code, label_prefix, mir, layout, current, false_block);
}

bool x86_gas_emit_jump(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, const anvil_mir_layout_t *layout,
                       anvil_mir_block_t current, anvil_mir_block_t target)
{
    return anvil_mir_layout_falls_into(layout, current, target) ||
           x86_gas_emit_jump_insn(code, label_prefix, mir, "jmp", "", anvil_mir_layout_target(layout, target));
}

bool x86_gas_compare_folds(const anvil_mir_func_t *mir, size_t reader, anvil_mir_vreg_t load, unsigned load_bits, bool *load_is_lhs,
                           anvil_mir_vreg_t *other)
{
    anvil_mir_instr_info_t info;
    if (!anvil_mir_get_instr_info(mir, reader, &info) || info.op < ANVIL_MIR_OP_CMP_EQ || info.op > ANVIL_MIR_OP_CMP_UGE || info.num_uses != 2)
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, reader, 0), rhs = anvil_mir_get_instr_use(mir, reader, 1);
    const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(mir, lhs);
    if (lhs == rhs || (lhs != load && rhs != load) || !lhs_info || lhs_info->reg_class != ANVIL_MIR_REG_GPR || lhs_info->size_bits != load_bits)
        return false;
    *load_is_lhs = lhs == load;
    *other = lhs == load ? rhs : lhs;
    return true;
}

void x86_gas_emit_memory_compare(anvil_strbuf_t *code, const char *suffix, const char *address, bool load_is_lhs, const char *other_reg)
{
    if (load_is_lhs)
        anvil_strbuf_appendf(code, "\tcmp%s %%%s, %s\n", suffix, other_reg, address);
    else
        anvil_strbuf_appendf(code, "\tcmp%s %s, %%%s\n", suffix, address, other_reg);
}
