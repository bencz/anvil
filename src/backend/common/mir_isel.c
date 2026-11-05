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
    free(facts->symbol);
    free(facts->materialize);
    free(facts->def_op);
    free(facts->width);
    free(facts->low_bits);
    memset(facts, 0, sizeof(*facts));
}

static unsigned bit_length(uint64_t value)
{
    unsigned bits = 0;
    while (value) {
        bits++;
        value >>= 1;
    }
    return bits ? bits : 1;
}

/* Low bits of a GPR vreg's register that can be nonzero, from what its
 * single definition at `index` is (0: unknown). */
static uint8_t value_width(const anvil_mir_isel_facts_t *facts, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info,
                           const anvil_mir_vreg_t *copy_of)
{
    const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, info->def);
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR)
        return 0;
    anvil_mir_vreg_t a = info->num_uses > 0 ? anvil_mir_get_instr_use(mir, index, 0) : ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t b = info->num_uses > 1 ? anvil_mir_get_instr_use(mir, index, 1) : ANVIL_MIR_NO_VREG;
    const anvil_mir_vreg_info_t *source = a != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(mir, a) : NULL;
    int64_t value;
    switch (info->op) {
    case ANVIL_MIR_OP_MOV:
        return info->has_imm && info->imm >= 0 ? (uint8_t)bit_length((uint64_t)info->imm) : 0;
    case ANVIL_MIR_OP_AND: {
        unsigned width = 0;
        if (anvil_mir_isel_constant(facts, a, &value) && value >= 0)
            width = bit_length((uint64_t)value);
        if (anvil_mir_isel_constant(facts, b, &value) && value >= 0 && (!width || bit_length((uint64_t)value) < width))
            width = bit_length((uint64_t)value);
        return (uint8_t)width;
    }
    case ANVIL_MIR_OP_ZEXT:
        return source ? (uint8_t)source->size_bits : 0;
    case ANVIL_MIR_OP_LOAD:
        return def->is_signed ? 0 : (uint8_t)def->size_bits;
    case ANVIL_MIR_OP_SHR:
        return source && anvil_mir_isel_constant(facts, b, &value) && value > 0 && value < source->size_bits ? (uint8_t)(source->size_bits - value) : 0;
    case ANVIL_MIR_OP_COPY:
        /* copy_of: the copied value, each side defined once (a bitcast's
         * signedness change); its width when it came earlier. */
        return copy_of[info->def] != ANVIL_MIR_NO_VREG && copy_of[info->def] != info->def && copy_of[info->def] < facts->num_vregs
                   ? facts->width[copy_of[info->def]]
                   : 0;
    default:
        return anvil_mir_isel_is_compare(info->op) ? 1 : 0;
    }
}

