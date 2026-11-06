/* IR-to-MachineIR lowering decisions shared by the backends: which MIR form
 * an IR vector operation, or a scalar one the target's operation table
 * prices, takes (the target's costs decide whether it has one), and which
 * casts leave a value unchanged.  Each backend still lowers operands and
 * records results its own way. */
#ifndef ANVIL_BACKEND_IR_LOWER_H
#define ANVIL_BACKEND_IR_LOWER_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"
#include "lower_map.h"

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

/* The state every backend's IR-to-MIR lowering shares, the first member of
 * its own lowering state (so a hook may cast it back): the function being
 * lowered, its MachineIR, what each IR value and block became
 * (lower_map.h), and the edge blocks made for PHI copies. */
typedef struct anvil_ir_lower anvil_ir_lower_t;
struct anvil_ir_lower {
    anvil_func_t *func;
    anvil_mir_func_t *mir;
    anvil_lower_map_t values;
    anvil_lower_map_t addresses;
    anvil_lower_map_t blocks;
    size_t num_edge_blocks;
    /* The registers holding `value` where the code being emitted reads it,
     * lowering it there when needed: one, or a word pair (high word first)
     * for a 64-bit integer on a 32-bit target. */
    bool (*value_regs)(anvil_ir_lower_t *lower, anvil_value_t *value, anvil_mir_vreg_t regs[2], size_t *count);
    /* `value` in one register (a branch condition, a switch selector or
     * case value), lowered where the code being emitted reads it. */
    anvil_mir_vreg_t (*value)(anvil_ir_lower_t *lower, anvil_value_t *value);
};

/* Control flow, the same on every target.  An edge into a block with PHIs
 * carries their values as one parallel copy: on an unconditional branch in
 * the branching block itself, on a conditional one or a switch case in a
 * block of its own made for the edge (anvil_ir_edge_target, which leaves
 * the current block as it was).  Switches from ANVIL_SWITCH_TREE_MIN_CASES
 * constant cases on a one-register selector take the shared compare tree,
 * smaller ones a chain of compares. */
anvil_mir_block_t anvil_ir_edge_target(anvil_ir_lower_t *lower, anvil_block_t *from, anvil_block_t *to);
/* A new MIR block named after `from` and `what`; the current block stays. */
anvil_mir_block_t anvil_ir_new_block(anvil_ir_lower_t *lower, const anvil_block_t *from, const char *what);
bool anvil_ir_lower_branch(anvil_ir_lower_t *lower, const anvil_instr_t *instr);
bool anvil_ir_lower_cond_branch(anvil_ir_lower_t *lower, const anvil_instr_t *instr);
bool anvil_ir_lower_switch(anvil_ir_lower_t *lower, const anvil_instr_t *instr);

/* Frees what anvil_ir_lower_t owns (the maps). */
void anvil_ir_lower_destroy(anvil_ir_lower_t *lower);

/* The MIR opcode of an IR operation: two operands (integer and FP
 * arithmetic, shifts, compares), one (negations, NOT, FABS), or a
 * conversion; false for any other operation. */
bool anvil_ir_binary_op(anvil_op_t op, anvil_mir_opcode_t *mir_op);
bool anvil_ir_unary_op(anvil_op_t op, anvil_mir_opcode_t *mir_op);
bool anvil_ir_cast_op(anvil_op_t op, anvil_mir_opcode_t *mir_op);
/* An integer or FP compare (anvil_mir_isel_is_compare, FCMP too). */
bool anvil_ir_compare_op(anvil_mir_opcode_t op);

/* A call's direct target: the function's or function global's symbol
 * (NULL: an indirect call), whether there is one, and the callee's
 * function type (through a function pointer too). */
const char *anvil_ir_call_symbol(const anvil_value_t *callee);
bool anvil_ir_call_is_direct(const anvil_value_t *callee);
anvil_type_t *anvil_ir_call_type(const anvil_value_t *callee);

/* The MIR block lowering IR `block` (the function's first block reuses the
 * MIR entry), cold like it. */
anvil_mir_block_t anvil_mir_block_for(anvil_mir_func_t *mir, const anvil_block_t *block, bool first);

/* A cast whose result is its operand's value in the same register: a
 * pointer reinterpreted as another pointer. */
bool anvil_cast_keeps_value(const anvil_instr_t *instr);

#endif
