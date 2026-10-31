#include "../ppc_internal.h"
#include "../../common/obj_format.h"
#include "../../common/abi_layout.h"

static void emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);

    anvil_strbuf_append(&emit->code, "\t.abiversion 2\n");
    anvil_strbuf_append(&emit->code, "\t.text\n");
    anvil_obj_emit_binding(&emit->code, &anvil_obj_format_elf, anvil_mir_func_linkage(emit->mir), name);
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
    anvil_strbuf_append(&emit->code, "0:\taddis %r2, %r12, (.TOC.-0b)@ha\n");
    anvil_strbuf_append(&emit->code, "\taddi %r2, %r2, (.TOC.-0b)@l\n");
    anvil_strbuf_appendf(&emit->code, "\t.localentry %s, .-0b\n", name);
}

/* The linker turns the nop into the TOC reload when the call goes through
 * a stub (another module). */
/* The linker routes calls to other modules through a stub that saves and
 * restores the TOC (the nop's slot), position-independent or not. */
static void emit_direct_call(ppc_mir_emit_t *emit, const char *symbol, bool via_got)
{
    (void)via_got;
    anvil_strbuf_appendf(&emit->code, "\tbl %s\n\tnop\n", symbol);
}

/* The callee's global entry derives its TOC from r12; ours is kept in this
 * frame's TOC save slot across the call. */
static void emit_indirect_call(ppc_mir_emit_t *emit, const char *target_reg)
{
    unsigned toc = emit->desc->toc_save_offset;
    anvil_strbuf_appendf(&emit->code, "\tstd %%r2, %u(%%r1)\n", toc);
    if (strcmp(target_reg, "%r12") != 0)
        anvil_strbuf_appendf(&emit->code, "\tmr %%r12, %s\n", target_reg);
    anvil_strbuf_append(&emit->code, "\tmtctr %r12\n");
    anvil_strbuf_append(&emit->code, "\tbctrl\n");
    anvil_strbuf_appendf(&emit->code, "\tld %%r2, %u(%%r1)\n", toc);
}

/* 64-bit ELF ABI v2: homogeneous floating aggregates of up to eight members
 * travel in FPRs; other arguments are their memory image in the parameter
 * save area (and r3-r10); other results of up to 16 bytes come back in
 * r3/r4, larger ones through a hidden pointer. */
static bool classify_aggregate(anvil_type_t *type, bool is_return, ppc_agg_plan_t *plan)
{
    if (!ppc_agg_plan_init(type, plan))
        return false;

    size_t count = 0;
    anvil_type_t *base = anvil_abi_homogeneous_float(type, &count);
    if (base && count <= 8) {
        plan->placement = PPC_AGG_REGS;
        plan->fp = true;
        plan->num_parts = (unsigned)count;
        for (unsigned i = 0; i < count; i++)
            plan->parts[i] = (anvil_agg_part_t){true, (unsigned)base->size, (unsigned)(i * base->size), 0};
        return true;
    }
    if (!is_return) {
        plan->placement = PPC_AGG_IMAGE;
        return true;
    }
    if (type->size > 16) {
        plan->placement = PPC_AGG_INDIRECT;
        return true;
    }
    static const anvil_agg_target_t ppc64le = {8, false};
    plan->placement = PPC_AGG_REGS;
    plan->num_parts = type->size > 8 ? 2 : 1;
    for (unsigned i = 0; i < plan->num_parts; i++)
        plan->parts[i] = anvil_agg_chunk(type->size, 8 * i, ppc64le);
    return true;
}

const ppc_abi_ops_t ppc_elfv2_abi_ops = {
    .call_gpr_clobbers = UINT64_C(1) | (UINT64_C(0x3ff) << 3),
    .call_fpr_clobbers = (UINT64_C(1) << 14) - 1,
    .call_vector_clobbers = (UINT64_C(1) << 20) - 1,
    .add_va_start = ppc_elf64_add_va_start,
    .emit_va_register_save = ppc_elf64_emit_va_register_save,
    .emit_va_start = ppc_elf64_emit_va_start,
    .build_va_arg = ppc_elf64_build_va_arg,
    .build_va_copy = ppc_cursor_va_copy,
    .build_va_copy_into = ppc_cursor_va_copy_into,
    .name = "PowerPC ELFV2",
    .result_extend_bits = 64,
    .abi = ANVIL_ABI_SYSV,
    .syntax = ANVIL_SYNTAX_GAS,
    .emit_function_header = emit_function_header,
    .emit_direct_call = emit_direct_call,
    .emit_symbol_address = ppc_elf64_emit_symbol_address,
    .emit_indirect_call = emit_indirect_call,
    .classify_aggregate = classify_aggregate,
    .assign_word = ppc_elf64_assign_word,
    .assign_fpr = ppc_elf64_assign_fpr,
    .assign_pair = NULL,
    .unnamed_fp_in_gprs = true,
    .variadic_save_area = 64,
    .emit_variadic_call_setup = NULL
};
