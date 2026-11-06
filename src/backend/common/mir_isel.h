/* Facts that MachineIR printers use for instruction selection: which GPR
 * vregs hold a known constant (their only definition is a MOV of an
 * immediate, so every use sees that value) or a known symbol address (a
 * SYMBOL_ADDR not through the GOT), how often each vreg is used, and which
 * of them still need their definition because some use cannot encode the
 * value in place.  The target decides what it can encode. */
#ifndef ANVIL_BACKEND_MIR_ISEL_H
#define ANVIL_BACKEND_MIR_ISEL_H

#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether operand `use` of instruction `index` will be encoded as the
 * immediate `value` by the target's printer. */
typedef bool (*anvil_mir_isel_takes_imm_fn)(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value);
/* The same for the address `symbol` + `offset` (absolute addressing). */
typedef bool (*anvil_mir_isel_takes_symbol_fn)(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, const char *symbol,
                                               int64_t offset);

typedef struct {
    const anvil_mir_func_t *mir;
    size_t num_vregs;
    uint8_t *def_count;  /* saturating; UINT8_MAX also marks non-MOV definitions */
    uint8_t *use_count;  /* saturating */
    int64_t *constant;   /* the immediate, or the offset from `symbol` */
    const char **symbol; /* a known symbol address, else NULL */
    bool *materialize;   /* false: no use needs the register */
    anvil_mir_opcode_t *def_op;  /* of the only definition (INVALID: several) */
    uint8_t *width;      /* low bits that can be nonzero in the register (0: unknown) */
    bool *low_bits;      /* every reader needs only the value's own bits */
} anvil_mir_isel_facts_t;

/* Either hook may be NULL: nothing is encoded in place then. */
bool anvil_mir_isel_facts_build(anvil_mir_isel_facts_t *facts, const anvil_mir_func_t *mir, anvil_mir_isel_takes_imm_fn takes_imm,
                                anvil_mir_isel_takes_symbol_fn takes_symbol, void *ctx);
void anvil_mir_isel_facts_destroy(anvil_mir_isel_facts_t *facts);

/* The constant a GPR vreg holds. */
bool anvil_mir_isel_constant(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, int64_t *out);
/* The symbol address (plus offset) a GPR vreg holds. */
bool anvil_mir_isel_symbol(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg, const char **symbol, int64_t *offset);

/* The opcode of `vreg`'s definitions, when they all share it
 * (ANVIL_MIR_OP_INVALID when they differ or there is none): a printer that knows what its form of that instruction
 * leaves in the upper bits (a narrow load extending, a narrow operation
 * zeroing) can skip an extension. */
