/* MVS save-area linkage over ANVIL caller-owned storage; HLASM syntax. */

#include "../systemz_internal.h"

static void emit_prologue(systemz_emit_t *emit)
{
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", emit->func_label);
    anvil_strbuf_append(&emit->code, "         STM   R14,R12,12(R13)\n");
    systemz_emit_establish_base(emit);
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
    /* R0 carries the low word of a pair result. */
    anvil_strbuf_append(&emit->code, emit->ret_low_word != ANVIL_MIR_NO_VREG ? "         LM    R1,R12,24(R13)\n" : "         LM    R0,R12,20(R13)\n");
    anvil_strbuf_append(&emit->code, "         BR    R14\n");
}

const systemz_abi_ops_t systemz_mvs_arena_31_abi_ops = {
    .name = "ANVIL MVS arena 31", .abi = ANVIL_ABI_MVS, .sret_returns_address = true, .pair_low_gpr = 0, .emit_prologue = emit_prologue, .emit_epilogue = emit_epilogue
};