bool anvil_mir_isel_facts_build(anvil_mir_isel_facts_t *facts, const anvil_mir_func_t *mir, anvil_mir_isel_takes_imm_fn takes_imm,
                                anvil_mir_isel_takes_symbol_fn takes_symbol, void *ctx)
{
    memset(facts, 0, sizeof(*facts));
    facts->mir = mir;
    facts->num_vregs = anvil_mir_num_vregs(mir);
    size_t n = facts->num_vregs ? facts->num_vregs : 1;
    facts->def_count = calloc(n, sizeof(*facts->def_count));
    facts->use_count = calloc(n, sizeof(*facts->use_count));
    facts->constant = calloc(n, sizeof(*facts->constant));
    facts->symbol = calloc(n, sizeof(*facts->symbol));
    facts->materialize = calloc(n, sizeof(*facts->materialize));
    facts->def_op = calloc(n, sizeof(*facts->def_op));
    facts->width = calloc(n, sizeof(*facts->width));
    anvil_mir_vreg_t *copy_of = malloc(n * sizeof(*copy_of));
    uint8_t *definitions = calloc(n, sizeof(*definitions));
    if (!facts->def_count || !facts->use_count || !facts->constant || !facts->symbol || !facts->materialize || !facts->def_op || !facts->width || !copy_of || !definitions) {
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
            /* The same constant again (a test block copied into a loop
             * latch defines its constants twice alike): still one value. */
            bool again = definitions[info.def] == 1 && facts->def_count[info.def] == 1 && facts->def_op[info.def] == info.op &&
                         ((info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.num_uses == 0 && !facts->symbol[info.def] &&
                           facts->constant[info.def] == info.imm) ||
                          (info.op == ANVIL_MIR_OP_SYMBOL_ADDR && info.symbol && !info.symbol_got && info.num_uses == 0 && facts->symbol[info.def] &&
                           strcmp(facts->symbol[info.def], info.symbol) == 0 && facts->constant[info.def] == (info.has_imm ? info.imm : 0)));
            if (again)
                continue;
            count(facts->def_count, info.def);
            if (info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.num_uses == 0)
                facts->constant[info.def] = info.imm;
            else if (info.op == ANVIL_MIR_OP_SYMBOL_ADDR && info.symbol && !info.symbol_got && info.num_uses == 0) {
                facts->symbol[info.def] = info.symbol;
                facts->constant[info.def] = info.has_imm ? info.imm : 0;
            } else if (info.op == ANVIL_MIR_OP_COPY && info.num_uses == 1)
                copy_of[info.def] = anvil_mir_get_instr_use(mir, i, 0);
            else
                facts->def_count[info.def] = UINT8_MAX;
            /* A second definition of any kind clears it. */
            if (definitions[info.def] < 2)
                definitions[info.def]++;
            /* The opcode every definition shares (a test block copied
             * into a loop latch defines its values twice alike). */
            facts->def_op[info.def] = definitions[info.def] == 1 || facts->def_op[info.def] == info.op ? info.op : ANVIL_MIR_OP_INVALID;
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
            from->reg_class == to->reg_class && from->size_bits == to->size_bits) {
            facts->constant[v] = facts->constant[source];
            facts->symbol[v] = facts->symbol[source];
        } else
            copy_of[v] = v;
    }
    for (size_t v = 0; v < facts->num_vregs; v++)
        if (copy_of[v] == v)
            facts->def_count[v] = UINT8_MAX;
    /* For widths: a copy defined once of a value defined once, alike in
     * class and size. */
    for (size_t i = 0; i < instrs; i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info) || info.op != ANVIL_MIR_OP_COPY || info.num_uses != 1 || info.def >= facts->num_vregs)
            continue;
        anvil_mir_vreg_t source = anvil_mir_get_instr_use(mir, i, 0);
        const anvil_mir_vreg_info_t *from = anvil_mir_get_vreg_info(mir, source), *to = anvil_mir_get_vreg_info(mir, info.def);
        if (copy_of[info.def] == info.def && source < facts->num_vregs && definitions[info.def] == 1 && definitions[source] == 1 && from && to &&
            from->reg_class == to->reg_class && from->size_bits == to->size_bits)
            copy_of[info.def] = source;
    }
    free(definitions);

    /* Known-zero upper bits: the widest over every definition, unknown if
     * any is (in instruction order: an operand defined later is still
     * unknown, which is safe). */
    uint8_t *seen = calloc(n, sizeof(*seen));
    for (size_t i = 0; seen && i < instrs; i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info) || info.def == ANVIL_MIR_NO_VREG || info.def >= facts->num_vregs)
            continue;
        uint8_t width = facts->def_op[info.def] == ANVIL_MIR_OP_INVALID ? 0 : value_width(facts, mir, i, &info, copy_of);
        facts->width[info.def] = !seen[info.def] ? width : (width && facts->width[info.def] ? (width > facts->width[info.def] ? width : facts->width[info.def]) : 0);
        seen[info.def] = 1;
    }
    free(seen);
    free(copy_of);

    for (size_t i = 0; i < instrs; i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) {
            anvil_mir_isel_facts_destroy(facts);
            return false;
        }
        for (size_t u = 0; u < info.num_uses; u++) {
            anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
            int64_t value;
            const char *symbol;
            if (use >= facts->num_vregs)
                continue;
            bool in_place = anvil_mir_isel_constant(facts, use, &value) ? takes_imm && takes_imm(ctx, i, &info, u, value)
                          : anvil_mir_isel_symbol(facts, use, &symbol, &value) ? takes_symbol && takes_symbol(ctx, i, &info, u, symbol, value)
                          : false;
            if (!in_place)
                facts->materialize[use] = true;
        }
    }
    for (size_t v = 0; v < facts->num_vregs; v++) {
        int64_t value;
        const char *symbol;
        if (!anvil_mir_isel_constant(facts, (anvil_mir_vreg_t)v, &value) && !anvil_mir_isel_symbol(facts, (anvil_mir_vreg_t)v, &symbol, &value))
            facts->materialize[v] = true;
    }

    /* Readers that need only a value's own bits: stores of it,
     * truncations, == / != compares with zero, and extensions whose result
     * is only compared with zero that way (an extension keeps zero-ness,
     * whether or not it extends). */
    facts->low_bits = malloc(n * sizeof(*facts->low_bits));
    bool *zero_only = malloc(n * sizeof(*zero_only));
    uint8_t *defined = calloc(n, sizeof(*defined));
    if (!facts->low_bits || !zero_only || !defined) {
        free(zero_only);
        free(defined);
        anvil_mir_isel_facts_destroy(facts);
        return false;
    }
    for (size_t v = 0; v < n; v++)
        facts->low_bits[v] = zero_only[v] = true;
    for (bool narrowed = true; narrowed;) {
        narrowed = false;
        for (size_t i = 0; i < instrs; i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, i, &info))
                continue;
            if (info.def != ANVIL_MIR_NO_VREG && info.def < facts->num_vregs && defined[info.def] < 2)
                defined[info.def] = 1;
            for (size_t u = 0; u < info.num_uses; u++) {
                anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
                if (use >= facts->num_vregs)
                    continue;
                int64_t other = 1;
                bool zero_test = (info.op == ANVIL_MIR_OP_CMP_EQ || info.op == ANVIL_MIR_OP_CMP_NE) && info.num_uses == 2 &&
                                 anvil_mir_isel_constant(facts, anvil_mir_get_instr_use(mir, i, 1 - u), &other) && other == 0;
                bool extension = (info.op == ANVIL_MIR_OP_SEXT || info.op == ANVIL_MIR_OP_ZEXT) && info.def != ANVIL_MIR_NO_VREG &&
                                 info.def < facts->num_vregs && zero_only[info.def];
                bool zero = zero_test || extension;
                bool low = zero || (info.op == ANVIL_MIR_OP_STORE && u == 0) || info.op == ANVIL_MIR_OP_TRUNC;
                if (!zero && zero_only[use]) {
                    zero_only[use] = false;
                    narrowed = true;
                }
                facts->low_bits[use] = facts->low_bits[use] && low;
            }
        }
    }
    /* About readers only: every definition may leave the upper bits as
     * they come. */
    for (size_t v = 0; v < facts->num_vregs; v++)
        facts->low_bits[v] = facts->low_bits[v] && defined[v] > 0;
    free(zero_only);
    free(defined);
    return true;
}

