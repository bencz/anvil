#include "../ppc_internal.h"
#include "../../common/obj_format.h"

static void emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);

    anvil_strbuf_append(&emit->code, "\t.abiversion 1\n");
    anvil_strbuf_append(&emit->code, "\t.section \".opd\",\"aw\"\n");
    anvil_strbuf_append(&emit->code, "\t.align 3\n");
    anvil_obj_emit_binding(&emit->code, &anvil_obj_format_elf, anvil_mir_func_linkage(emit->mir), name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
    anvil_strbuf_appendf(&emit->code, "\t.quad .L.%s,.TOC.@tocbase,0\n", name);
    /* Explicitly: the section before the descriptor may be a previous
     * function's string pool. */
    anvil_strbuf_append(&emit->code, "\t.text\n");
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, ".L.%s:\n", name);
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

/* Through the callee's descriptor: entry point, then its TOC; ours is kept
 * in this frame's TOC save slot across the call. */
static void emit_indirect_call(ppc_mir_emit_t *emit, const char *target_reg)
{
    unsigned toc = emit->desc->toc_save_offset;
    anvil_strbuf_appendf(&emit->code, "\tstd %%r2, %u(%%r1)\n", toc);
    anvil_strbuf_appendf(&emit->code, "\tld %%r0, 0(%s)\n", target_reg);
    anvil_strbuf_appendf(&emit->code, "\tld %%r2, 8(%s)\n", target_reg);
    anvil_strbuf_append(&emit->code, "\tmtctr %r0\n");
    anvil_strbuf_append(&emit->code, "\tbctrl\n");
    anvil_strbuf_appendf(&emit->code, "\tld %%r2, %u(%%r1)\n", toc);
}

/* 64-bit ELF ABI v1: aggregate arguments are their memory image in the
 * parameter save area (and r3-r10); results go through a hidden pointer. */
static bool classify_aggregate(anvil_type_t *type, bool is_return, ppc_agg_plan_t *plan)
{
    if (!ppc_agg_plan_init(type, plan))
        return false;
    plan->placement = is_return ? PPC_AGG_INDIRECT : PPC_AGG_IMAGE;
    return true;
}

const ppc_abi_ops_t ppc_elfv1_abi_ops = {
    .call_gpr_clobbers = UINT64_C(1) | (UINT64_C(0x3ff) << 3),
    .call_fpr_clobbers = (UINT64_C(1) << 14) - 1,
    .call_vector_clobbers = (UINT64_C(1) << 20) - 1,
    .add_va_start = ppc_elf64_add_va_start,
    .emit_va_register_save = ppc_elf64_emit_va_register_save,
    .emit_va_start = ppc_elf64_emit_va_start,
    .build_va_arg = ppc_elf64_build_va_arg,
    .build_va_copy = ppc_cursor_va_copy,
    .build_va_copy_into = ppc_cursor_va_copy_into,
    .name = "PowerPC ELFV1",
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
