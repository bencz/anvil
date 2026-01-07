#ifndef ANVIL_HASH_H
#define ANVIL_HASH_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilHashEntry {
    const char* key;
    void* value;
    uint32_t hash;
    bool occupied;
    bool deleted;
} AnvilHashEntry;

typedef struct AnvilHash {
    AnvilHashEntry* entries;
    size_t capacity;
    size_t size;
    size_t deleted;
} AnvilHash;

void anvil_hash_init(AnvilHash* hash);
void anvil_hash_init_sized(AnvilHash* hash, size_t initial_capacity);
void anvil_hash_free(AnvilHash* hash);
void anvil_hash_clear(AnvilHash* hash);
void anvil_hash_insert(AnvilHash* hash, const char* key, void* value);
void* anvil_hash_get(const AnvilHash* hash, const char* key);
bool anvil_hash_contains(const AnvilHash* hash, const char* key);
bool anvil_hash_remove(AnvilHash* hash, const char* key);
size_t anvil_hash_size(const AnvilHash* hash);
bool anvil_hash_empty(const AnvilHash* hash);

typedef struct AnvilHashIter {
    const AnvilHash* hash;
    size_t index;
} AnvilHashIter;

AnvilHashIter anvil_hash_iter(const AnvilHash* hash);
bool anvil_hash_iter_next(AnvilHashIter* iter, const char** key, void** value);

#ifdef __cplusplus
}
#endif

#endif