bool anvil_mir_isel_low_bits_only(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg)
{
    return facts && facts->low_bits && vreg < facts->num_vregs && facts->low_bits[vreg];
}

/* A single-definition GPR value whose definition the facts recorded. */
static bool known_value(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg)
{
    if (!facts || !facts->def_count || vreg >= facts->num_vregs || facts->def_count[vreg] != 1)
        return false;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(facts->mir, vreg);
    return info && info->reg_class == ANVIL_MIR_REG_GPR;
}

bool anvil_mir_isel_constant(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out)
{
    if (!known_value(facts, vreg) || facts->symbol[vreg])
        return false;
    *out = facts->constant[vreg];
    return true;
}

bool anvil_mir_isel_symbol(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, const char **symbol, int64_t *offset)
{
    if (!known_value(facts, vreg) || !facts->symbol[vreg])
        return false;
    *symbol = facts->symbol[vreg];
    *offset = facts->constant[vreg];
    return true;
}

bool anvil_mir_isel_zero_extended(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg)
{
    if (!facts || !facts->width || vreg >= facts->num_vregs || !facts->width[vreg])
        return false;
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(facts->mir, vreg);
    return info && facts->width[vreg] <= info->size_bits;
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

/* Whether instruction `index` reads `vreg` as a == / != test with zero. */
static bool zero_test_of(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info,
                         anvil_mir_vreg_t vreg)
{
    int64_t value = 1;
    return (info->op == ANVIL_MIR_OP_CMP_EQ || info->op == ANVIL_MIR_OP_CMP_NE) && info->num_uses == 2 &&
           anvil_mir_get_instr_use(mir, index, 0) == vreg && anvil_mir_get_instr_use(mir, index, 1) != vreg &&
           anvil_mir_isel_constant(facts, anvil_mir_get_instr_use(mir, index, 1), &value) && value == 0;
}

bool anvil_mir_counter_loops(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, bool calls_clobber_counter, bool counts_in_place,
                             uint8_t *roles)
{
    size_t count = anvil_mir_num_instrs(mir);
    for (size_t i = 0; i < count; i++)
        roles[i] = ANVIL_MIR_COUNTER_NONE;
    for (size_t step = 0; step + 2 < count; step++) {
        anvil_mir_instr_info_t add, test, branch;
        int64_t one = 0;
        if (!anvil_mir_get_instr_info(mir, step, &add) || add.op != ANVIL_MIR_OP_ADD || add.num_uses != 2 || add.def == ANVIL_MIR_NO_VREG ||
            anvil_mir_get_instr_use(mir, step, 0) != add.def || !anvil_mir_isel_constant(facts, anvil_mir_get_instr_use(mir, step, 1), &one) ||
            one != 1)
            continue;
        anvil_mir_vreg_t counter = add.def;
        /* The test: the next reader of j in the latch, then the branch. */
        size_t at = step + 1;
        while (at < count && anvil_mir_get_instr_info(mir, at, &test) && test.block == add.block && test.op == ANVIL_MIR_OP_MOV && test.num_uses == 0)
            at++;
        if (at + 1 >= count || !anvil_mir_get_instr_info(mir, at, &test) || test.block != add.block || test.op != ANVIL_MIR_OP_CMP_NE ||
            !zero_test_of(mir, facts, at, &test, counter) || !anvil_mir_isel_feeds_next_branch(facts, at, &test) ||
            !anvil_mir_get_instr_info(mir, at + 1, &branch) || branch.op != ANVIL_MIR_OP_BR_COND || branch.true_block > add.block)
            continue;
        anvil_mir_block_t header = branch.true_block, latch = add.block;
        size_t init = SIZE_MAX, loop_first = SIZE_MAX, last_outside_read = 0;
        bool fits = true;
        for (size_t k = 0; k < count && fits; k++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(mir, k, &info)) {
                fits = false;
                break;
            }
            bool inside = info.block >= header && info.block <= latch;
            if (inside && loop_first == SIZE_MAX)
                loop_first = k;
            if (info.def == counter && k != step) {
                fits = !inside && init == SIZE_MAX && info.block < header;
                init = k;
            }
            bool reads = false;
            for (size_t u = 0; u < info.num_uses; u++)
                reads |= anvil_mir_get_instr_use(mir, k, u) == counter;
            if (reads && k != step && k != at && (inside || (counts_in_place && !zero_test_of(mir, facts, k, &info, counter))))
                fits = false;
            if (reads && !inside)
                last_outside_read = k;
            /* Innermost: no other branch back inside the loop. */
            if (inside && k != at + 1 && (info.op == ANVIL_MIR_OP_BR || info.op == ANVIL_MIR_OP_BR_COND) &&
                ((info.true_block >= header && info.true_block <= info.block) ||
                 (info.op == ANVIL_MIR_OP_BR_COND && info.false_block >= header && info.false_block <= info.block)))
                fits = false;
            if (inside && roles[k] != ANVIL_MIR_COUNTER_NONE)
                fits = false;
        }
        /* Outside, only before the loop: the counter's register need not
         * hold j once it runs. */
        if (!fits || init == SIZE_MAX || loop_first == SIZE_MAX || last_outside_read > loop_first)
            continue;
        if (calls_clobber_counter) {
            for (size_t k = init + 1; k < count && fits; k++) {
                anvil_mir_instr_info_t info;
                if (!anvil_mir_get_instr_info(mir, k, &info) || (k >= loop_first && !(info.block >= header && info.block <= latch)))
                    continue;
                /* A call, or an instruction whose printed form calls a
                 * helper (it declares the call's clobbers). */
                bool clobbers = info.op == ANVIL_MIR_OP_CALL;
                for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
                    clobbers |= info.clobbers[c] != 0;
                if (clobbers)
                    fits = false;
            }
        }
        if (!fits)
            continue;
        roles[init] = ANVIL_MIR_COUNTER_INIT;
        roles[step] = ANVIL_MIR_COUNTER_STEP;
        roles[at] = ANVIL_MIR_COUNTER_TEST;
        roles[at + 1] = ANVIL_MIR_COUNTER_BRANCH;
    }
    return true;
}