anvil_mir_opcode_t anvil_mir_isel_def_op(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether the register holding `vreg` is zero above the value's own width:
 * its only definition is a non-negative constant, an AND with one, a ZEXT,
 * a narrow unsigned load (printers leave narrow loads extended by their
 * signedness), a logical right shift by a constant, a compare, or a copy of
 * such a value.  It holds for a printer whose forms of those operations
 * leave the register's upper bits zero; there a ZEXT of the value is a
 * copy. */
bool anvil_mir_isel_zero_extended(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether every reader of `vreg` needs only the bits of its own width: a
 * store of it, a truncation, an == / != compare with zero, or an extension
 * read only by such compares.  A printer
 * may then leave any definition of it (a narrow load) unextended: the
 * register's upper bits are read by nothing. */
bool anvil_mir_isel_low_bits_only(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether the MOV or SYMBOL_ADDR defining `vreg` must still be emitted. */
bool anvil_mir_isel_materialize(const anvil_mir_isel_facts_t *facts, anvil_mir_vreg_t vreg);

/* Whether the compare at `index` is consumed only by the BR_COND right after
 * it, so the printer can branch on the condition flags instead. */
bool anvil_mir_isel_feeds_next_branch(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info);
/* The same for the condition of the SELECT right after it (a conditional
 * move on the flags). */
bool anvil_mir_isel_feeds_next_select(const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info);

bool anvil_mir_isel_is_compare(anvil_mir_opcode_t op);

/* Folding an instruction into its reader (a load as a memory operand, a
 * shift as a shifted operand): the instruction reading `index`'s result,
 * its only reader, at most `window` instructions on in the same block,
 * provided nothing between writes a register `index` reads, clobbers its
 * registers' class or calls (`memory`: or writes memory).  SIZE_MAX when
 * there is none.  Needs the register assignment. */
size_t anvil_mir_isel_fold_reader(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t window, bool memory);

/* Before allocation, with facts built by the printer's own rules: marks
 * registerless (anvil_mir_set_registerless) the constants no printed
 * instruction materializes and the integer compares whose only reader is
 * the branch right after (the printer branches on the flags).  The
 * printer must then never need their registers. */
bool anvil_mir_isel_mark_registerless(anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts);
/* Whether instruction `index` respects its registerless result (prints it
 * as nothing, or as flags for the next branch): printers fail otherwise
 * rather than write a register that holds something else. */
bool anvil_mir_isel_registerless_ok(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, const anvil_mir_instr_info_t *info);

/* Whether an instruction the printers print writes physical register
 * `phys` of `reg_class`: as its result or its printer scratch (constants
 * never materialized write nothing).  A callee-saved register needs saving
 * exactly then. */
bool anvil_mir_isel_writes(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, anvil_mir_reg_class_t reg_class, int phys);

/* The only reader of `index`'s result, at most `window` instructions on
 * in the same block with no call between; SIZE_MAX when there is none. */
size_t anvil_mir_isel_only_reader(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t window);

/* Whether nothing strictly between instructions `from` and `to` writes the
 * register holding `vreg` (or clobbers it, or calls); constants never
 * materialized write nothing. */
bool anvil_mir_isel_keeps(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t from, size_t to, anvil_mir_vreg_t vreg);

/* How a multiply by the constant `value` (`width` bits) runs without a
 * multiplier: by 2^shift, a shift; by 2^shift + 1 (shift >= 1), x + (x <<
 * shift), which a target with a shifted operand or a scaled address takes
 * in one instruction. */
typedef enum { ANVIL_MIR_MUL_MULTIPLY, ANVIL_MIR_MUL_SHIFT, ANVIL_MIR_MUL_SHIFT_ADD } anvil_mir_mul_form_t;
anvil_mir_mul_form_t anvil_mir_isel_mul_form(int64_t value, unsigned width, unsigned *shift);

/* Counter loops: an innermost loop whose counter j only counts up to zero
 * (count_to_zero's shape): one definition before the loop (INIT), an
 * in-place j = j + 1 in the latch (STEP) and j != 0 there consumed by the
 * branch back (TEST, then BRANCH), j otherwise read only by == / != tests
 * with zero (which the counter's negation keeps) when the target counts in
 * j's own register (`counts_in_place`), by anything before the loop
 * otherwise.  A target with a
 * decrement-and-branch counter then counts -j down instead: set it after
 * INIT, print nothing for STEP and TEST, and branch on the counter.  With
 * `calls_clobber_counter` (a special register) the loop and the stretch
 * from INIT to it hold no calls.  A counter narrower than
 * `narrowest_counter` bits, which the target cannot count modulo its width,
 * stays as it is.  `roles` has one entry per instruction. */
enum { ANVIL_MIR_COUNTER_NONE, ANVIL_MIR_COUNTER_INIT, ANVIL_MIR_COUNTER_STEP, ANVIL_MIR_COUNTER_TEST, ANVIL_MIR_COUNTER_BRANCH };
bool anvil_mir_counter_loops(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, bool calls_clobber_counter, bool counts_in_place,
                             unsigned narrowest_counter, uint8_t *roles);

/* Block layout after allocation.  A block (not the entry or the frame
 * block) that prints
 * nothing but its final unconditional jump (copies within one register:
 * PHI moves the allocator made free) is not printed (`skip`); a branch to
 * it goes where it leads (`forward`, through chains), and the block a
 * printer falls into after block b is `next[b]` (num_blocks: none). */
typedef struct {
    anvil_mir_block_t *forward;
    anvil_mir_block_t *next;
    bool *skip;
    size_t num_blocks;
} anvil_mir_layout_t;

bool anvil_mir_layout_build(anvil_mir_layout_t *layout, const anvil_mir_func_t *mir);
void anvil_mir_layout_destroy(anvil_mir_layout_t *layout);
/* Where a branch to `block` lands (the block itself without a layout). */
anvil_mir_block_t anvil_mir_layout_target(const anvil_mir_layout_t *layout, anvil_mir_block_t block);
/* Whether a branch from `from` to `target` falls through. */
bool anvil_mir_layout_falls_into(const anvil_mir_layout_t *layout, anvil_mir_block_t from, anvil_mir_block_t target);

/* Whether the blocks before `frame_block` (anvil_mir_frame_block) can run
 * without the frame: compares only the next branch reads (registerless),
 * branches, returns of nothing and constants never materialized, reading no
 * spilled value.  A printer then places its prologue at frame_block and
 * returns from those blocks without an epilogue. */
bool anvil_mir_isel_frameless_prefix(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, anvil_mir_block_t frame_block);

/* x + (a cmp b): an integer compare at `index` (not feeding a branch or
 * select) read only by a zero extension right after it, read only by an
 * add right after that.  Fills the two indexes and the add's other
 * operand: a printer adds the condition itself (ARM64 cinc, x86 adc on the
 * carry) and prints nothing for the extension. */
bool anvil_mir_isel_condition_add(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, size_t *extension, size_t *add,
                                  anvil_mir_vreg_t *other);

#endif
