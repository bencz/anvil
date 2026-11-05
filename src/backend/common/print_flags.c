#include "print_flags.h"

#include <string.h>

void anvil_print_flags_reset(anvil_print_flags_t *flags)
{
    flags->phys = -1;
    flags->set = false;
    flags->spelling = NULL;
}

void anvil_print_flags_set(anvil_print_flags_t *flags, int phys, int bytes)
{
    flags->phys = phys;
    flags->bits = (uint16_t)(bytes * 8);
    flags->set = true;
    flags->spelling = NULL;
}

void anvil_print_flags_offer(anvil_print_flags_t *flags, int phys, int bytes, size_t at, size_t length, const char *spelling)
{
    anvil_print_flags_set(flags, phys, bytes);
    flags->at = at;
    flags->length = length;
    flags->spelling = spelling;
}

/* The general register a move writes (-1: none, or memory). */
static int moved_into(const anvil_mir_func_t *mir, const anvil_mir_instr_info_t *info)
{
    if (info->op == ANVIL_MIR_OP_SPILL_STORE || info->def == ANVIL_MIR_NO_VREG)
        return -1;
    const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, info->def);
    const anvil_regalloc_assignment_t *to = anvil_mir_get_assignment(mir, info->def);
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR)
        return -1;
    return to && !to->spilled ? to->phys_reg : -2;
}

void anvil_print_flags_after(anvil_print_flags_t *flags, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info,
                             bool printed)
{
    (void)index;
    bool move = info->op == ANVIL_MIR_OP_COPY || info->op == ANVIL_MIR_OP_SPILL_LOAD || info->op == ANVIL_MIR_OP_SPILL_STORE;
    int written = move ? moved_into(mir, info) : -2;
    bool keeps = move && written != -2 && written != flags->phys;
    if (!flags->set && printed && !keeps)
        anvil_print_flags_reset(flags);
    flags->set = false;
}

bool anvil_print_flags_take(anvil_print_flags_t *flags, anvil_strbuf_t *code, int phys, int bytes)
{
    if (flags->phys < 0 || flags->phys != phys || flags->bits != bytes * 8)
        return false;
    if (!flags->spelling)
        return true;
    size_t grown = strlen(flags->spelling);
    if (flags->at + flags->length > code->len || code->failed)
        return false;
    /* Room at the end, then the tail moves over. */
    for (size_t pad = flags->length; pad < grown; pad++)
        anvil_strbuf_append_char(code, ' ');
    if (code->failed)
        return false;
    size_t tail = flags->at + flags->length;
    size_t old_len = code->len - (grown > flags->length ? grown - flags->length : 0);
    memmove(code->data + flags->at + grown, code->data + tail, old_len - tail);
    memcpy(code->data + flags->at, flags->spelling, grown);
    code->len = old_len - flags->length + grown;
    code->data[code->len] = '\0';
    flags->spelling = NULL;
    return true;
}
