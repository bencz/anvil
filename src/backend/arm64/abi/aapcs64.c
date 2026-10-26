/*
 * ANVIL - ARM64 procedure call standards.
 *
 * AAPCS64 (Linux and other ELF systems) and Apple's ARM64 variant share the
 * argument and aggregate rules below; Darwin differs in passing every
 * unnamed (variadic) argument in memory and in using a plain va_list.
 */

#include "../arm64_internal.h"
#include "../../common/abi_layout.h"

#include <string.h>

const anvil_agg_target_t arm64_agg_target = {8, false};

/* AAPCS64 B.3/C.3 classification of a struct, union or array. */
static bool classify_aggregate(anvil_type_t *type, bool is_return, arm64_agg_plan_t *plan)
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

/* Scalars: the next x or v register (C.6, C.9), else a doubleword stack
 * slot (C.10, C.15).  Darwin passes unnamed arguments in memory without
 * consuming registers. */
static arm64_arg_loc_t assign_scalar(const arm64_abi_desc_t *desc, arm64_arg_state_t *state, bool fp, bool unnamed)
{
    arm64_arg_loc_t loc = {-1, state->stack};
    size_t *count = fp ? &state->fpr : &state->gpr;
    unsigned limit = fp ? desc->num_fpr_args : desc->num_gpr_args;
    if (!(unnamed && desc->unnamed_args_on_stack) && *count < limit) {
        loc.reg = (int)(*count)++;
        return loc;
    }
    if (!(unnamed && desc->unnamed_args_on_stack))
        *count = limit;
    state->stack += 8;
    return loc;
}

/* Aggregates: all parts in consecutive registers (HFAs in v registers,
 * 16-byte aligned composites from an even x register), else the memory
 * image in doubleword slots, quadword aligned for 16-byte aligned types,
 * which also closes the register class (C.2-C.4, C.10-C.14). */
static arm64_arg_loc_t assign_aggregate(const arm64_abi_desc_t *desc, arm64_arg_state_t *state, const arm64_agg_plan_t *plan)
{
    arm64_arg_loc_t loc = {-1, 0};
    size_t *count = plan->fp ? &state->fpr : &state->gpr;
    unsigned limit = plan->fp ? desc->num_fpr_args : desc->num_gpr_args;
    if (!plan->fp && plan->align >= 16)
        *count = (*count + 1) & ~(size_t)1;
    if (*count + plan->num_parts <= limit) {
        loc.reg = (int)*count;
        *count += plan->num_parts;
        return loc;
    }
    *count = limit;
    if (plan->align >= 16)
        state->stack = (state->stack + 15) & ~INT64_C(15);
    loc.offset = state->stack;
    state->stack += (int64_t)((plan->size + 7) & ~(size_t)7);
    return loc;
}

static const arm64_abi_desc_t arm64_abi_descs[] = {
    {
        .abi = ANVIL_ABI_SYSV,
        .name = "aapcs64",
        .num_gpr_args = 8,
        .num_fpr_args = 8,
        .indirect_result_reg = ARM64_X8,
        .indirect_call_reg = ARM64_X16,
        .unnamed_args_on_stack = false,
        .has_register_va_list = true,
        .classify_aggregate = classify_aggregate,
        .assign_scalar = assign_scalar,
        .assign_aggregate = assign_aggregate,
    },
    {
        .abi = ANVIL_ABI_DARWIN,
        .name = "darwin",
        .num_gpr_args = 8,
        .num_fpr_args = 8,
        .indirect_result_reg = ARM64_X8,
        .indirect_call_reg = ARM64_X16,
        .unnamed_args_on_stack = true,
        .has_register_va_list = false,
        .classify_aggregate = classify_aggregate,
        .assign_scalar = assign_scalar,
        .assign_aggregate = assign_aggregate,
    },
};

const arm64_abi_desc_t *arm64_get_abi_desc(anvil_abi_t abi)
{
    for (size_t i = 0; i < sizeof(arm64_abi_descs) / sizeof(arm64_abi_descs[0]); i++)
        if (arm64_abi_descs[i].abi == abi)
            return &arm64_abi_descs[i];
    return NULL;
}
