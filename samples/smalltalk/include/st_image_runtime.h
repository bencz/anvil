#ifndef ANVIL_SMALLTALK_IMAGE_RUNTIME_H
#define ANVIL_SMALLTALK_IMAGE_RUNTIME_H

#include "st_heap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_IMAGE_RUNTIME_ABI_VERSION UINT32_C(2)
#define ST_IMAGE_ROOT_PROVIDER_ABI_VERSION UINT32_C(1)

/* Reserved semantic identity for the image-initialized stdout stream.  Its
 * dense runtime index is assigned by the application AOT plan and is never
 * inferred from a manifest position or source-level name at runtime. */
#define ST_IMAGE_EXTERNAL_ID_TRANSCRIPT UINT32_C(0xf0000001)

typedef enum {
    ST_IMAGE_RUNTIME_OK = 0,
    ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT,
    ST_IMAGE_RUNTIME_ERR_INVALID_STATE,
    ST_IMAGE_RUNTIME_ERR_INVALID_DESCRIPTOR,
    ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE,
    ST_IMAGE_RUNTIME_ERR_DUPLICATE_ID,
    ST_IMAGE_RUNTIME_ERR_INVALID_VALUE,
    ST_IMAGE_RUNTIME_ERR_NOT_MEMBER,
    ST_IMAGE_RUNTIME_ERR_UNINITIALIZED,
    ST_IMAGE_RUNTIME_ERR_CONFLICT,
    ST_IMAGE_RUNTIME_ERR_OVERFLOW,
    ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY,
    ST_IMAGE_RUNTIME_ERR_HEAP,
    ST_IMAGE_RUNTIME_ERR_VISITOR_ABORTED
} st_image_runtime_status_t;

typedef st_image_runtime_status_t (*st_image_root_span_fn)(
    void *owner, const st_value_t **roots_out, size_t *root_count_out);

typedef struct {
    uint32_t abi_version;
    void *owner;
    st_image_root_span_fn roots;
} st_image_root_provider_t;

typedef struct {
    uint32_t id;
    /* ST_VALUE_INVALID reserves a dense slot for later bootstrap. */
    st_value_t value;
} st_image_runtime_entry_t;

typedef struct {
    uint32_t class_id;
    uint32_t shape_id;
} st_image_string_layout_t;

typedef struct {
    uint32_t class_id;
    uint32_t shape_id;
    size_t descriptor_fixed_word_index;
} st_image_external_stream_layout_t;

typedef struct {
    const st_runtime_descriptors_t *descriptors;
    /* Exactly one heap mode is selected. A non-NULL borrowed_heap remains
     * owned by the caller; otherwise the sidecar constructs and owns a heap
     * using heap_allocator. */
    st_heap_t *borrowed_heap;
    st_runtime_allocator_t heap_allocator;
    st_runtime_allocator_t table_allocator;
    const st_image_runtime_entry_t *globals;
    size_t global_count;
    const st_image_runtime_entry_t *literals;
    size_t literal_count;
    st_image_string_layout_t string_layout;
    st_image_external_stream_layout_t external_stream_layout;
} st_image_runtime_options_t;

typedef struct st_image_runtime_state st_image_runtime_state_t;
typedef struct st_image_runtime {
    uint32_t abi_version;
    bool initialized;
    bool owns_heap;
    st_heap_t *heap;
    st_image_runtime_state_t *state;
} st_image_runtime_t;

/* Initialization is transactional. Entry IDs must each be a permutation of
 * 1..count. Every present object is authenticated as an exact live base in
 * the selected heap; every immediate must have a valid tagged encoding.
 * ST_VALUE_INVALID creates an explicitly uninitialized slot. */
st_image_runtime_status_t st_image_runtime_init(
    st_image_runtime_t *runtime, const st_image_runtime_options_t *options);
void st_image_runtime_destroy(st_image_runtime_t *runtime);

