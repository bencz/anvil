/* What a MachineIR lowering made of each IR value or block: one map keyed
 * by pointer, shared by every backend.  Only looked up, never iterated, so
 * its hash order never reaches the output. */
#ifndef ANVIL_BACKEND_LOWER_MAP_H
#define ANVIL_BACKEND_LOWER_MAP_H

#include "anvil/anvil_internal.h"
#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const void *key;
    int64_t first;
    int64_t second;
    bool flag;
} anvil_lower_map_entry_t;

/* Zero-initialized empty. */
typedef struct {
    anvil_lower_map_entry_t *entries;
    size_t capacity;
    size_t count;
} anvil_lower_map_t;

/* Records (or replaces) `key`'s entry; false for a NULL key or when the
 * table cannot grow (the map is unchanged then). */
bool anvil_lower_map_put(anvil_lower_map_t *map, const void *key, int64_t first, int64_t second, bool flag);
const anvil_lower_map_entry_t *anvil_lower_map_find(const anvil_lower_map_t *map, const void *key);
void anvil_lower_map_destroy(anvil_lower_map_t *map);

/* An IR value's virtual register. */
static inline bool anvil_lower_vreg_put(anvil_lower_map_t *map, const anvil_value_t *value, anvil_mir_vreg_t vreg)
{
    return vreg != ANVIL_MIR_NO_VREG && anvil_lower_map_put(map, value, vreg, 0, false);
}

static inline anvil_mir_vreg_t anvil_lower_vreg_get(const anvil_lower_map_t *map, const anvil_value_t *value)
{
    const anvil_lower_map_entry_t *entry = anvil_lower_map_find(map, value);
    return entry ? (anvil_mir_vreg_t)entry->first : ANVIL_MIR_NO_VREG;
}

/* An address that is a register plus a constant (a GEP of constant
 * indices, kept for the accesses' displacements). */
static inline bool anvil_lower_address_put(anvil_lower_map_t *map, const anvil_value_t *value, anvil_mir_vreg_t base, int64_t offset)
{
    return base != ANVIL_MIR_NO_VREG && anvil_lower_map_put(map, value, base, offset, false);
}

static inline bool anvil_lower_address_get(const anvil_lower_map_t *map, const anvil_value_t *value, anvil_mir_vreg_t *base, int64_t *offset)
{
    const anvil_lower_map_entry_t *entry = anvil_lower_map_find(map, value);
    if (!entry)
        return false;
    if (base)
        *base = (anvil_mir_vreg_t)entry->first;
    if (offset)
        *offset = entry->second;
    return true;
}

/* An IR block's MachineIR block. */
static inline bool anvil_lower_block_put(anvil_lower_map_t *map, const anvil_block_t *block, anvil_mir_block_t mir_block)
{
    return anvil_lower_map_put(map, block, mir_block, 0, false);
}

static inline anvil_mir_block_t anvil_lower_block_get(const anvil_lower_map_t *map, const anvil_block_t *block)
{
    const anvil_lower_map_entry_t *entry = anvil_lower_map_find(map, block);
    return entry ? (anvil_mir_block_t)entry->first : ANVIL_MIR_NO_BLOCK;
}

/* One MachineIR block per IR block of `func`, in its order (the first is
 * the entry), recorded in `blocks`. */
bool anvil_lower_create_blocks(anvil_mir_func_t *mir, const anvil_func_t *func, anvil_lower_map_t *blocks);

#endif
