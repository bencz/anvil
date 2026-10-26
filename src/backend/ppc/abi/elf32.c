#include "../ppc_internal.h"

static void emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);

    anvil_strbuf_append(&emit->code, "\t.text\n");
    anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
}

static void emit_direct_call(ppc_mir_emit_t *emit, const char *symbol)
{
    anvil_strbuf_appendf(&emit->code, "\tbl %s\n", symbol);
}

static void emit_indirect_call(ppc_mir_emit_t *emit, const char *target_reg)
{
    anvil_strbuf_appendf(&emit->code, "\tmtctr %s\n", target_reg);
    anvil_strbuf_append(&emit->code, "\tbctrl\n");
}

/* SVR4 PowerPC ABI as clang and GCC implement it on Linux: aggregates are
 * passed by reference to a copy and returned through a hidden pointer. */
static bool classify_aggregate(anvil_type_t *type, bool is_return, ppc_agg_plan_t *plan)
{
    if (!ppc_agg_plan_init(type, plan))
        return false;
    plan->placement = is_return ? PPC_AGG_INDIRECT : PPC_AGG_REFERENCE;
    return true;
}

/* GPRs, FPRs and stack words are allocated independently. */
static ppc_arg_loc_t assign_word(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state)
{
    ppc_arg_loc_t loc = {-1, state->stack};
    if (state->gpr < desc->num_gpr_arg_regs) {
        loc.reg = desc->gpr_arg_regs[state->gpr++];
        return loc;
    }
    state->stack += 4;
    return loc;
}

static ppc_arg_loc_t assign_fpr(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single)
{
    ppc_arg_loc_t loc = {-1, 0};
    (void)single;
    if (state->fpr < desc->num_fpr_arg_regs) {
        loc.reg = desc->fpr_arg_regs[state->fpr++];
        return loc;
    }
    state->stack = (state->stack + 7) & ~INT64_C(7);
    loc.offset = state->stack;
    state->stack += 8;
    return loc;
}

/* 64-bit integers: r3:r4, r5:r6, r7:r8 or r9:r10, else an aligned stack
 * doubleword (and no further GPR use). */
static ppc_arg_loc_t assign_pair(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, int *lo_reg)
{
    ppc_arg_loc_t loc = {-1, 0};
    state->gpr += state->gpr & 1;
    if (state->gpr + 2 <= desc->num_gpr_arg_regs) {
        loc.reg = desc->gpr_arg_regs[state->gpr];
        *lo_reg = desc->gpr_arg_regs[state->gpr + 1];
        state->gpr += 2;
        return loc;
    }
    state->gpr = desc->num_gpr_arg_regs;
    state->stack = (state->stack + 7) & ~INT64_C(7);
    loc.offset = state->stack;
    state->stack += 8;
    return loc;
}

/* A variadic callee saves its FPR arguments only when CR6 says so. */
static void emit_variadic_call_setup(ppc_mir_emit_t *emit, bool fp_in_regs)
{
    anvil_strbuf_append(&emit->code, fp_in_regs ? "\tcreqv 6, 6, 6\n" : "\tcrxor 6, 6, 6\n");
}

const ppc_abi_ops_t ppc_elf32_abi_ops = {
    .name = "PowerPC ELF32",
    .abi = ANVIL_ABI_SYSV,
    .syntax = ANVIL_SYNTAX_GAS,
    .emit_function_header = emit_function_header,
    .emit_direct_call = emit_direct_call,
    .emit_indirect_call = emit_indirect_call,
    .classify_aggregate = classify_aggregate,
    .assign_word = assign_word,
    .assign_fpr = assign_fpr,
    .assign_pair = assign_pair,
    .unnamed_fp_in_gprs = false,
    .variadic_save_area = 0,
    .emit_variadic_call_setup = emit_variadic_call_setup
};
