/* Legalization of 64-bit integer operations for targets whose general
 * registers hold 32 bits.  A 64-bit value travels as a pair of 32-bit
 * virtual registers; these helpers express every operation with
 * target-independent MachineIR (carries through unsigned compares), and
 * reach the target only to call a run-time helper (multiplication,
 * division, variable shifts, floating conversions) in its calling
 * convention. */
#ifndef ANVIL_BACKEND_WIDE_MIR_H
#define ANVIL_BACKEND_WIDE_MIR_H

#include "anvil/anvil.h"
#include "anvil/anvil_machine.h"
#include <stdbool.h>
#include <stdint.h>

/* A 64-bit value (hi set) or, as a helper operand, one register value
 * (hi == ANVIL_MIR_NO_VREG, the value in lo). */
typedef struct {
    anvil_mir_vreg_t hi;
    anvil_mir_vreg_t lo;
} anvil_wide_pair_t;

typedef struct anvil_wide_target {
    /* Calls the run-time helper `name` (libgcc / compiler-rt ABI) with the
     * target's C calling convention.  `result` vregs are allocated by the
     * caller and must be defined by the call sequence. */
    bool (*call_helper)(void *ctx, const char *name, const anvil_wide_pair_t *args, size_t num_args,
                        anvil_wide_pair_t result);
    void *ctx;
} anvil_wide_target_t;

/* New pair; the high half carries the value's signedness. */
bool anvil_wide_new(anvil_mir_func_t *mir, bool is_unsigned, anvil_wide_pair_t *out);

bool anvil_wide_constant(anvil_mir_func_t *mir, int64_t value, bool is_unsigned, anvil_wide_pair_t *out);

/* ADD, SUB, MUL, SDIV, UDIV, SMOD, UMOD, AND, OR, XOR. */
bool anvil_wide_binary(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_op_t op,
                       anvil_wide_pair_t a, anvil_wide_pair_t b, bool is_unsigned, anvil_wide_pair_t *out);

/* SHL, SHR (logical), SAR.  A known count (0..63) expands inline; otherwise
 * `count` (32-bit) is passed to the helper. */
bool anvil_wide_shift(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_op_t op,
                      anvil_wide_pair_t a, anvil_mir_vreg_t count, bool count_known, int64_t known_count,
                      bool is_unsigned, anvil_wide_pair_t *out);

bool anvil_wide_neg(anvil_mir_func_t *mir, anvil_wide_pair_t a, bool is_unsigned, anvil_wide_pair_t *out);
bool anvil_wide_not(anvil_mir_func_t *mir, anvil_wide_pair_t a, bool is_unsigned, anvil_wide_pair_t *out);

/* Integer compare (MIR CMP_* opcode) into an 8-bit boolean. */
bool anvil_wide_compare(anvil_mir_func_t *mir, anvil_mir_opcode_t op, anvil_wide_pair_t a, anvil_wide_pair_t b,
                        anvil_mir_vreg_t result);

/* 1..32-bit integer to 64 bits, by sign or zero extension. */
bool anvil_wide_extend(anvil_mir_func_t *mir, anvil_mir_vreg_t src, bool sign_extend, bool is_unsigned,
                       anvil_wide_pair_t *out);

/* Low `bits` (1..32) of a 64-bit value. */
anvil_mir_vreg_t anvil_wide_truncate(anvil_mir_func_t *mir, anvil_wide_pair_t a, uint16_t bits, bool is_signed);

/* 64-bit integer <-> floating point through the run-time helpers. */
bool anvil_wide_to_fp(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_wide_pair_t a,
                      bool is_unsigned, uint16_t fp_bits, anvil_mir_vreg_t *out);
bool anvil_wide_from_fp(anvil_mir_func_t *mir, const anvil_wide_target_t *target, anvil_mir_vreg_t src,
                        bool is_unsigned, anvil_wide_pair_t *out);

#endif
