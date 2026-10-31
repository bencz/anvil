/* AT&T assembly the i386 (gas32.c) and x86-64 (gas64.c) GAS printers spell
 * alike: condition codes, floating compares on the ucomis/fucomip flags,
 * block labels and conditional branches. */
#ifndef ANVIL_X86_GAS_H
#define ANVIL_X86_GAS_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>

/* The setcc mnemonic of an integer compare; the jcc suffix is the text
 * after "set". */
const char *x86_gas_setcc(anvil_mir_opcode_t op);
const char *x86_gas_jcc(anvil_mir_opcode_t op);

/* The jcc suffix of the opposite condition (NULL: none). */
const char *x86_gas_inverse_jcc(const char *cc);

/* After `ucomis b, a` (or fucomip with ST(0) = a, ST(1) = b) unordered sets
 * ZF, PF and CF, a < b CF, a == b ZF.  The predicates one condition decides,
 * some on the swapped compare; OEQ and UNE need two. */
bool x86_gas_fcmp_branch(anvil_fcmp_pred_t pred, const char **cc, bool *swap);

/* dst8 = pred on those flags (not swapped), through local branches named
 * after instruction `index`. */
void x86_gas_emit_fcmp_bool(anvil_strbuf_t *code, const char *label_prefix, const char *function, size_t index,
                            anvil_fcmp_pred_t pred, const char *dst8);

/* "<prefix><function>_<block name>", as a definition or a reference. */
bool x86_gas_emit_label(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block);
bool x86_gas_emit_branch_target(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t block);

/* Jumps to true_block on `cc` and to false_block otherwise, from code in
 * block `current`: a target that is the next block falls through. */
bool x86_gas_emit_cond_branch(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t current,
                              const char *cc, anvil_mir_block_t true_block, anvil_mir_block_t false_block);

/* An unconditional jump, omitted to the next block. */
bool x86_gas_emit_jump(anvil_strbuf_t *code, const char *label_prefix, const anvil_mir_func_t *mir, anvil_mir_block_t current,
                       anvil_mir_block_t target);

#endif