/* A constant the printers never materialize: it writes no register. */
static bool silent(const anvil_mir_isel_facts_t *facts, const anvil_mir_instr_info_t *info)
{
    return (info->op == ANVIL_MIR_OP_MOV || info->op == ANVIL_MIR_OP_SYMBOL_ADDR) && info->def != ANVIL_MIR_NO_VREG &&
           !anvil_mir_isel_materialize(facts, info->def);
}

/* The register `vreg` is in (-1: none, spilled or unassigned). */
static int fold_phys(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *at = vreg == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_assignment(mir, vreg);
    const anvil_mir_vreg_info_t *info = vreg == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, vreg);
    /* A registerless value holds no register: -2, read or written by none. */
    if (info && info->registerless)
        return -2;
    return at && !at->spilled ? at->phys_reg : -1;
}

size_t anvil_mir_isel_fold_reader(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t window, bool memory)
{
    anvil_mir_instr_info_t info, next;
    if (!anvil_mir_get_instr_info(mir, index, &info) || info.def == ANVIL_MIR_NO_VREG || facts->use_count[info.def] != 1 || info.num_uses > 4)
        return SIZE_MAX;
    /* The registers it reads, by class. */
    int read_phys[4];
    anvil_mir_reg_class_t read_class[4];
    for (size_t u = 0; u < info.num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, index, u);
        const anvil_mir_vreg_info_t *use_info = use == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, use);
        read_phys[u] = fold_phys(mir, use);
        if (!use_info || read_phys[u] == -1)
            return SIZE_MAX;
        read_class[u] = use_info->reg_class;
    }
    size_t count = anvil_mir_num_instrs(mir);
    for (size_t at = index + 1; at < count && at <= index + window; at++) {
        if (!anvil_mir_get_instr_info(mir, at, &next) || next.block != info.block)
            return SIZE_MAX;
        for (size_t u = 0; u < next.num_uses; u++) {
            if (anvil_mir_get_instr_use(mir, at, u) == info.def)
                return at;
        }
        if (next.op == ANVIL_MIR_OP_CALL || next.op == ANVIL_MIR_OP_DYN_ALLOCA || next.op == ANVIL_MIR_OP_CALL_STACK_ARG)
            return SIZE_MAX;
        if (memory && (next.op == ANVIL_MIR_OP_STORE || next.op == ANVIL_MIR_OP_ATOMIC || next.op == ANVIL_MIR_OP_SPILL_STORE))
            return SIZE_MAX;
        if (silent(facts, &next))
            continue;
        const anvil_mir_vreg_info_t *def = next.def == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, next.def);
        int written = fold_phys(mir, next.def);
        for (size_t u = 0; u < info.num_uses; u++) {
            if (read_phys[u] < 0)
                continue;  /* registerless: nothing to keep */
            uint64_t lost = next.clobbers[read_class[u]] | next.scratch[read_class[u]];
            if ((read_phys[u] >= 64 ? lost != 0 : (lost >> read_phys[u]) & 1) || (def && def->reg_class == read_class[u] && written == read_phys[u]))
                return SIZE_MAX;
        }
    }
    return SIZE_MAX;
}

