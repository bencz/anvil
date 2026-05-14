/*
 * ANVIL - Shared value-id → stack-slot-offset map for backends.
 *
 * Previously every backend did a linear scan through a stack_slots[] array
 * to answer "what offset did we give to this value?", once per operand per
 * instruction. This made every backend's codegen O(slots × instrs × ops) in
 * aggregate. The helper here replaces that scan with an O(1) lookup by
 * using anvil_value_t::id as a direct index.
 *
 * Typical use pattern from a backend:
 *
 *   anvil_slot_map_t slots;
 *   anvil_slot_map_init(&slots);
 *   ...
 *   anvil_slot_map_set(&slots, val, offset);
 *   int offset = anvil_slot_map_get(&slots, val);
 *   ...
 *   anvil_slot_map_free(&slots);
 *   anvil_slot_map_reset(&slots); // keep allocation, forget entries
 */

#ifndef ANVIL_BACKEND_SLOT_MAP_H
#define ANVIL_BACKEND_SLOT_MAP_H

#include "anvil/anvil_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ANVIL_SLOT_MAP_EMPTY INT32_MIN

typedef struct {
    int32_t *by_id;       /* indexed by value->id; entry == EMPTY → no slot */
    size_t   cap;
} anvil_slot_map_t;

static inline void anvil_slot_map_init(anvil_slot_map_t *m)
{
    m->by_id = NULL;
    m->cap = 0;
}

static inline void anvil_slot_map_free(anvil_slot_map_t *m)
{
    free(m->by_id);
    m->by_id = NULL;
    m->cap = 0;
}

/* Grow if needed so that entries up to `needed` ids are representable. */
static inline void anvil_slot_map_ensure(anvil_slot_map_t *m, uint32_t needed)
{
    if (needed < m->cap) return;
    size_t new_cap = m->cap ? m->cap * 2 : 64;
    while (new_cap <= needed) new_cap *= 2;
    int32_t *grown = realloc(m->by_id, new_cap * sizeof(*grown));
    if (!grown) return; /* best-effort; a subsequent set/get will bail out */
    for (size_t i = m->cap; i < new_cap; i++) grown[i] = ANVIL_SLOT_MAP_EMPTY;
    m->by_id = grown;
    m->cap = new_cap;
}

/* Erase every recorded entry without freeing the backing storage — useful
 * between function codegen calls inside one backend instance. */
static inline void anvil_slot_map_reset(anvil_slot_map_t *m)
{
    if (!m->by_id) return;
    for (size_t i = 0; i < m->cap; i++) m->by_id[i] = ANVIL_SLOT_MAP_EMPTY;
}

static inline void anvil_slot_map_set(anvil_slot_map_t *m,
                                      const anvil_value_t *val,
                                      int offset)
{
    if (!val) return;
    anvil_slot_map_ensure(m, val->id);
    if (!m->by_id) return;
    m->by_id[val->id] = offset;
}

/* Returns the slot offset or -1 if this value has no slot. */
static inline int anvil_slot_map_get(const anvil_slot_map_t *m,
                                     const anvil_value_t *val)
{
    if (!m->by_id || !val) return -1;
    if (val->id >= m->cap) return -1;
    int32_t off = m->by_id[val->id];
    return (off == ANVIL_SLOT_MAP_EMPTY) ? -1 : (int)off;
}

#endif /* ANVIL_BACKEND_SLOT_MAP_H */
