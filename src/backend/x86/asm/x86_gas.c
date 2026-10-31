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

bool x86_gas_emit_cond_branch(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t current,
                              const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    const char *inverse = x86_gas_inverse_jcc(cc);
    if (true_block == current + 1 && inverse)
        return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", inverse, false_block);
    return x86_gas_emit_jump_insn(code, label_prefix, mir, "j", cc, true_block) &&
           x86_gas_emit_jump(code, label_prefix, mir, current, false_block);
}

bool x86_gas_emit_jump(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t current,
                       anvil_mir_block_t target)
{
    return target == current + 1 || x86_gas_emit_jump_insn(code, label_prefix, mir, "jmp", "", target);
}