/* Attaches image-lifetime dynamic root providers. Providers and returned root
 * spans are borrowed and must remain stable while a visit/collection is in
 * progress. The provider vector grows transactionally. Mutation, GC,
 * attachment and detachment are externally serialized with the heap. */
bool st_image_runtime_root_provider_attach(
    st_image_runtime_t *runtime, const st_image_root_provider_t *provider);
bool st_image_runtime_root_provider_detach(
    st_image_runtime_t *runtime, const st_image_root_provider_t *provider);
const st_image_root_provider_t *st_image_runtime_root_provider(
    const st_image_runtime_t *runtime);
bool st_image_runtime_root_provider_contains(
    const st_image_runtime_t *runtime,
    const st_image_root_provider_t *provider);
const st_image_root_provider_t *st_image_runtime_root_provider_find(
    const st_image_runtime_t *runtime, st_image_root_span_fn roots);

/* Generated-code ABI. `index` is zero based. The frame must belong to a ready
 * AOT thread whose image field is this exact sidecar. On every failure the
 * result remains ST_VALUE_INVALID; nil is never a failure substitute. */
st_image_runtime_status_t st_image_runtime_global_load(
    StFrame *frame, uint32_t index, st_value_t *result_out);
st_image_runtime_status_t st_image_runtime_literal_load(
    StFrame *frame, uint32_t index, st_value_t *result_out);

/* Generated code reaches this only after a non-OK load. It cannot continue
 * with ST_VALUE_INVALID (or silently substitute nil). StValue is retained as
 * the IR-level result type until Anvil exposes a noreturn terminator. */
_Noreturn st_value_t st_aot_image_runtime_contract_violation(
    st_image_runtime_status_t status, const StFrame *frame);

/* Bootstrap mutators fill only a reserved, uninitialized dense slot and are
 * transactional with respect to publication. String bytes are copied into a
 * newly allocated immutable byte-indexed object. Transcript is an instance
 * of the explicitly configured ExternalStream layout whose configured fixed
 * descriptor ivar receives the authenticated SmallInteger 1 (stdout). No
 * class/global names or manifest positions are inferred. */
st_image_runtime_status_t st_image_runtime_bootstrap_string_literal(
    st_image_runtime_t *runtime, uint32_t literal_index,
    const void *bytes, size_t byte_count, st_value_t *result_out);

/* Publishes an already-created, authenticated value into one reserved global
 * slot. This is used by the managed bootstrap to expose class objects after
 * it has built the complete class/metaclass graph. The value is validated as
 * an exact member of this image heap (or a valid immediate) before the slot is
 * made visible to generated code. */
st_image_runtime_status_t st_image_runtime_bootstrap_global_value(
    st_image_runtime_t *runtime, uint32_t global_index, st_value_t value);

st_image_runtime_status_t st_image_runtime_bootstrap_transcript(
    st_image_runtime_t *runtime, uint32_t global_index,
    st_value_t *result_out);

typedef bool (*st_image_runtime_root_visitor_fn)(
    void *user, const st_value_t *root_slot);

/* Enumerates initialized globals followed by initialized literals in stable
 * dense-index order. Each root is re-authenticated before it is exposed. */
st_image_runtime_status_t st_image_runtime_visit_roots(
    const st_image_runtime_t *runtime,
    st_image_runtime_root_visitor_fn visitor, void *user,
    size_t *visited_out);

/* Precise collection over the image's complete dense root vector plus the
 * supplied frame chain. Uninitialized slots are stored as nil roots and are
 * therefore safe but are not loadable or reported by visit_roots. */
st_image_runtime_status_t st_image_runtime_collect(
    st_image_runtime_t *runtime, const StFrame *top_frame,
    st_heap_collection_stats_t *stats_out);

st_heap_t *st_image_runtime_heap(st_image_runtime_t *runtime);
const st_heap_t *st_image_runtime_heap_const(
    const st_image_runtime_t *runtime);
const char *st_image_runtime_status_string(st_image_runtime_status_t status);

#ifdef __cplusplus
}
#endif

#endif
