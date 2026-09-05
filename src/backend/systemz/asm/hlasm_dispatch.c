#include "../systemz_internal.h"

static void emit_module_end(anvil_strbuf_t *out)
{
    anvil_strbuf_append(out, "         END\n");
}

const systemz_asm_ops_t systemz_hlasm_ops = {
    .name = "HLASM", .syntax = ANVIL_SYNTAX_HLASM, .emit_mir = systemz_emit_mir_ex, .emit_globals = systemz_emit_globals, .emit_module_end = emit_module_end
};
