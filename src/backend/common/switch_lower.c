#include "switch_lower.h"

#include <stdlib.h>

/* A run of consecutive case values with one destination. */
typedef struct {
    int64_t low, high;
    size_t target;      /* destination identity */
    size_t first;       /* the first case of the run (for case_target) */
} switch_run_t;

typedef struct {
    const anvil_switch_lowering_t *lowering;
    const int64_t *values;
    bool ok;
} switch_sort_t;

static uint64_t width_mask(unsigned bits)
{
    return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
}

/* Case values as the selector's order sees them. */
static bool before(const anvil_switch_lowering_t *lowering, int64_t a, int64_t b)
{
    if (lowering->is_signed)
        return a < b;
    uint64_t mask = width_mask(lowering->bits);
    return ((uint64_t)a & mask) < ((uint64_t)b & mask);
}

static anvil_mir_vreg_t constant(const anvil_switch_lowering_t *lowering, int64_t value)
{
    anvil_mir_vreg_t vreg = anvil_mir_add_vreg_typed(lowering->mir, ANVIL_MIR_REG_GPR, (uint16_t)lowering->bits, lowering->is_signed);
    return vreg != ANVIL_MIR_NO_VREG && anvil_mir_add_instr_imm(lowering->mir, ANVIL_MIR_OP_MOV, vreg, value) ? vreg : ANVIL_MIR_NO_VREG;
}

/* cond = the selector within run (one value: equality; several: one
 * unsigned compare of selector - low against high - low). */
static anvil_mir_vreg_t run_test(const anvil_switch_lowering_t *lowering, const switch_run_t *run)
{
    anvil_mir_vreg_t cond = lowering->new_bool(lowering->user);
    if (cond == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;
    if (run->low == run->high) {
        anvil_mir_vreg_t value = constant(lowering, run->low);
        anvil_mir_vreg_t uses[] = {lowering->selector, value};
        return value != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(lowering->mir, ANVIL_MIR_OP_CMP_EQ, cond, uses, 2) ? cond : ANVIL_MIR_NO_VREG;
    }
    anvil_mir_vreg_t low = constant(lowering, run->low);
    anvil_mir_vreg_t span = constant(lowering, (int64_t)(((uint64_t)run->high - (uint64_t)run->low) & width_mask(lowering->bits)));
    anvil_mir_vreg_t offset = anvil_mir_add_vreg_typed(lowering->mir, ANVIL_MIR_REG_GPR, (uint16_t)lowering->bits, false);
    if (low == ANVIL_MIR_NO_VREG || span == ANVIL_MIR_NO_VREG || offset == ANVIL_MIR_NO_VREG)
        return ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t sub_uses[] = {lowering->selector, low}, cmp_uses[] = {offset, span};
    return anvil_mir_add_instr(lowering->mir, ANVIL_MIR_OP_SUB, offset, sub_uses, 2) &&
                   anvil_mir_add_instr(lowering->mir, ANVIL_MIR_OP_CMP_ULE, cond, cmp_uses, 2)
               ? cond
               : ANVIL_MIR_NO_VREG;
}

/* Runs [first, last] from the current block: a few tested one after the
 * other, more split at the middle run's low value. */
static bool emit_runs(const anvil_switch_lowering_t *lowering, const switch_run_t *runs, size_t first, size_t last)
{
    enum { SWITCH_LINEAR_RUNS = 3 };
    if (last - first + 1 <= SWITCH_LINEAR_RUNS) {
        for (size_t at = first; at <= last; at++) {
            anvil_mir_block_t target = lowering->case_target(lowering->user, runs[at].first);
            anvil_mir_block_t next = at < last ? lowering->new_block(lowering->user) : lowering->default_target;
            anvil_mir_vreg_t cond = target != ANVIL_MIR_NO_BLOCK && next != ANVIL_MIR_NO_BLOCK ? run_test(lowering, &runs[at]) : ANVIL_MIR_NO_VREG;
            if (cond == ANVIL_MIR_NO_VREG || !anvil_mir_add_cond_branch(lowering->mir, cond, target, next))
                return false;
            if (at < last && !anvil_mir_set_current_block(lowering->mir, next))
                return false;
        }
        return true;
    }
    size_t middle = first + (last - first + 1) / 2;
    anvil_mir_vreg_t cond = lowering->new_bool(lowering->user);
    anvil_mir_vreg_t pivot = cond != ANVIL_MIR_NO_VREG ? constant(lowering, runs[middle].low) : ANVIL_MIR_NO_VREG;
    anvil_mir_block_t below = pivot != ANVIL_MIR_NO_VREG ? lowering->new_block(lowering->user) : ANVIL_MIR_NO_BLOCK;
    anvil_mir_block_t above = below != ANVIL_MIR_NO_BLOCK ? lowering->new_block(lowering->user) : ANVIL_MIR_NO_BLOCK;
    anvil_mir_vreg_t uses[] = {lowering->selector, pivot};
    if (above == ANVIL_MIR_NO_BLOCK ||
        !anvil_mir_add_instr(lowering->mir, lowering->is_signed ? ANVIL_MIR_OP_CMP_LT : ANVIL_MIR_OP_CMP_ULT, cond, uses, 2) ||
        !anvil_mir_add_cond_branch(lowering->mir, cond, below, above))
        return false;
    return anvil_mir_set_current_block(lowering->mir, below) && emit_runs(lowering, runs, first, middle - 1) &&
           anvil_mir_set_current_block(lowering->mir, above) && emit_runs(lowering, runs, middle, last);
}

bool anvil_mir_lower_switch_tree(const anvil_switch_lowering_t *lowering, const int64_t *values, const size_t *targets, size_t count)
{
    if (!lowering || !values || !targets || !count || !lowering->new_block || !lowering->case_target || !lowering->new_bool)
        return false;
    size_t *order = malloc(count * sizeof(*order));
    switch_run_t *runs = malloc(count * sizeof(*runs));
    if (!order || !runs) {
        free(order);
        free(runs);
        return false;
    }
    /* Insertion sort by value (switches are small). */
    for (size_t i = 0; i < count; i++) {
        size_t at = i;
        while (at > 0 && before(lowering, values[i], values[order[at - 1]])) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    size_t num_runs = 0;
    uint64_t mask = width_mask(lowering->bits);
    for (size_t k = 0; k < count; k++) {
        size_t c = order[k];
        /* A repeated value: the earlier case (first in the stable order)
         * decides it, as a chain of compares would. */
        if (k > 0 && ((uint64_t)values[c] & mask) == ((uint64_t)values[order[k - 1]] & mask))
            continue;
        if (num_runs && runs[num_runs - 1].target == targets[c] &&
            (((uint64_t)runs[num_runs - 1].high + 1) & mask) == ((uint64_t)values[c] & mask) &&
            before(lowering, runs[num_runs - 1].high, values[c])) {
            runs[num_runs - 1].high = values[c];
            continue;
        }
        runs[num_runs++] = (switch_run_t){.low = values[c], .high = values[c], .target = targets[c], .first = c};
    }
    bool ok = emit_runs(lowering, runs, 0, num_runs - 1);
    free(order);
    free(runs);
    return ok;
}
