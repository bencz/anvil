#include "../ppc_internal.h"

static void emit_function_header(ppc_mir_emit_t *emit)
{
    const char *name = anvil_mir_func_name(emit->mir);

    anvil_strbuf_append(&emit->code, "\t.abiversion 1\n");
    anvil_strbuf_append(&emit->code, "\t.section \".opd\",\"aw\"\n");
    anvil_strbuf_append(&emit->code, "\t.align 3\n");
    anvil_strbuf_appendf(&emit->code, "\t.globl %s\n", name);
    anvil_strbuf_appendf(&emit->code, "%s:\n", name);
    anvil_strbuf_appendf(&emit->code, "\t.quad .L.%s,.TOC.@tocbase,0\n", name);
    anvil_strbuf_append(&emit->code, "\t.previous\n");
    anvil_strbuf_appendf(&emit->code, "\t.type %s, @function\n", name);
    anvil_strbuf_appendf(&emit->code, ".L.%s:\n", name);
}

static void emit_direct_call(ppc_mir_emit_t *emit, const char *symbol)
{
    anvil_strbuf_appendf(&emit->code, "\tbl %s\n", symbol);

    if (emit->has_frame)
        anvil_strbuf_appendf(&emit->code, "\tld %%r2, %u(%%r31)\n", emit->desc->toc_save_offset);
}

static void emit_indirect_call(ppc_mir_emit_t *emit, const char *target_reg)
{
    anvil_strbuf_appendf(&emit->code, "\tld %%r11, 0(%s)\n", target_reg);
    anvil_strbuf_appendf(&emit->code, "\tld %%r2, 8(%s)\n", target_reg);
    anvil_strbuf_append(&emit->code, "\tmtctr %r11\n");
    anvil_strbuf_append(&emit->code, "\tbctrl\n");

    if (emit->has_frame)
        anvil_strbuf_appendf(&emit->code, "\tld %%r2, %u(%%r31)\n", emit->desc->toc_save_offset);
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
    .name = "PowerPC ELFV1",
    .abi = ANVIL_ABI_SYSV,
    .syntax = ANVIL_SYNTAX_GAS,
    .emit_function_header = emit_function_header,
    .emit_direct_call = emit_direct_call,
    .emit_indirect_call = emit_indirect_call,
    .classify_aggregate = classify_aggregate,
    .assign_word = ppc_elf64_assign_word,
    .assign_fpr = ppc_elf64_assign_fpr,
    .assign_pair = NULL,
    .unnamed_fp_in_gprs = true,
    .variadic_save_area = 64,
    .emit_variadic_call_setup = NULL
};
