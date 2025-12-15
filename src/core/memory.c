/*
 * ANVIL - Memory management utilities
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

void anvil_pool_init(anvil_pool_t *pool, size_t block_size)
{
    if (!pool) return;
    pool->blocks = NULL;
    pool->block_size = block_size;
    pool->used = 0;
    pool->next = NULL;
}

void anvil_pool_destroy(anvil_pool_t *pool)
{
    if (!pool) return;
    
    /* Free all blocks in the chain */
    anvil_pool_t *p = pool;
    while (p) {
        anvil_pool_t *next = p->next;
        free(p->blocks);
        if (p != pool) {
            free(p);
        }
        p = next;
    }
    
    pool->blocks = NULL;
    pool->used = 0;
    pool->next = NULL;
}

void *anvil_alloc(anvil_ctx_t *ctx, size_t size)
{
    (void)ctx;
    return calloc(1, size);
}

void *anvil_realloc(anvil_ctx_t *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

char *anvil_strdup(anvil_ctx_t *ctx, const char *str)
{
    (void)ctx;
    return str ? strdup(str) : NULL;
}
