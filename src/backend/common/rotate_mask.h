/* Rotate-and-mask: a value rotated left within a register and ANDed with a
 * constant mask, the operation of the rotate-and-mask instructions some
 * targets have (one instruction for a shift by a constant, an AND with a
 * contiguous mask, or one of each).  A printer describes an instruction as
 * one (anvil_rotate_mask_of), composes it with the instruction reading its
 * result when that is the only reader shortly after
 * (anvil_mir_rotate_mask_pair), and spells the composition when its mask is
 * one run of ones that its instructions take. */
#ifndef ANVIL_ROTATE_MASK_H
#define ANVIL_ROTATE_MASK_H

#include "mir_isel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned width;   /* the register bits rotated: 32 or 64 */
    unsigned rotate;  /* left, below width */
    uint64_t mask;    /* within width */
} anvil_rotate_mask_t;

/* `op` (SHL or SHR by `imm`, AND with `imm`, MUL by a power of two `imm`)
 * on `bits`-bit operands as a rotate-and-mask of a `width`-bit register
 * (bits <= width; the result's bits above `bits` are zero). */
bool anvil_rotate_mask_of(anvil_mir_opcode_t op, int64_t imm, unsigned bits, unsigned width, anvil_rotate_mask_t *out);

/* `first`, then `second` on its result. */
anvil_rotate_mask_t anvil_rotate_mask_then(const anvil_rotate_mask_t *first, const anvil_rotate_mask_t *second);

/* Whether the mask is one run of ones, without wrapping: from bit `begin`
 * to bit `end`, numbered from the most significant (bit 0) as the
 * rotate-and-mask instructions number them. */
bool anvil_rotate_mask_run(const anvil_rotate_mask_t *rm, unsigned *begin, unsigned *end);

/* The instruction at `index` (an operation anvil_rotate_mask_of takes, its
 * second operand a constant) read only by another such shortly after with
 * nothing writing its first operand's register between (see
 * anvil_mir_isel_fold_reader), the reader taking the result as its first
 * operand (either, for an AND): their composition, the reader and the
 * first operand.  Both on operands of one width, or the first on 32 bits
 * and the reader on 64 reading its zero extension (`between`, otherwise
 * SIZE_MAX: the composition makes it unneeded).  The composition rotates
 * at least `min_width` bits (the operands' width when wider). */
bool anvil_mir_rotate_mask_pair(const anvil_mir_func_t *mir, const anvil_mir_isel_facts_t *facts, size_t index, unsigned min_width,
                                anvil_rotate_mask_t *out, size_t *reader, size_t *between, anvil_mir_vreg_t *source);

#endif