bool anvil_mir_isel_keeps(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t from, size_t to, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *held = vreg == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, vreg);
    int phys = fold_phys(mir, vreg);
    if (held && phys == -2)
        return true;  /* registerless: nothing to keep */
    if (!held || phys < 0)
        return false;
    anvil_mir_instr_info_t next;
    for (size_t at = from + 1; at < to; at++) {
        if (!anvil_mir_get_instr_info(mir, at, &next) || next.op == ANVIL_MIR_OP_CALL || next.op == ANVIL_MIR_OP_DYN_ALLOCA ||
            next.op == ANVIL_MIR_OP_CALL_STACK_ARG)
            return false;
        if (silent(facts, &next))
            continue;
        uint64_t lost = next.clobbers[held->reg_class] | next.scratch[held->reg_class];
        const anvil_mir_vreg_info_t *def = next.def == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, next.def);
        if ((phys >= 64 ? lost != 0 : (lost >> phys) & 1) || (def && def->reg_class == held->reg_class && fold_phys(mir, next.def) == phys))
            return false;
    }
    return true;
}

size_t anvil_mir_isel_only_reader(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t window)
{
    anvil_mir_instr_info_t info, next;
    if (!anvil_mir_get_instr_info(mir, index, &info) || info.def == ANVIL_MIR_NO_VREG || facts->use_count[info.def] != 1)
        return SIZE_MAX;
    size_t count = anvil_mir_num_instrs(mir);
    for (size_t at = index + 1; at < count && at <= index + window; at++) {
        if (!anvil_mir_get_instr_info(mir, at, &next) || next.block != info.block)
            return SIZE_MAX;
        for (size_t u = 0; u < next.num_uses; u++) {
            if (anvil_mir_get_instr_use(mir, at, u) == info.def)
                return at;
        }
        if (next.op == ANVIL_MIR_OP_CALL || next.op == ANVIL_MIR_OP_DYN_ALLOCA || next.op == ANVIL_MIR_OP_CALL_STACK_ARG)
            return SIZE_MAX;
    }
    return SIZE_MAX;
}

