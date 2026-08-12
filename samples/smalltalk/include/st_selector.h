#ifndef ANVIL_SMALLTALK_SELECTOR_H
#define ANVIL_SMALLTALK_SELECTOR_H

#include "st_ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t st_selector_id_t;

#define ST_SELECTOR_INVALID_ID UINT32_C(0)

typedef enum {
    ST_SELECTOR_UNARY,
    ST_SELECTOR_BINARY,
    ST_SELECTOR_KEYWORD
} st_selector_kind_t;

typedef enum {
    ST_SELECTOR_OK = 0,
    ST_SELECTOR_ERR_INVALID_ARGUMENT,
    ST_SELECTOR_ERR_INVALID_SPELLING,
    ST_SELECTOR_ERR_OUT_OF_MEMORY,
    ST_SELECTOR_ERR_OVERFLOW,
    ST_SELECTOR_ERR_ID_EXHAUSTED,
    ST_SELECTOR_ERR_FROZEN
} st_selector_status_t;

typedef void *(*st_selector_allocate_fn)(void *user, size_t size);
typedef void (*st_selector_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_selector_allocate_fn allocate;
    st_selector_deallocate_fn deallocate;
    void *user;
} st_selector_allocator_t;

typedef struct {
    const char *bytes;
    size_t length;
    uint64_t hash;
    uint32_t arity;
    st_selector_kind_t kind;
} st_selector_t;

typedef struct st_selector_table_entry st_selector_table_entry_t;

typedef struct {
    st_selector_t *selectors;
    size_t count;
    size_t selector_capacity;
    st_selector_table_entry_t *entries;
    size_t table_capacity;
    uint64_t hash_seed;
    st_selector_allocator_t allocator;
    st_selector_status_t status;
    bool initialized;
    bool frozen;
} st_selector_table_t;

/* `table` must be zero-initialized or previously destroyed.  A zero allocator
 * selects malloc/free.  `hash_seed` is explicit so snapshot builds can be
 * deterministic while interactive runtimes can choose a per-process seed.
 * Mutation is serialized by the owning runtime; this low-level table does not
 * hide a global lock.  Readers may run concurrently only after publication of
 * an immutable/frozen table snapshot. */
bool st_selector_table_init(st_selector_table_t *table,
                            st_selector_allocator_t allocator,
                            uint64_t hash_seed);
void st_selector_table_destroy(st_selector_table_t *table);
bool st_selector_table_freeze(st_selector_table_t *table);
bool st_selector_table_is_frozen(const st_selector_table_t *table);

/* Builds the canonical selector snapshot for an ordered image/application
 * program.  Method selectors are interned first in unit and lexical order,
 * followed by every message selector in AST order.  The two-pass ordering
 * keeps image method identities stable while still representing sends for
 * which no method is implemented, as required by doesNotUnderstand:.
 *
 * Construction is transactional: `table` must be zero-initialized, and it
 * remains zero-initialized on failure.  On success it owns a frozen snapshot
 * which must be destroyed with st_selector_table_destroy(). */
st_selector_status_t st_selector_table_build_for_units(
    st_selector_table_t *table,
    const st_ast_unit_t *const *units,
    size_t unit_count,
    st_selector_allocator_t allocator,
    uint64_t hash_seed);

/* Intern a syntactically valid method selector.  On allocation failure the
 * table is unchanged and the same operation may be retried. */
st_selector_status_t st_selector_intern(st_selector_table_t *table,
                                        const void *bytes, size_t length,
                                        st_selector_id_t *id_out);

bool st_selector_lookup(const st_selector_table_t *table,
                        const void *bytes, size_t length,
                        st_selector_id_t *id_out);
/* Before freeze, a returned descriptor pointer is invalidated by the next
 * successful intern that grows the descriptor vector. IDs remain stable. */
const st_selector_t *st_selector_get(const st_selector_table_t *table,
                                     st_selector_id_t id);
size_t st_selector_count(const st_selector_table_t *table);
st_selector_status_t st_selector_table_status(const st_selector_table_t *table);
const char *st_selector_status_string(st_selector_status_t status);

#endif
