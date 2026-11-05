/* What the condition flags (condition register field, condition code) tell
 * while a printer emits one block: the zero-ness of the general register an
 * arithmetic instruction just wrote.  A compare of that register with zero
 * for == or != then needs no compare of its own.
 *
 * A printer records an instruction that sets the flags from its result
 * (anvil_print_flags_set), or one with a form that would (a record form:
 * anvil_print_flags_offer, which remembers where its mnemonic is and what to
 * spell instead).  After each MIR instruction, anvil_print_flags_after keeps
 * that knowledge only across instructions that printed nothing or only move
 * data without writing the register described. */
#ifndef ANVIL_PRINT_FLAGS_H
#define ANVIL_PRINT_FLAGS_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int phys;              /* -1: nothing known */
    uint16_t bits;
    bool set;              /* by the instruction being printed */
    /* An offer not yet taken: the mnemonic at `at` (length `length`) of
     * the code becomes `spelling`, which sets the flags. */
    size_t at, length;
    const char *spelling;
} anvil_print_flags_t;

void anvil_print_flags_reset(anvil_print_flags_t *flags);

/* The instruction being printed left the flags telling `phys`'s zero-ness
 * over `bytes`. */
void anvil_print_flags_set(anvil_print_flags_t *flags, int phys, int bytes);

/* It wrote `phys` with the mnemonic at code offset `at` of `length` chars;
 * `spelling` is its form that also sets the flags. */
void anvil_print_flags_offer(anvil_print_flags_t *flags, int phys, int bytes, size_t at, size_t length, const char *spelling);

/* After MIR instruction `index` (`printed`: whether it emitted anything).
 * Moves (register copies, spill stores and reloads) leave the flags on
 * every target; they end what the flags tell only when they write its
 * general register. */
void anvil_print_flags_after(anvil_print_flags_t *flags, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info,
                             bool printed);

/* Whether the flags tell `phys`'s zero-ness over `bytes`, taking an offer
 * (rewriting its mnemonic in `code`) when that is how. */
bool anvil_print_flags_take(anvil_print_flags_t *flags, anvil_strbuf_t *code, int phys, int bytes);

#endif
