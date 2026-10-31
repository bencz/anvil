/* A target's vector instructions as data.  One table per backend lists, for
 * each IR lane operation it can run, the lane width, lane kind and operand
 * shape it covers, the CPU features it needs, its cost and its mnemonic.
 * The same table answers the cost hook (IR side), MIR legality and the
 * printer, so the three never disagree. */
#ifndef ANVIL_BACKEND_VECTOR_ISA_H
#define ANVIL_BACKEND_VECTOR_ISA_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ANVIL_VECTOR_LANES_ANY,    /* loads, stores: any element type */
    ANVIL_VECTOR_LANES_INT,
    ANVIL_VECTOR_LANES_FP
} anvil_vector_lane_kind_t;

typedef struct {
    anvil_op_t operation;
    unsigned lane_bits;              /* 8, 16, 32, 64; 0: any width */
    anvil_vector_lane_kind_t lanes;
    anvil_vector_shape_t shape;
    anvil_cpu_features_t features;   /* all required */
    unsigned cost;
    const char *mnemonic;
} anvil_vector_isa_t;

/* What one vector operation asks of the table. */
typedef struct {
    anvil_op_t operation;
    unsigned lane_bits;
    bool floating;
    anvil_vector_shape_t shape;
} anvil_vector_key_t;

/* The entry for `key` usable with `features`, or NULL. */
const anvil_vector_isa_t *anvil_vector_isa_find(const anvil_vector_isa_t *table, size_t count, const anvil_vector_key_t *key,
                                                anvil_cpu_features_t features);

/* The cost hook over a table: 0 unless `type` is a vector of `vector_bytes`
 * the table covers for `operation` and `shape` with the context's CPU. */
unsigned anvil_vector_isa_cost(const anvil_vector_isa_t *table, size_t count, size_t vector_bytes, anvil_ctx_t *ctx, anvil_op_t operation,
                               anvil_type_t *type, anvil_vector_shape_t shape);

/* The key of MIR VECTOR or SCALAR instruction `index`: FP lanes for FP
 * operations and splats of an FPR scalar; UNIFORM when a non-splat's last
 * use is scalar; SCALAR for the scalar opcode. */
bool anvil_vector_mir_key(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, anvil_vector_key_t *key);

/* Whether `vreg`, used by instruction `index`, is a lane mask (each lane
 * all ones or zero): a vector comparison defined earlier in the block.  A
 * printer may then reduce it with a shorter idiom. */
bool anvil_vector_mir_is_mask(const anvil_mir_func_t *mir, size_t index, anvil_mir_vreg_t vreg);

/* Appends "\tMNEMONIC r0, r1, ..." for a three-address target, the
 * destination first; false when an operand name is missing (count <= 4). */
bool anvil_vector_isa_print(anvil_strbuf_t *code, const char *mnemonic, const char *const *operands, size_t count);

/* The table entry MIR VECTOR or SCALAR instruction `index` needs, or NULL. */
const anvil_vector_isa_t *anvil_vector_isa_for_mir(const anvil_vector_isa_t *table, size_t count, const anvil_mir_func_t *mir, size_t index,
                                                   const anvil_mir_instr_info_t *info, anvil_cpu_features_t features);

#endif
