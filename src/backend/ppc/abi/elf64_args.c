/*
 * Argument assignment shared by the 64-bit ELF ABIs (v1 and v2).
 *
 * Every argument is mapped onto the parameter save area: doubleword k
 * travels in r3+k for k < 8, floating arguments take FPRs but still consume
 * their doubleword, and the rest lives in memory at its save area offset.
 */

#include "../ppc_internal.h"

ppc_arg_loc_t ppc_elf64_assign_word(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state)
{
    ppc_arg_loc_t loc = {-1, state->stack};
    size_t slot = (size_t)(state->stack / 8);
    if (slot < desc->num_gpr_arg_regs)
        loc.reg = desc->gpr_arg_regs[slot];
    state->stack += 8;
    return loc;
}

ppc_arg_loc_t ppc_elf64_assign_fpr(const anvil_ppc_target_desc_t *desc, ppc_arg_state_t *state, bool single)
{
    ppc_arg_loc_t loc = {-1, state->stack};
    if (state->fpr < desc->num_fpr_arg_regs)
        loc.reg = desc->fpr_arg_regs[state->fpr++];
    else if (single && !desc->little_endian)
        loc.offset += 4;    /* the low-order word of its doubleword */
    state->stack += 8;
    return loc;
}

static void ppc_elf64_symbol_operand(anvil_strbuf_t *code, const char *symbol, int64_t addend, const char *suffix)
{
    anvil_strbuf_append(code, symbol);
    if (addend)
        anvil_strbuf_appendf(code, "%+lld", (long long)addend);
    anvil_strbuf_append(code, suffix);
}

void ppc_elf64_emit_symbol_address(ppc_mir_emit_t *emit, const char *dst, const char *symbol, int64_t addend, bool got)
{
    if (got) {
        anvil_strbuf_appendf(&emit->code, "\taddis %s, %%r2, %s@got@ha\n\tld %s, %s@got@l(%s)\n", dst, symbol, dst, symbol, dst);
        if (addend)
            ppc_emit_addi_large(emit, (int)strtol(dst + 2, NULL, 10), (int)strtol(dst + 2, NULL, 10), addend);
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\taddis %s, %%r2, ", dst);
    ppc_elf64_symbol_operand(&emit->code, symbol, addend, "@toc@ha\n");
    anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, ", dst, dst);
    ppc_elf64_symbol_operand(&emit->code, symbol, addend, "@toc@l\n");
}
