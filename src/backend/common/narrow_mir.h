/* Integers narrower than the smallest operation width a target has (32 bits
 * on ARM64, PowerPC and SystemZ) live in full registers whose high bits are
 * undefined.  Operations whose result depends on those bits (division,
 * remainder, right shifts, compares, integer->FP conversion) must see
 * extended operands; the others (add, sub, mul, logic, left shift) are
 * correct in the low bits regardless.  Lowerings route such operations
 * through these helpers. */
#ifndef ANVIL_BACKEND_NARROW_MIR_H
#define ANVIL_BACKEND_NARROW_MIR_H

#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stdint.h>

/* `src` extended to `bits` (sign or zero), or `src` itself when it is at
 * least that wide or not a GPR. */
anvil_mir_vreg_t anvil_mir_extend_gpr(anvil_mir_func_t *mir, anvil_mir_vreg_t src, uint16_t bits, bool sign);

/* Emits `def = lhs OP rhs`.  When the operation reads high bits and an
 * operand is narrower than `min_bits`, the operands are extended (signed or
 * unsigned per the opcode) and a non-compare result is computed at
 * `min_bits` and truncated into `def`.  `imm`/`has_imm` carry an FCMP
 * predicate or similar payload. */
bool anvil_mir_add_binary_extending(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t def, anvil_mir_vreg_t lhs,
                                    anvil_mir_vreg_t rhs, uint16_t min_bits);

/* The source of SITOFP/UITOFP extended to at least `min_bits`. */
anvil_mir_vreg_t anvil_mir_int_to_fp_source(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_mir_vreg_t src, uint16_t min_bits);

#endif
