#include "hash.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

#define HASH_INITIAL_CAPACITY 16
#define HASH_LOAD_FACTOR 0.75

static void anvil_hash_grow(AnvilHash* hash);

void anvil_hash_init(AnvilHash* hash) {
    anvil_hash_init_sized(hash, HASH_INITIAL_CAPACITY);
}

void anvil_hash_init_sized(AnvilHash* hash, size_t initial_capacity) {
    if (initial_capacity < HASH_INITIAL_CAPACITY) {
        initial_capacity = HASH_INITIAL_CAPACITY;
    }
    size_t cap = 1;
    while (cap < initial_capacity) cap *= 2;
    
    hash->entries = (AnvilHashEntry*)calloc(cap, sizeof(AnvilHashEntry));
    hash->capacity = cap;
    hash->size = 0;
    hash->deleted = 0;
}

void anvil_hash_free(AnvilHash* hash) {
    if (hash->entries) {
        free(hash->entries);
        hash->entries = NULL;
    }
    hash->capacity = 0;
    hash->size = 0;
    hash->deleted = 0;
}

void anvil_hash_clear(AnvilHash* hash) {
    if (hash->entries) {
        memset(hash->entries, 0, hash->capacity * sizeof(AnvilHashEntry));
    }
    hash->size = 0;
    hash->deleted = 0;
}

static size_t anvil_hash_find_slot(const AnvilHash* hash, const char* key, uint32_t h) {
    size_t mask = hash->capacity - 1;
    size_t idx = h & mask;
    size_t first_deleted = SIZE_MAX;
    
    for (size_t i = 0; i < hash->capacity; i++) {
        size_t probe = (idx + i) & mask;
        AnvilHashEntry* entry = &hash->entries[probe];
        
        if (!entry->occupied && !entry->deleted) {
            return first_deleted != SIZE_MAX ? first_deleted : probe;
        }
        if (entry->deleted && first_deleted == SIZE_MAX) {
            first_deleted = probe;
            continue;
        }
        if (entry->occupied && entry->hash == h && anvil_str_eq(entry->key, key)) {
            return probe;
        }
    }
    return first_deleted != SIZE_MAX ? first_deleted : 0;
}

static void anvil_hash_grow(AnvilHash* hash) {
    size_t old_cap = hash->capacity;
    AnvilHashEntry* old_entries = hash->entries;
    
    size_t new_cap = old_cap * 2;
    hash->entries = (AnvilHashEntry*)calloc(new_cap, sizeof(AnvilHashEntry));
    hash->capacity = new_cap;
    hash->size = 0;
    hash->deleted = 0;
    
    for (size_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            anvil_hash_insert(hash, old_entries[i].key, old_entries[i].value);
        }
    }
    free(old_entries);
}

void anvil_hash_insert(AnvilHash* hash, const char* key, void* value) {
    if ((hash->size + hash->deleted + 1) > (size_t)(hash->capacity * HASH_LOAD_FACTOR)) {
        anvil_hash_grow(hash);
    }
    
    uint32_t h = anvil_str_hash32(key);
    size_t idx = anvil_hash_find_slot(hash, key, h);
    AnvilHashEntry* entry = &hash->entries[idx];
    
    if (entry->occupied && !entry->deleted) {
        entry->value = value;
    } else {
        if (entry->deleted) hash->deleted--;
        entry->key = key;
        entry->value = value;
        entry->hash = h;
        entry->occupied = true;
        entry->deleted = false;
        hash->size++;
    }
}

void* anvil_hash_get(const AnvilHash* hash, const char* key) {
    if (hash->size == 0) return NULL;
    
    uint32_t h = anvil_str_hash32(key);
    size_t mask = hash->capacity - 1;
    size_t idx = h & mask;
    
    for (size_t i = 0; i < hash->capacity; i++) {
        size_t probe = (idx + i) & mask;
        AnvilHashEntry* entry = &hash->entries[probe];
        
        if (!entry->occupied && !entry->deleted) return NULL;
        if (entry->occupied && !entry->deleted && 
            entry->hash == h && anvil_str_eq(entry->key, key)) {
            return entry->value;
        }
    }
    return NULL;
}

bool anvil_hash_contains(const AnvilHash* hash, const char* key) {
    if (hash->size == 0) return false;
    
    uint32_t h = anvil_str_hash32(key);
    size_t mask = hash->capacity - 1;
    size_t idx = h & mask;
    
    for (size_t i = 0; i < hash->capacity; i++) {
        size_t probe = (idx + i) & mask;
        AnvilHashEntry* entry = &hash->entries[probe];
        
        if (!entry->occupied && !entry->deleted) return false;
        if (entry->occupied && !entry->deleted && 
            entry->hash == h && anvil_str_eq(entry->key, key)) {
            return true;
        }
    }
    return false;
}

bool anvil_hash_remove(AnvilHash* hash, const char* key) {
    if (hash->size == 0) return false;
    
    uint32_t h = anvil_str_hash32(key);
    size_t mask = hash->capacity - 1;
    size_t idx = h & mask;
    
    for (size_t i = 0; i < hash->capacity; i++) {
        size_t probe = (idx + i) & mask;
        AnvilHashEntry* entry = &hash->entries[probe];
        
        if (!entry->occupied && !entry->deleted) return false;
        if (entry->occupied && !entry->deleted && 
            entry->hash == h && anvil_str_eq(entry->key, key)) {
            entry->deleted = true;
            hash->size--;
            hash->deleted++;
            return true;
        }
    }
    return false;
}

size_t anvil_hash_size(const AnvilHash* hash) {
    return hash->size;
}

bool anvil_hash_empty(const AnvilHash* hash) {
    return hash->size == 0;
}

AnvilHashIter anvil_hash_iter(const AnvilHash* hash) {
    AnvilHashIter iter = { hash, 0 };
    return iter;
}

bool anvil_hash_iter_next(AnvilHashIter* iter, const char** key, void** value) {
    while (iter->index < iter->hash->capacity) {
        AnvilHashEntry* entry = &iter->hash->entries[iter->index++];
        if (entry->occupied && !entry->deleted) {
            if (key) *key = entry->key;
            if (value) *value = entry->value;
            return true;
        }
    }
    return false;
}
