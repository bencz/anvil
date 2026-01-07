#ifndef ANVIL_ARENA_H
#define ANVIL_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_ARENA_BLOCK_SIZE (64 * 1024)

typedef struct AnvilArenaBlock {
    struct AnvilArenaBlock* next;
    size_t size;
    size_t used;
    uint8_t data[];
} AnvilArenaBlock;

typedef struct AnvilArena {
    AnvilArenaBlock* first;
    AnvilArenaBlock* current;
    size_t total_allocated;
} AnvilArena;

AnvilArena* anvil_arena_new(void);
void anvil_arena_free(AnvilArena* arena);
void anvil_arena_reset(AnvilArena* arena);
void* anvil_arena_alloc(AnvilArena* arena, size_t size);
void* anvil_arena_alloc_aligned(AnvilArena* arena, size_t size, size_t align);
char* anvil_arena_strdup(AnvilArena* arena, const char* str);
char* anvil_arena_strndup(AnvilArena* arena, const char* str, size_t n);
void* anvil_arena_memdup(AnvilArena* arena, const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