bool anvil_mir_isel_writes(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, anvil_mir_reg_class_t reg_class, int phys)
{
    anvil_mir_instr_info_t info;
    for (size_t at = 0; at < anvil_mir_num_instrs(mir); at++) {
        if (!anvil_mir_get_instr_info(mir, at, &info))
            return true;
        if (phys < 64 && ((info.scratch[reg_class] >> phys) & 1))
            return true;
        if (info.def == ANVIL_MIR_NO_VREG || silent(facts, &info))
            continue;
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, info.def);
        if (def && !def->registerless && def->reg_class == reg_class && fold_phys(mir, info.def) == phys)
            return true;
    }
    return false;
}

bool anvil_mir_isel_mark_registerless(anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts)
{
    anvil_mir_instr_info_t info;
    /* Definitions per value (facts->def_count counts constants only). */
    uint8_t *definitions = calloc(facts->num_vregs ? facts->num_vregs : 1, sizeof(*definitions));
    if (!definitions)
        return false;
    for (size_t at = 0; at < anvil_mir_num_instrs(mir); at++) {
        if (!anvil_mir_get_instr_info(mir, at, &info)) {
            free(definitions);
            return false;
        }
        if (info.def != ANVIL_MIR_NO_VREG && info.def < facts->num_vregs && definitions[info.def] < 2)
            definitions[info.def]++;
    }
    bool ok = true;
    for (size_t at = 0; ok && at < anvil_mir_num_instrs(mir); at++) {
        if (!anvil_mir_get_instr_info(mir, at, &info)) {
            ok = false;
            break;
        }
        if (info.def == ANVIL_MIR_NO_VREG || info.def >= facts->num_vregs)
            continue;
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, info.def);
        if (!def || def->has_fixed_reg || def->is_live_in || def->reg_class != ANVIL_MIR_REG_GPR)
            continue;
        bool constant = silent(facts, &info);
        bool flags = anvil_mir_isel_is_compare(info.op) && info.op != ANVIL_MIR_OP_FCMP && definitions[info.def] == 1 &&
                     facts->use_count[info.def] == 1 && anvil_mir_isel_feeds_next_branch(facts, at, &info);
        if (flags) {
            const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, at, 0));
            flags = lhs && lhs->reg_class == ANVIL_MIR_REG_GPR;
        }
        if ((constant || flags) && !anvil_mir_set_registerless(mir, info.def, true))
            ok = false;
    }
    free(definitions);
    return ok;
}

