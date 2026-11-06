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
#include "lower_map.h"
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

/* Whether a 64-bit value is unsigned (its pair's high half then is). */
static inline bool anvil_wide_value_unsigned(const anvil_value_t *value)
{
    return value && value->type && value->type->kind == ANVIL_TYPE_U64;
}

/* A 64-bit value's pair in a lowering's map (anvil_lower_map_t). */
static inline bool anvil_lower_pair_put(anvil_lower_map_t *map, const anvil_value_t *value, anvil_mir_vreg_t hi, anvil_mir_vreg_t lo, bool is_unsigned)
{
    return hi != ANVIL_MIR_NO_VREG && lo != ANVIL_MIR_NO_VREG && anvil_lower_map_put(map, value, hi, lo, is_unsigned);
}

static inline bool anvil_lower_wide_put(anvil_lower_map_t *map, const anvil_value_t *value, anvil_wide_pair_t pair)
{
    return anvil_lower_pair_put(map, value, pair.hi, pair.lo, anvil_wide_value_unsigned(value));
}

static inline bool anvil_lower_pair_get(const anvil_lower_map_t *map, const anvil_value_t *value, anvil_mir_vreg_t *hi, anvil_mir_vreg_t *lo,
                                        bool *is_unsigned)
{
    const anvil_lower_map_entry_t *entry = anvil_lower_map_find(map, value);
    if (!entry)
        return false;
    if (hi)
        *hi = (anvil_mir_vreg_t)entry->first;
    if (lo)
        *lo = (anvil_mir_vreg_t)entry->second;
    if (is_unsigned)
        *is_unsigned = entry->flag;
    return true;
}

/* A 64-bit constant: an integer constant's own, or one the lowering
 * computed for the value and recorded. */
static inline bool anvil_lower_constant_put(anvil_lower_map_t *map, const anvil_value_t *value, int64_t constant)
{
    return anvil_lower_map_put(map, value, constant, 0, false);
}

static inline bool anvil_lower_constant_get(const anvil_lower_map_t *map, const anvil_value_t *value, int64_t *constant)
{
    if (value && value->kind == ANVIL_VAL_CONST_INT) {
        if (constant)
            *constant = value->data.i;
        return true;
    }
    const anvil_lower_map_entry_t *entry = anvil_lower_map_find(map, value);
    if (entry && constant)
        *constant = entry->first;
    return entry != NULL;
}

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
