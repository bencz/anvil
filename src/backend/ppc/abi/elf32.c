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

const ppc_abi_ops_t ppc_elf32_abi_ops = {
    .name = "PowerPC ELF32",
    .abi = ANVIL_ABI_SYSV,
    .syntax = ANVIL_SYNTAX_GAS,
    .emit_function_header = emit_function_header,
    .emit_direct_call = emit_direct_call,
    .emit_indirect_call = emit_indirect_call
};
