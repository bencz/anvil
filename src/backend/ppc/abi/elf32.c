#include "../ppc_internal.h"
#include "../../common/obj_format.h"

static void emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);

    anvil_strbuf_append(&emit->code, "\t.text\n");
    anvil_obj_emit_binding(&emit->code, &anvil_obj_format_elf, anvil_mir_func_linkage(emit->mir), name);
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
}

/* reg = this instruction's address (bcl to the next one; LR is saved in
 * the frame of every position-independent function), then reg += delta. */
static void elf32_pc_relative(ppc_mir_emit_t *emit, const char *reg, const char *target)
{
    const char *function = anvil_mir_func_name(emit->mir);
    size_t label = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tbcl 20, 31, .L%s_pic%zu\n.L%s_pic%zu:\n\tmflr %s\n", function, label, function, label, reg);
    anvil_strbuf_appendf(&emit->code, "\taddis %s, %s, (%s-.L%s_pic%zu)@ha\n\taddi %s, %s, (%s-.L%s_pic%zu)@l\n", reg, reg, target,
                         function, label, reg, reg, target, function, label);
}

/* Position-independent calls to other modules load the target from the
 * GOT (no dependence on the secure-PLT r30 convention). */
static void emit_direct_call(ppc_mir_emit_t *emit, const char *symbol, bool via_got)
{
    if (!via_got || emit->reloc == ANVIL_RELOC_STATIC) {
        anvil_strbuf_appendf(&emit->code, "\tbl %s\n", symbol);
        return;
    }
    elf32_pc_relative(emit, "%r12", "_GLOBAL_OFFSET_TABLE_");
    anvil_strbuf_appendf(&emit->code, "\tlwz %%r12, %s@got(%%r12)\n\tmtctr %%r12\n\tbctrl\n", symbol);
}

/* Static code uses absolute addresses; position-independent code reaches
 * its own symbols PC-relatively and the others through the GOT. */
static void emit_symbol_address(ppc_mir_emit_t *emit, const char *dst, const char *symbol, int64_t addend, bool got)
{
    char target[512];
    if (addend)
        snprintf(target, sizeof(target), "%s%+lld", symbol, (long long)addend);
    else
        snprintf(target, sizeof(target), "%s", symbol);
    if (emit->reloc == ANVIL_RELOC_STATIC) {
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %s@ha\n\taddi %s, %s, %s@l\n", dst, target, dst, dst, target);
    } else if (!got) {
        elf32_pc_relative(emit, dst, target);
    } else {
        elf32_pc_relative(emit, dst, "_GLOBAL_OFFSET_TABLE_");
        anvil_strbuf_appendf(&emit->code, "\tlwz %s, %s@got(%s)\n", dst, symbol, dst);
        if (addend)
            anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %lld\n", dst, dst, (long long)addend);
    }
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

/* Past f8, a float takes a 4-byte stack word in single format and a double
 * an 8-aligned doubleword (as GCC and clang lay them out). */
static ppc_arg_loc_t assign_fpr(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single)
{
    ppc_arg_loc_t loc = {-1, 0};
    if (state->fpr < desc->num_fpr_arg_regs) {
        loc.reg = desc->fpr_arg_regs[state->fpr++];
        return loc;
    }
    int64_t size = single ? 4 : 8;
    state->stack = (state->stack + size - 1) & ~(size - 1);
    loc.offset = state->stack;
    state->stack += size;
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
    .call_gpr_clobbers = UINT64_C(1) | (UINT64_C(0x3ff) << 3),
    .call_fpr_clobbers = (UINT64_C(1) << 14) - 1,
    .call_vector_clobbers = (UINT64_C(1) << 20) - 1,
    .add_va_start = ppc_elf32_add_va_start,
    .emit_va_register_save = ppc_elf32_emit_va_register_save,
    .emit_va_start = ppc_elf32_emit_va_start,
    .build_va_arg = ppc_elf32_build_va_arg,
    .build_va_copy = ppc_elf32_build_va_copy,
    .build_va_copy_into = ppc_elf32_build_va_copy_into,
    .name = "PowerPC ELF32",
    .result_extend_bits = 32,
    .abi = ANVIL_ABI_SYSV,
    .syntax = ANVIL_SYNTAX_GAS,
    .emit_function_header = emit_function_header,
    .emit_direct_call = emit_direct_call,
    .emit_symbol_address = emit_symbol_address,
    .emit_indirect_call = emit_indirect_call,
    .classify_aggregate = classify_aggregate,
    .assign_word = assign_word,
    .assign_fpr = assign_fpr,
    .assign_pair = assign_pair,
    .unnamed_fp_in_gprs = false,
    .variadic_save_area = 0,
    .emit_variadic_call_setup = emit_variadic_call_setup
};
