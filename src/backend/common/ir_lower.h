/* IR-to-MachineIR lowering decisions shared by the backends: which MIR form
 * an IR vector operation, or a scalar one the target's operation table
 * prices, takes (the target's costs decide whether it has one), and which
 * casts leave a value unchanged.  Each backend still lowers operands and
 * records results its own way. */
#ifndef ANVIL_BACKEND_IR_LOWER_H
#define ANVIL_BACKEND_IR_LOWER_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stdint.h>

/* The most operands a lane operation has (select: mask, then, else). */
enum { ANVIL_VECTOR_MAX_USES = 3 };

typedef struct {
    anvil_op_t operation;   /* the MIR VECTOR or SCALAR instruction's ir_op */
    anvil_fcmp_pred_t predicate;  /* FCMP */
    unsigned lane_bits;     /* its imm: the lane width, or a scalar's width */
    size_t num_uses;
    bool supported;         /* the target's vector costs accept it */
    bool scalar;            /* MIR SCALAR: the operation on single values */
    unsigned widening;      /* ACCUM_*: lanes of the last operand per result lane */
} anvil_vector_mir_form_t;

/* False when `instr` is neither a lane-wise vector operation (vector loads
 * and stores lower as plain 128-bit accesses) nor a scalar one the target
 * prices in the scalar shape (FMA, FSQRT); otherwise its MIR form, with
 * `supported` false when the target cannot run the vector form.  A shift by
 * a scalar amount is the UNIFORM shape: every lane shifts by that one
 * count. */
bool anvil_vector_mir_form(anvil_ctx_t *ctx, const anvil_instr_t *instr, anvil_vector_mir_form_t *form);

/* Appends the form's instruction defining `def` from the lowered operands
 * `uses` (form->num_uses of them); false when the form is unsupported or a
 * vreg is missing. */
bool anvil_vector_mir_emit(anvil_mir_func_t *mir, const anvil_vector_mir_form_t *form, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses);

/* The MIR block lowering IR `block` (the function's first block reuses the
 * MIR entry), cold like it. */
anvil_mir_block_t anvil_mir_block_for(anvil_mir_func_t *mir, const anvil_block_t *block, bool first);

/* A cast whose result is its operand's value in the same register: a
 * pointer reinterpreted as another pointer. */
bool anvil_cast_keeps_value(const anvil_instr_t *instr);

#endif