bool anvil_mir_isel_registerless_ok(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info)
{
    const anvil_mir_vreg_info_t *def = info->def == ANVIL_MIR_NO_VREG ? NULL : anvil_mir_get_vreg_info(mir, info->def);
    if (!def || !def->registerless)
        return true;
    return silent(facts, info) || (anvil_mir_isel_is_compare(info->op) && anvil_mir_isel_feeds_next_branch(facts, index, info));
}

bool anvil_mir_layout_build(anvil_mir_layout_t *layout, const anvil_mir_func_t *mir)
{
    memset(layout, 0, sizeof(*layout));
    size_t blocks = anvil_mir_num_blocks(mir);
    layout->num_blocks = blocks;
    layout->forward = malloc((blocks ? blocks : 1) * sizeof(*layout->forward));
    layout->next = malloc((blocks ? blocks : 1) * sizeof(*layout->next));
    layout->skip = calloc(blocks ? blocks : 1, sizeof(*layout->skip));
    bool *candidate = malloc((blocks ? blocks : 1) * sizeof(*candidate));
    bool *ended = calloc(blocks ? blocks : 1, sizeof(*ended));
    if (!layout->forward || !layout->next || !layout->skip || !candidate || !ended) {
        free(candidate);
        free(ended);
        anvil_mir_layout_destroy(layout);
        return false;
    }
    /* Neither the entry nor the frame block (it holds the prologue). */
    for (size_t b = 0; b < blocks; b++) {
        candidate[b] = b != 0 && b != anvil_mir_frame_block(mir);
        layout->forward[b] = (anvil_mir_block_t)b;
    }
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info) || info.block >= blocks) {
            free(candidate);
            free(ended);
            anvil_mir_layout_destroy(layout);
            return false;
        }
        size_t b = info.block;
        if (!candidate[b])
            continue;
        if (ended[b]) {
            candidate[b] = false;
            continue;
        }
        if (info.op == ANVIL_MIR_OP_BR) {
            ended[b] = true;
            layout->forward[b] = info.true_block;
            continue;
        }
        bool silent_copy = false;
        if (info.op == ANVIL_MIR_OP_COPY && info.num_uses == 1 && info.def != ANVIL_MIR_NO_VREG) {
            const anvil_regalloc_assignment_t *d = anvil_mir_get_assignment(mir, info.def);
            const anvil_regalloc_assignment_t *u = anvil_mir_get_assignment(mir, anvil_mir_get_instr_use(mir, i, 0));
            bool clobbers = false;
            for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
                clobbers |= info.clobbers[c] != 0 || info.scratch[c] != 0;
            silent_copy = d && u && !d->spilled && !u->spilled && d->phys_reg >= 0 && d->phys_reg == u->phys_reg && d->reg_class == u->reg_class &&
                          !clobbers;
        }
        if (!silent_copy)
            candidate[b] = false;
    }
    for (size_t b = 0; b < blocks; b++) {
        layout->skip[b] = candidate[b] && ended[b] && layout->forward[b] != (anvil_mir_block_t)b && layout->forward[b] < blocks;
        if (!layout->skip[b])
            layout->forward[b] = (anvil_mir_block_t)b;
    }
    /* Chains of them end at a printed block (a cycle of them keeps its
     * blocks printed). */
    for (size_t b = 0; b < blocks; b++) {
        if (!layout->skip[b])
            continue;
        anvil_mir_block_t at = layout->forward[b];
        size_t steps = 0;
        while (layout->skip[at] && steps++ < blocks)
            at = layout->forward[at];
        if (layout->skip[at]) {
            layout->skip[b] = false;
            layout->forward[b] = (anvil_mir_block_t)b;
        } else {
            layout->forward[b] = at;
        }
    }
    for (size_t b = 0; b < blocks; b++) {
        if (layout->skip[b] && layout->skip[layout->forward[b]]) {
            layout->skip[b] = false;
            layout->forward[b] = (anvil_mir_block_t)b;
        }
    }
    anvil_mir_block_t following = (anvil_mir_block_t)blocks;
    for (size_t b = blocks; b-- > 0;) {
        layout->next[b] = following;
        if (!layout->skip[b])
            following = (anvil_mir_block_t)b;
    }
    free(candidate);
    free(ended);
    return true;
}

