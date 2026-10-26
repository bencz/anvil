/* Target-independent MachineIR building blocks for aggregates passed or
 * returned by value (BYVAL / SRET parameters).  Target lowerers classify an
 * aggregate with their ABI module and use these helpers to move its parts
 * between memory and argument/return registers.  Only loads, stores,
 * extensions and truncations are emitted, never shifts, so no emitter
 * scratch register is involved. */
#ifndef ANVIL_BACKEND_AGGREGATE_MIR_H
#define ANVIL_BACKEND_AGGREGATE_MIR_H

#include "anvil/anvil_machine.h"
#include <stdbool.h>
#include <stddef.h>

/* Register shape of the target. */
typedef struct {
    unsigned word;          /* general register width in bytes */
    bool big_endian;
} anvil_agg_target_t;

/* One register-sized piece of an aggregate.  A general register holds the
 * bytes [offset, offset + size) of the object at bytes [pad, pad + size) of
 * its memory image, so pad = word - size right-justifies a part on a
 * big-endian target; pad is 0 for the usual left-justified image. */
typedef struct {
    bool fp;                /* floating register (4 or 8 bytes) */
    unsigned size;          /* bytes, 1..word */
    unsigned offset;
    unsigned pad;
} anvil_agg_part_t;

/* Address (a word-sized register) of a new frame object of `size` bytes. */
anvil_mir_vreg_t anvil_agg_frame_object(anvil_mir_func_t *mir, size_t size, size_t align,
                                        anvil_agg_target_t target);

/* Register width in bits for a part: 32/64 for floating parts, the word
 * width for integer parts. */
uint16_t anvil_agg_part_bits(const anvil_agg_part_t *part, anvil_agg_target_t target);

/* Loads a part from base+offset into a register value (bytes outside the
 * part are zero).  Never reads memory outside the part. */
anvil_mir_vreg_t anvil_agg_load_part(anvil_mir_func_t *mir, const anvil_agg_part_t *part,
                                     anvil_mir_vreg_t base, anvil_agg_target_t target);

/* Stores exactly part->size bytes of a register value to base+offset. */
bool anvil_agg_store_part(anvil_mir_func_t *mir, const anvil_agg_part_t *part,
                          anvil_mir_vreg_t value, anvil_mir_vreg_t base, anvil_agg_target_t target);

/* Integer part [offset, offset + word) of an object of `size` bytes, as laid
 * out in memory (left-justified). */
anvil_agg_part_t anvil_agg_chunk(size_t size, size_t offset, anvil_agg_target_t target);

/* Copies `size` bytes using the widest naturally sized accesses. */
bool anvil_agg_copy(anvil_mir_func_t *mir, anvil_mir_vreg_t from, anvil_mir_vreg_t to,
                    size_t size, anvil_agg_target_t target);

#endif
