/* MVS save-area linkage over ANVIL caller-owned storage; HLASM syntax. */

#include "../systemz_internal.h"

static void emit_prologue(systemz_emit_t *emit)
{
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", emit->func_label);
    anvil_strbuf_append(&emit->code, "         STM   R14,R12,12(R13)\n");
    anvil_strbuf_append(&emit->code, "         LR    R12,R15\n");
    anvil_strbuf_appendf(&emit->code, "         USING %s,R12\n", emit->func_label);
    anvil_strbuf_append(&emit->code, "         LR    R11,R13\n");
    if (emit->frame_size <= 4095) {
        anvil_strbuf_appendf(&emit->code, "         LA    R2,%d(,R13)\n", emit->frame_size);
    } else {
        systemz_emit_load_imm(emit, 2, emit->frame_size);
        anvil_strbuf_append(&emit->code, "         AR    R2,R13\n");
    }
    anvil_strbuf_append(&emit->code, "         ST    R11,4(,R2)\n");
    anvil_strbuf_append(&emit->code, "         ST    R2,8(,R11)\n");
    anvil_strbuf_append(&emit->code, "         LR    R13,R2\n");
}

static void emit_epilogue(systemz_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "         L     R13,4(,R13)\n");
    anvil_strbuf_append(&emit->code, "         L     R14,12(,R13)\n");
    anvil_strbuf_append(&emit->code, "         LM    R0,R12,20(R13)\n");
    anvil_strbuf_append(&emit->code, "         BR    R14\n");
}

const systemz_abi_ops_t systemz_mvs_arena_31_abi_ops = {
    .name = "ANVIL MVS arena 31", .abi = ANVIL_ABI_MVS, .emit_prologue = emit_prologue, .emit_epilogue = emit_epilogue
};