void anvil_mir_layout_destroy(anvil_mir_layout_t *layout)
{
    free(layout->forward);
    free(layout->next);
    free(layout->skip);
    memset(layout, 0, sizeof(*layout));
}

anvil_mir_block_t anvil_mir_layout_target(const anvil_mir_layout_t *layout, anvil_mir_block_t block)
{
    return layout && layout->forward && block < layout->num_blocks ? layout->forward[block] : block;
}

bool anvil_mir_layout_falls_into(const anvil_mir_layout_t *layout, anvil_mir_block_t from, anvil_mir_block_t target)
{
    if (!layout || !layout->next || from >= layout->num_blocks)
        return target == from + 1;
    return anvil_mir_layout_target(layout, target) == layout->next[from];
}

bool anvil_mir_isel_frameless_prefix(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, anvil_mir_block_t frame_block)
{
    if (frame_block == 0 || frame_block >= anvil_mir_num_blocks(mir))
        return false;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return false;
        if (info.block >= frame_block)
            continue;
        for (size_t c = 0; c < ANVIL_MIR_REG_CLASS_COUNT; c++)
            if (info.clobbers[c] || info.scratch[c])
                return false;
        for (size_t u = 0; u < info.num_uses; u++) {
            const anvil_regalloc_assignment_t *a = anvil_mir_get_assignment(mir, anvil_mir_get_instr_use(mir, i, u));
            if (!a || a->spilled)
                return false;
        }
        const anvil_mir_vreg_info_t *def = info.def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(mir, info.def) : NULL;
        bool ok = info.op == ANVIL_MIR_OP_BR || info.op == ANVIL_MIR_OP_BR_COND || (info.op == ANVIL_MIR_OP_RET && info.num_uses == 0) ||
                  (anvil_mir_isel_is_compare(info.op) && def && def->registerless && anvil_mir_isel_feeds_next_branch(facts, i, &info)) ||
                  silent(facts, &info);
        if (!ok)
            return false;
    }
    return true;
}

bool anvil_mir_isel_condition_add(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t *extension, size_t *add,
                                  anvil_mir_vreg_t *other)
{
    anvil_mir_instr_info_t compare, widen, sum;
    if (!anvil_mir_get_instr_info(mir, index, &compare) || !anvil_mir_isel_is_compare(compare.op) || compare.op == ANVIL_MIR_OP_FCMP ||
        compare.op == ANVIL_MIR_OP_CMP || compare.def == ANVIL_MIR_NO_VREG || compare.def >= facts->num_vregs || facts->use_count[compare.def] != 1 ||
        !anvil_mir_get_instr_info(mir, index + 1, &widen) || widen.block != compare.block || widen.op != ANVIL_MIR_OP_ZEXT ||
        widen.num_uses != 1 || anvil_mir_get_instr_use(mir, index + 1, 0) != compare.def || widen.def == ANVIL_MIR_NO_VREG ||
        widen.def >= facts->num_vregs || facts->use_count[widen.def] != 1 || !anvil_mir_get_instr_info(mir, index + 2, &sum) ||
        sum.block != compare.block || sum.op != ANVIL_MIR_OP_ADD || sum.num_uses != 2 || sum.def == ANVIL_MIR_NO_VREG)
        return false;
    const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0));
    if (!lhs || lhs->reg_class != ANVIL_MIR_REG_GPR)
        return false;
    anvil_mir_vreg_t a = anvil_mir_get_instr_use(mir, index + 2, 0), b = anvil_mir_get_instr_use(mir, index + 2, 1);
    if ((a == widen.def) == (b == widen.def))
        return false;
    *other = a == widen.def ? b : a;
    const anvil_mir_vreg_info_t *o = anvil_mir_get_vreg_info(mir, *other), *d = anvil_mir_get_vreg_info(mir, sum.def);
    if (!o || !d || o->reg_class != ANVIL_MIR_REG_GPR || d->reg_class != ANVIL_MIR_REG_GPR)
        return false;
    *extension = index + 1;
    *add = index + 2;
    return true;
}
