/* MVS save-area linkage over ANVIL caller-owned storage; HLASM syntax. */

#include "../systemz_internal.h"

static void emit_prologue(systemz_emit_t *emit)
{
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", emit->func_label);
    anvil_strbuf_append(&emit->code, "         STMG  R14,R12,24(R13)\n");
    anvil_strbuf_append(&emit->code, "         LGR   R12,R15\n");
    anvil_strbuf_appendf(&emit->code, "         USING %s,R12\n", emit->func_label);
    anvil_strbuf_append(&emit->code, "         LGR   R11,R13\n");
    if (emit->frame_size <= 4095) {
        anvil_strbuf_appendf(&emit->code, "         LA    R2,%d(,R13)\n", emit->frame_size);
    } else {
        systemz_emit_load_imm(emit, 2, emit->frame_size);
        anvil_strbuf_append(&emit->code, "         AGR   R2,R13\n");
    }
    anvil_strbuf_append(&emit->code, "         STG   R11,8(,R2)\n");
    anvil_strbuf_append(&emit->code, "         STG   R2,16(,R11)\n");
    anvil_strbuf_append(&emit->code, "         LGR   R13,R2\n");
}

static void emit_epilogue(systemz_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "         LG    R13,8(,R13)\n");
    anvil_strbuf_append(&emit->code, "         LG    R14,24(,R13)\n");
    anvil_strbuf_append(&emit->code, "         LMG   R0,R12,40(R13)\n");
    anvil_strbuf_append(&emit->code, "         BR    R14\n");
}

const systemz_abi_ops_t systemz_mvs_arena_64_abi_ops = {
    .name = "ANVIL MVS arena 64", .abi = ANVIL_ABI_MVS, .emit_prologue = emit_prologue, .emit_epilogue = emit_epilogue
};
