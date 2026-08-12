#ifndef ANVIL_SMALLTALK_HEAP_H
#define ANVIL_SMALLTALK_HEAP_H

#include "st_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_HEAP_OK = 0,
    ST_HEAP_ERR_INVALID_ARGUMENT,
    ST_HEAP_ERR_INVALID_DESCRIPTOR,
    ST_HEAP_ERR_OUT_OF_MEMORY,
    ST_HEAP_ERR_OVERFLOW,
    ST_HEAP_ERR_BAD_ALIGNMENT,
    ST_HEAP_ERR_BAD_EXTENT,
    ST_HEAP_ERR_BAD_OBJECT,
    ST_HEAP_ERR_NOT_OBJECT,
    ST_HEAP_ERR_NOT_MEMBER,
    ST_HEAP_ERR_BAD_SLOT,
    ST_HEAP_ERR_IMMUTABLE,
    ST_HEAP_ERR_INVALID_ROOT,
    ST_HEAP_ERR_DANGLING_REFERENCE,
    ST_HEAP_ERR_INVALID_FRAME,
    ST_HEAP_ERR_FRAME_CYCLE,
    ST_HEAP_ERR_RECLAIM_PROTOCOL,
    ST_HEAP_ERR_CONFLICT
} st_heap_status_t;

typedef struct st_heap_state st_heap_state_t;

/* Zero-initialize before st_heap_init. The state is intentionally opaque: its
 * exact-base allocation registry is the sole provenance authority. */
typedef struct {
    st_heap_state_t *state;
} st_heap_t;

typedef struct {
    size_t objects_before;
    size_t marked_objects;
    size_t reclaimed_objects;
    size_t bytes_before;
    size_t reclaimed_bytes;
    size_t bytes_after;
    uint64_t collection_index;
} st_heap_collection_stats_t;

typedef struct {
    const st_value_t *values;
    size_t count;
} st_heap_root_set_t;

/* Optional two-phase notification for external ownership attached to exact
 * heap allocations (for example an NLR closure's retained HomeToken).
 * `prepare` runs for every unreachable object before any sweep mutation. It
 * must be pure, non-allocating, authenticate value/class/shape/extent, and
 * return a stable nonzero cookie only when commit notification is required.
 * Any prepare error aborts the collection without sweep or callbacks.
 * `commit` runs exactly once per nonzero cookie after sweep/table/stat state is
 * committed. It must be infallible, non-allocating and must not reenter heap
 * operations. */
typedef st_heap_status_t (*st_heap_reclaim_prepare_fn)(
    void *user, st_value_t exact_value, st_object_extent_t extent,
    uint32_t class_id, uint32_t shape_id, uintptr_t *cookie_out);
typedef void (*st_heap_reclaim_commit_fn)(
    void *user, st_value_t exact_value, uint32_t class_id,
    uint32_t shape_id, uintptr_t cookie);

typedef struct {
    st_heap_reclaim_prepare_fn prepare;
    st_heap_reclaim_commit_fn commit;
    void *user;
} st_heap_reclaim_observer_t;

/* Descriptors and everything they reference must have image lifetime.
 * Heap mutation and collection are stop-the-world and externally serialized.
 * No weak, finalizer, moving, or generational protocol is implied. */
st_heap_status_t st_heap_init(
    st_heap_t *heap, const st_runtime_descriptors_t *descriptors,
    st_runtime_allocator_t allocator);
/* Image restore may choose the first monotonically assigned identity. Zero is
 * reserved and rejected. The default initializer starts at one. */
st_heap_status_t st_heap_init_with_identity_seed(
    st_heap_t *heap, const st_runtime_descriptors_t *descriptors,
    st_runtime_allocator_t allocator, uint64_t first_allocation_identity);
void st_heap_destroy(st_heap_t *heap);

/* Installation/removal is externally serialized. Only one exact observer is
 * active. External contexts must reconcile their live entries, remove the
 * observer with the identical triple, and only then destroy the heap. */
st_heap_status_t st_heap_reclaim_observer_install(
    st_heap_t *heap, st_heap_reclaim_observer_t observer);
