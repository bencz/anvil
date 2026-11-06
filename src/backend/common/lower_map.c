#include "lower_map.h"
#include "ir_lower.h"

#include <stdlib.h>

enum { LOWER_MAP_INITIAL = 64 };

static size_t slot_of(const anvil_lower_map_t *map, const void *key)
{
    /* Fibonacci hashing of the address (its low bits are alignment). */
    uint64_t bits = (uint64_t)(uintptr_t)key * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(bits >> 32) & (map->capacity - 1);
}

static anvil_lower_map_entry_t *probe(const anvil_lower_map_t *map, const void *key)
{
    size_t slot = slot_of(map, key);
    while (map->entries[slot].key && map->entries[slot].key != key)
        slot = (slot + 1) & (map->capacity - 1);
    return &map->entries[slot];
}

/* Keeps the load at most three quarters (a probe always ends). */
static bool grow(anvil_lower_map_t *map)
{
    if (map->capacity && (map->count + 1) * 4 <= map->capacity * 3)
        return true;
    size_t capacity = map->capacity ? map->capacity * 2 : LOWER_MAP_INITIAL;
    if (capacity < map->capacity || capacity > SIZE_MAX / sizeof(anvil_lower_map_entry_t))
        return false;
    anvil_lower_map_t grown = {calloc(capacity, sizeof(anvil_lower_map_entry_t)), capacity, map->count};
    if (!grown.entries)
        return false;
    for (size_t slot = 0; slot < map->capacity; slot++)
        if (map->entries[slot].key)
            *probe(&grown, map->entries[slot].key) = map->entries[slot];
    free(map->entries);
    *map = grown;
    return true;
}

bool anvil_lower_map_put(anvil_lower_map_t *map, const void *key, int64_t first, int64_t second, bool flag)
{
    if (!map || !key)
        return false;
    anvil_lower_map_entry_t *entry = map->capacity ? probe(map, key) : NULL;
    if (!entry || !entry->key) {
        if (!grow(map))
            return false;
        entry = probe(map, key);
        map->count++;
    }
    *entry = (anvil_lower_map_entry_t){key, first, second, flag};
    return true;
}

const anvil_lower_map_entry_t *anvil_lower_map_find(const anvil_lower_map_t *map, const void *key)
{
    if (!map || !key || !map->capacity)
        return NULL;
    const anvil_lower_map_entry_t *entry = probe(map, key);
    return entry->key ? entry : NULL;
}

void anvil_lower_map_destroy(anvil_lower_map_t *map)
{
    if (!map)
        return;
    free(map->entries);
    *map = (anvil_lower_map_t){0};
}

bool anvil_lower_create_blocks(anvil_mir_func_t *mir, const anvil_func_t *func, anvil_lower_map_t *blocks)
{
    bool first = true;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_mir_block_t mir_block = anvil_mir_block_for(mir, block, first);
        if (mir_block == ANVIL_MIR_NO_BLOCK || !anvil_lower_block_put(blocks, block, mir_block))
            return false;
        first = false;
    }
    return true;
}
