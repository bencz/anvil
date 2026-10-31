#include "mir_isel.h"

#include <stdlib.h>
#include <string.h>

static void count(uint8_t *counter, anvil_mir_vreg_t vreg)
{
    if (counter[vreg] < UINT8_MAX)
        counter[vreg]++;
}

bool anvil_mir_isel_is_compare(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP:
    case ANVIL_MIR_OP_CMP_EQ:
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_ULT:
    case ANVIL_MIR_OP_CMP_ULE:
    case ANVIL_MIR_OP_CMP_UGT:
    case ANVIL_MIR_OP_CMP_UGE:
        return true;
    default:
        return false;
    }
}

void anvil_mir_isel_facts_destroy(anvil_mir_isel_facts_t *facts)
{
    if (!facts)
        return;
    free(facts->def_count);
    free(facts->use_count);
    free(facts->constant);
    free(facts->materialize);
    free(facts->def_op);
    memset(facts, 0, sizeof(*facts));
}

bool anvil_mir_isel_facts_build(anvil_mir_isel_facts_t *facts, const anvil_mir_func_t *mir, anvil_mir_isel_takes_imm_fn takes_imm, void *ctx)
{
    memset(facts, 0, sizeof(*facts));
    facts->mir = mir;
    facts->num_vregs = anvil_mir_num_vregs(mir);
    size_t n = facts->num_vregs ? facts->num_vregs : 1;
    facts->def_count = calloc(n, sizeof(*facts->def_count));
    facts->use_count = calloc(n, sizeof(*facts->use_count));
    facts->constant = calloc(n, sizeof(*facts->constant));
    facts->materialize = calloc(n, sizeof(*facts->materialize));
    facts->def_op = calloc(n, sizeof(*facts->def_op));
    anvil_mir_vreg_t *copy_of = malloc(n * sizeof(*copy_of));
    uint8_t *definitions = calloc(n, sizeof(*definitions));
    if (!facts->def_count || !facts->use_count || !facts->constant || !facts->materialize || !facts->def_op || !copy_of || !definitions) {
        free(copy_of);
        free(definitions);
        anvil_mir_isel_facts_destroy(facts);
        return false;
    }
    for (size_t v = 0; v < n; v++)
        copy_of[v] = ANVIL_MIR_NO_VREG;

    size_t instrs = anvil_mir_num_instrs(mir);
    for (size_t i = 0; i < instrs; i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) {
            free(copy_of);
            free(definitions);
            anvil_mir_isel_facts_destroy(facts);
            return false;
        }
        if (info.def != ANVIL_MIR_NO_VREG && info.def < facts->num_vregs) {
            count(facts->def_count, info.def);
            if (info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.num_uses == 0)
                facts->constant[info.def] = info.imm;
            else if (info.op == ANVIL_MIR_OP_COPY && info.num_uses == 1)
                copy_of[info.def] = anvil_mir_get_instr_use(mir, i, 0);
            else
                facts->def_count[info.def] = UINT8_MAX;
            /* A second definition of any kind clears it. */
            if (definitions[info.def] < 2)
                definitions[info.def]++;
            facts->def_op[info.def] = definitions[info.def] == 1 ? info.op : ANVIL_MIR_OP_INVALID;
        }
        for (size_t u = 0; u < info.num_uses; u++) {
            anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
            if (use < facts->num_vregs)
                count(facts->use_count, use);
        }
    }

    /* A single-definition copy of a constant (a split or a phi move) holds
     * the constant too. */
    for (size_t v = 0; v < facts->num_vregs; v++) {
        if (copy_of[v] == ANVIL_MIR_NO_VREG || facts->def_count[v] != 1)
            continue;
        anvil_mir_vreg_t source = copy_of[v];
        for (unsigned depth = 0; source < facts->num_vregs && facts->def_count[source] == 1 && copy_of[source] != ANVIL_MIR_NO_VREG && depth < 8; depth++)
            source = copy_of[source];
        const anvil_mir_vreg_info_t *from = anvil_mir_get_vreg_info(mir, source), *to = anvil_mir_get_vreg_info(mir, (anvil_mir_vreg_t)v);
        if (source < facts->num_vregs && copy_of[source] == ANVIL_MIR_NO_VREG && facts->def_count[source] == 1 && from && to &&
            from->reg_class == to->reg_class && from->size_bits == to->size_bits)
            facts->constant[v] = facts->constant[source];
        else
            copy_of[v] = v;
    }
    for (size_t v = 0; v < facts->num_vregs; v++)
        if (copy_of[v] == v)
            facts->def_count[v] = UINT8_MAX;
    free(copy_of);
    free(definitions);

    for (size_t i = 0; i < instrs; i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) {
            anvil_mir_isel_facts_destroy(facts);
            return false;
        }
        for (size_t u = 0; u < info.num_uses; u++) {
            anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
            int64_t value;
            if (use >= facts->num_vregs)
                continue;
            if (!anvil_mir_isel_constant(facts, use, &value) || !takes_imm || !takes_imm(ctx, i, &info, u, value))
                facts->materialize[use] = true;
        }
    }
    for (size_t v = 0; v < facts->num_vregs; v++) {
        int64_t value;
        if (!anvil_mir_isel_constant(facts, (anvil_mir_vreg_t)v, &value))
            facts->materialize[v] = true;
    }
    return true;
}

bool anvil_mir_isel_constant(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out)
{
    if (!facts || !facts->def_count || vreg >= facts->num_vregs || facts->def_count[vreg] != 1)
        return false;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(facts->mir, vreg);
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return false;
    *out = facts->constant[vreg];
    return true;
}

anvil_mir_opcode_t anvil_mir_isel_def_op(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg)
{
    return facts && facts->def_op && vreg < facts->num_vregs ? facts->def_op[vreg] : ANVIL_MIR_OP_INVALID;
}

bool anvil_mir_isel_materialize(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg)
{
    return !facts || !facts->materialize || vreg >= facts->num_vregs || facts->materialize[vreg];
}

static bool feeds_next(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info, anvil_mir_opcode_t consumer)
{
    anvil_mir_instr_info_t next;
    return facts && facts->use_count && info->def != ANVIL_MIR_NO_VREG && info->def < facts->num_vregs &&
           facts->use_count[info->def] == 1 && anvil_mir_get_instr_info(facts->mir, index + 1, &next) &&
           next.op == consumer && next.block == info->block && anvil_mir_get_instr_use(facts->mir, index + 1, 0) == info->def;
}

bool anvil_mir_isel_feeds_next_branch(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info)
{
    return feeds_next(facts, index, info, ANVIL_MIR_OP_BR_COND);
}

bool anvil_mir_isel_feeds_next_select(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info)
{
    return feeds_next(facts, index, info, ANVIL_MIR_OP_SELECT);
}