st_heap_status_t st_heap_reclaim_observer_remove(
    st_heap_t *heap, st_heap_reclaim_observer_t observer);

st_heap_status_t st_heap_allocate(
    st_heap_t *heap, uint32_t class_id, uint32_t shape_id,
    size_t indexed_length, size_t indexed_capacity,
    st_header_flags_t flags, st_value_t *value_out);

/* Exact-base registry lookup happens before st_object_validate can dereference
 * the word. Aligned interior pointers and foreign aligned pointers are not
 * authorized merely because their tag encoding looks like an object. */
st_heap_status_t st_heap_authorize(
    const st_heap_t *heap, st_value_t value, st_object_extent_t *extent_out);
st_heap_status_t st_heap_object_view(
    const st_heap_t *heap, st_value_t value, st_object_view_t *view_out);
bool st_heap_contains(const st_heap_t *heap, st_value_t value);

/* Authenticated mutation boundary for managed fixed pointer slots.  Both
 * operations prove exact heap membership and the shape bitmap before touching
 * storage.  Store additionally validates the child, rejects immutable owners,
 * and performs the old-to-young remembered barrier before publication. */
st_heap_status_t st_heap_fixed_reference_load(
    const st_heap_t *heap, st_value_t owner, size_t index,
    st_value_t *value_out);
st_heap_status_t st_heap_fixed_reference_store(
    st_heap_t *heap, st_value_t owner, size_t index, st_value_t value);

/* Authenticated mutation boundary for ST_INDEXED_VALUES elements. Indices are
 * checked against the logical indexed length, rather than spare allocation
 * capacity. Store has the same child authentication, immutability check and
 * remembered-set publication protocol as fixed-reference store. */
st_heap_status_t st_heap_indexed_reference_load(
    const st_heap_t *heap, st_value_t owner, size_t index,
    st_value_t *value_out);
st_heap_status_t st_heap_indexed_reference_store(
    st_heap_t *heap, st_value_t owner, size_t index, st_value_t value);

/* Lazy, stable identity hash derived solely from the monotonic allocation
 * identity, never from pointer bits. The result is in SmallInteger range. */
st_heap_status_t st_heap_identity_hash(
    st_heap_t *heap, st_value_t value, uint64_t *identity_hash_out);

size_t st_heap_object_count(const st_heap_t *heap);
size_t st_heap_allocated_bytes(const st_heap_t *heap);
uint64_t st_heap_collection_count(const st_heap_t *heap);
/* Borrowed immutable image descriptor set used by this heap. */
const st_runtime_descriptors_t *st_heap_descriptors(const st_heap_t *heap);

/*
 * Precise STW collection. Frames themselves and immutable method metadata are
 * trusted runtime/AOT storage; their contents are nevertheless validated:
 * acyclic caller chain, exact safepoint map, arity, receiver, argv, and live
 * shadow-root bits. A frame's root_count is its physical
 * frame_root_capacity; a safepoint map may describe a shorter logical prefix.
 * A chain containing a home token or CAN_UNWIND/NLR metadata must carry an
 * initialized st_aot_thread_t with the exact authenticated control sidecar;
 * its pending NLR, transient leave value, and ARMED/RUNNING ensure vectors are
 * validated and marked in addition to ordinary frame roots.
 * `global_roots` supplies image/application roots.
 *
 * Mark state is phase-local. Any OOM, malformed frame/root/object, interior
 * pointer, or nonmember reference discards it and leaves the heap unchanged.
 */
st_heap_status_t st_heap_collect(
    st_heap_t *heap, const StFrame *top_frame,
    const st_value_t *global_roots, size_t global_root_count,
    st_heap_collection_stats_t *stats_out);

/* Same precise collector with a scatter/gather root interface. Root spans
 * are borrowed only for the call and visited in set/index order. This avoids
 * allocating and copying growing image-owned registries at every safepoint. */
st_heap_status_t st_heap_collect_root_sets(
    st_heap_t *heap, const StFrame *top_frame,
    const st_heap_root_set_t *root_sets, size_t root_set_count,
    st_heap_collection_stats_t *stats_out);

const char *st_heap_status_string(st_heap_status_t status);

#ifdef __cplusplus
}
#endif

#endif
