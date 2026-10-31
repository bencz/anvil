/*
 * ANVIL - ARM64 procedure call rules shared by every ABI.
 *
 * The AAPCS64 composite classification, register allocation and stack
 * placement of aggregates hold for Linux (abi/linux.c) and Darwin
 * (abi/darwin.c) alike; each ABI module builds its descriptor on them.
 */

#include "../arm64_internal.h"
#include "../../common/abi_layout.h"

#include <string.h>

const anvil_agg_target_t arm64_agg_target = {8, false};

/* AAPCS64 B.3/C.3 classification of a struct, union or array. */
bool arm64_aapcs64_classify_aggregate(anvil_type_t *type, bool is_return, arm64_agg_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (!type || (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY) || !type->size)
        return false;
    plan->size = type->size;
    plan->align = type->align ? type->align : 1;
    plan->placement = ARM64_AGG_REGS;

    /* Homogeneous floating aggregates of up to four members travel one
     * member per v register. */
    size_t count = 0;
    anvil_type_t *base = anvil_abi_homogeneous_float(type, &count);
    if (base && count <= 4) {
        plan->fp = true;
        plan->num_parts = (unsigned)count;
        for (unsigned i = 0; i < count; i++)
            plan->parts[i] = (anvil_agg_part_t){true, (unsigned)base->size, (unsigned)(i * base->size), 0};
        return true;
    }

    /* Larger composites go through memory; the rest is its memory image in
     * one or two doublewords. */
    if (type->size > 16) {
        plan->placement = is_return ? ARM64_AGG_INDIRECT : ARM64_AGG_REFERENCE;
        return true;
    }
    plan->num_parts = type->size > 8 ? 2 : 1;
    for (unsigned i = 0; i < plan->num_parts; i++)
        plan->parts[i] = anvil_agg_chunk(type->size, 8 * i, arm64_agg_target);
    return true;
}

int arm64_aapcs64_take_regs(size_t *next, unsigned limit, unsigned count)
{
    if (*next + count <= limit) {
        int first = (int)*next;
        *next += count;
        return first;
    }
    *next = limit;
    return -1;
}

int64_t arm64_aapcs64_aggregate_stack(arm64_arg_state_t *state, const arm64_agg_plan_t *plan)
{
    if (plan->align >= 16)
        state->stack = (state->stack + 15) & ~INT64_C(15);
    else
        state->stack = (state->stack + 7) & ~INT64_C(7);
    int64_t offset = state->stack;
    state->stack += (int64_t)((plan->size + 7) & ~(size_t)7);
    return offset;
}

const arm64_abi_desc_t *arm64_get_abi_desc(anvil_abi_t abi)
{
    static const arm64_abi_desc_t *const descs[] = { &arm64_linux_abi, &arm64_darwin_abi };
    for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); i++)
        if (descs[i]->abi == abi)
            return descs[i];
    return NULL;
}

static const int aapcs64_gprs[] = {8, 7, 6, 5, 4, 3, 2, 1, 0, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
static const int aapcs64_fprs[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15};
static const int aapcs64_scratch_gprs[] = {12, 13, 14, 15};
static const int aapcs64_scratch_fprs[] = {16, 17, 18, 19};

const arm64_register_convention_t arm64_aapcs64_registers = {
    .gprs = aapcs64_gprs,
    .num_gprs = (int)(sizeof(aapcs64_gprs) / sizeof(aapcs64_gprs[0])),
    .fprs = aapcs64_fprs,
    .num_fprs = (int)(sizeof(aapcs64_fprs) / sizeof(aapcs64_fprs[0])),
    .scratch_gprs = aapcs64_scratch_gprs,
    .num_scratch_gprs = (int)(sizeof(aapcs64_scratch_gprs) / sizeof(aapcs64_scratch_gprs[0])),
    .scratch_fprs = aapcs64_scratch_fprs,
    .num_scratch_fprs = (int)(sizeof(aapcs64_scratch_fprs) / sizeof(aapcs64_scratch_fprs[0])),
    /* x0-x18; v0-v7 and v16-v31 (v8-v15 keep their low 64 bits). */
    .call_gpr_clobbers = (UINT64_C(1) << 19) - 1,
    .call_fpr_clobbers = UINT64_C(0xFFFF00FF),
    .fpr_partially_preserved = UINT64_C(0xFF00),
    .fpr_preserved_bits = 64,
    .vector_temps = {31, 30},
};
