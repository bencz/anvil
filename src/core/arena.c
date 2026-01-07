#include "arena.h"
#include <stdlib.h>
#include <string.h>

static AnvilArenaBlock* anvil_arena_block_new(size_t min_size) {
    size_t size = min_size > ANVIL_ARENA_BLOCK_SIZE ? min_size : ANVIL_ARENA_BLOCK_SIZE;
    AnvilArenaBlock* block = (AnvilArenaBlock*)malloc(sizeof(AnvilArenaBlock) + size);
    if (!block) return NULL;
    block->next = NULL;
    block->size = size;
    block->used = 0;
    return block;
}

AnvilArena* anvil_arena_new(void) {
    AnvilArena* arena = (AnvilArena*)malloc(sizeof(AnvilArena));
    if (!arena) return NULL;
    
    arena->first = anvil_arena_block_new(ANVIL_ARENA_BLOCK_SIZE);
    if (!arena->first) {
        free(arena);
        return NULL;
    }
    arena->current = arena->first;
    arena->total_allocated = 0;
    return arena;
}

void anvil_arena_free(AnvilArena* arena) {
    if (!arena) return;
    AnvilArenaBlock* block = arena->first;
    while (block) {
        AnvilArenaBlock* next = block->next;
        free(block);
        block = next;
    }
    free(arena);
}

void anvil_arena_reset(AnvilArena* arena) {
    if (!arena) return;
    AnvilArenaBlock* block = arena->first;
    while (block) {
        block->used = 0;
        block = block->next;
    }
    arena->current = arena->first;
    arena->total_allocated = 0;
}

void* anvil_arena_alloc_aligned(AnvilArena* arena, size_t size, size_t align) {
    if (!arena || size == 0) return NULL;
    
    AnvilArenaBlock* block = arena->current;
    size_t padding = (align - (((uintptr_t)block->data + block->used) % align)) % align;
    size_t total = size + padding;
    
    if (block->used + total > block->size) {
        AnvilArenaBlock* new_block = anvil_arena_block_new(total);
        if (!new_block) return NULL;
        block->next = new_block;
        arena->current = new_block;
        block = new_block;
        padding = (align - ((uintptr_t)block->data % align)) % align;
        total = size + padding;
    }
    
    void* ptr = block->data + block->used + padding;
    block->used += total;
    arena->total_allocated += size;
    return ptr;
}

void* anvil_arena_alloc(AnvilArena* arena, size_t size) {
    return anvil_arena_alloc_aligned(arena, size, 8);
}

char* anvil_arena_strdup(AnvilArena* arena, const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char* dup = (char*)anvil_arena_alloc(arena, len);
    if (dup) memcpy(dup, str, len);
    return dup;
}

char* anvil_arena_strndup(AnvilArena* arena, const char* str, size_t n) {
    if (!str) return NULL;
    size_t len = strlen(str);
    if (len > n) len = n;
    char* dup = (char*)anvil_arena_alloc(arena, len + 1);
    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }
    return dup;
}

void* anvil_arena_memdup(AnvilArena* arena, const void* data, size_t size) {
    if (!data || size == 0) return NULL;
    void* dup = anvil_arena_alloc(arena, size);
    if (dup) memcpy(dup, data, size);
    return dup;
}
