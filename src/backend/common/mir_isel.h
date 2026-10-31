/* Facts that MachineIR printers use for instruction selection: which GPR
 * vregs hold a known constant (their only definition is a MOV of an
 * immediate, so every use sees that value), how often each vreg is used,
 * and which constants still need their MOV because some use cannot encode
 * them as an immediate.  The target decides what an immediate can be. */
#ifndef ANVIL_BACKEND_MIR_ISEL_H
#define ANVIL_BACKEND_MIR_ISEL_H

#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether operand `use` of instruction `index` will be encoded as the
 * immediate `value` by the target's printer. */
typedef bool (*anvil_mir_isel_takes_imm_fn)(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value);

typedef struct {
    const anvil_mir_func_t *mir;
    size_t num_vregs;
    uint8_t *def_count;  /* saturating; UINT8_MAX also marks non-MOV definitions */
    uint8_t *use_count;  /* saturating */
    int64_t *constant;
    bool *materialize;   /* false: no use needs the register */
    anvil_mir_opcode_t *def_op;  /* of the only definition (INVALID: several) */
} anvil_mir_isel_facts_t;

bool anvil_mir_isel_facts_build(anvil_mir_isel_facts_t *facts, const anvil_mir_func_t *mir, anvil_mir_isel_takes_imm_fn takes_imm, void *ctx);
void anvil_mir_isel_facts_destroy(anvil_mir_isel_facts_t *facts);

/* The constant a GPR vreg holds. */
bool anvil_mir_isel_constant(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out);

/* The opcode of `vreg`'s only definition (ANVIL_MIR_OP_INVALID when it has
 * several or none): a printer that knows what its form of that instruction
 * leaves in the upper bits (a narrow load extending, a narrow operation
 * zeroing) can skip an extension. */
anvil_mir_opcode_t anvil_mir_isel_def_op(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether the MOV defining `vreg` must still be emitted. */
bool anvil_mir_isel_materialize(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether the compare at `index` is consumed only by the BR_COND right after
 * it, so the printer can branch on the condition flags instead. */
bool anvil_mir_isel_feeds_next_branch(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info);
/* The same for the condition of the SELECT right after it (a conditional
 * move on the flags). */
bool anvil_mir_isel_feeds_next_select(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info);

bool anvil_mir_isel_is_compare(anvil_mir_opcode_t op);

#endif
