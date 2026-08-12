#ifndef ANVIL_SMALLTALK_SYMBOL_INTERN_H
#define ANVIL_SMALLTALK_SYMBOL_INTERN_H

#include "st_image_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_SYMBOL_INTERN_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_SYMBOL_INTERN_OK = 0,
    ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT,
    ST_SYMBOL_INTERN_ERR_INVALID_STATE,
    ST_SYMBOL_INTERN_ERR_INVALID_DESCRIPTOR,
    ST_SYMBOL_INTERN_ERR_INVALID_VALUE,
    ST_SYMBOL_INTERN_ERR_TYPE_MISMATCH,
    ST_SYMBOL_INTERN_ERR_NOT_MEMBER,
    ST_SYMBOL_INTERN_ERR_BAD_OBJECT,
    ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY,
    ST_SYMBOL_INTERN_ERR_OVERFLOW,
    ST_SYMBOL_INTERN_ERR_CONFLICT
} st_symbol_intern_status_t;

typedef uint64_t (*st_symbol_hash_fn)(
    void *user, const st_object_view_t *authenticated_string);

typedef struct {
    st_image_runtime_t *image;
    uint32_t string_class_id;
    uint32_t string_uint8_shape_id;
    uint32_t string_uint16_shape_id;
    uint32_t string_uint32_shape_id;
    uint32_t symbol_class_id;
    uint32_t symbol_uint8_shape_id;
    uint32_t symbol_uint16_shape_id;
    uint32_t symbol_uint32_shape_id;
    /* Zero selects 16. A nonzero capacity must be a power of two >= 4. */
    size_t initial_table_capacity;
    st_runtime_allocator_t allocator;
    /* NULL selects the width-independent production hash. A custom trusted
     * hash must return equal hashes for equal scalar sequences regardless of
     * physical width; it exists primarily for deterministic collision tests.
     * Equality itself is always verified scalar-by-scalar. */
    st_symbol_hash_fn hash;
    void *hash_user;
} st_symbol_intern_options_t;

typedef struct st_symbol_intern_state st_symbol_intern_state_t;
typedef struct st_symbol_intern_context {
    uint32_t abi_version;
    bool initialized;
    st_image_runtime_t *image;
    st_image_root_provider_t root_provider;
    st_symbol_intern_state_t *state;
} st_symbol_intern_context_t;

typedef struct {
    const char *bytes;
    size_t length;
} st_symbol_utf8_t;

typedef struct st_symbol_intern_batch_state st_symbol_intern_batch_state_t;
typedef struct {
    st_symbol_intern_batch_state_t *state;
} st_symbol_intern_batch_t;

/* The context is attached to exactly one live image and is its dynamic Symbol
 * root provider. Heap mutation, interning, collection and destruction follow
 * the image heap's existing externally-serialized STW contract. */
st_symbol_intern_status_t st_symbol_intern_context_init(
    st_symbol_intern_context_t *context,
    const st_symbol_intern_options_t *options);
void st_symbol_intern_context_destroy(st_symbol_intern_context_t *context);

/* Interns the authenticated Unicode scalar sequence of an exact configured
 * String or Symbol. Equal sequences, regardless of UINT8/16/32 source width,
 * return the identical immutable, minimally-wide Symbol object. result_out
 * remains ST_VALUE_INVALID on every failure. */
st_symbol_intern_status_t st_symbol_intern(
    st_symbol_intern_context_t *context, st_value_t string,
    st_value_t *result_out);

/* Transactional bulk interning for image bootstrap. `prepare` validates UTF-8,
 * allocates every missing immutable Symbol, and constructs complete private
 * entry/table snapshots without changing the live interner. The embedding
 * must externally serialize the context and must not collect its heap until
 * the batch is committed or destroyed: newly allocated candidate Symbols are
 * deliberately not roots yet. Existing and duplicate spellings resolve to
 * the same value in the dense input-order result span.
 *
 * Commit performs no allocation. With the serialization contract respected it
 * atomically replaces the interner snapshots and makes every returned value a
 * canonical image root. A conflict leaves both snapshots unchanged. Destroy
 * aborts an uncommitted batch or releases its result storage after commit;
 * heap allocations from an aborted prepare remain unreachable and GC-owned. */
void st_symbol_intern_batch_init(st_symbol_intern_batch_t *batch);
st_symbol_intern_status_t st_symbol_intern_batch_prepare_utf8(
    st_symbol_intern_batch_t *batch, st_symbol_intern_context_t *context,
    const st_symbol_utf8_t *spellings, size_t spelling_count);
const st_value_t *st_symbol_intern_batch_values(
    const st_symbol_intern_batch_t *batch, size_t *count_out);
st_symbol_intern_status_t st_symbol_intern_batch_commit(
    st_symbol_intern_batch_t *batch);
void st_symbol_intern_batch_destroy(st_symbol_intern_batch_t *batch);

size_t st_symbol_intern_count(const st_symbol_intern_context_t *context);
size_t st_symbol_intern_table_capacity(
    const st_symbol_intern_context_t *context);

/* Generic runtime-symbol ABI consumed by AOT primitive lowering. */
uint32_t st_aot_string_as_symbol_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);

const char *st_symbol_intern_status_string(st_symbol_intern_status_t status);

#ifdef __cplusplus
}
#endif

#endif
