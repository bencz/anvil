#ifndef ANVIL_SMALLTALK_PLATFORM_FIBER_H
#define ANVIL_SMALLTALK_PLATFORM_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct st_native_fiber st_native_fiber_t;
typedef void (*st_native_fiber_entry_fn)(void *argument);

/* Contexts belong to the creating OS thread. The root borrows its stack;
 * children own guarded stacks. An entry must switch away before returning.
 * destroy is legal only for a context that is not currently executing. */
typedef struct {
    st_native_fiber_t *(*capture)(void);
    st_native_fiber_t *(*create)(size_t stack_size, st_native_fiber_entry_fn entry, void *argument);
    void (*transfer)(st_native_fiber_t *from, st_native_fiber_t *to);
    void (*destroy)(st_native_fiber_t *fiber);
    bool (*release)(st_native_fiber_t *root);
    uint64_t (*milliseconds)(void);
    void (*sleep)(uint32_t milliseconds);
    uintptr_t (*thread_id)(void);
} st_fiber_platform_ops_t;

extern const st_fiber_platform_ops_t st_fiber_platform;

#endif
