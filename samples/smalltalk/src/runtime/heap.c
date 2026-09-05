#include "../platform/runtime.h"
#include "st_heap.h"
#include "st_control_roots.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ST_HEAP_MAGIC UINT64_C(0x5354484541503031)
#define ST_HEAP_INITIAL_ENTRY_CAPACITY 8u
#define ST_HEAP_INITIAL_TABLE_CAPACITY 16u
#define ST_HEAP_BITS_PER_WORD 64u

typedef struct {
    st_value_t value;
    st_object_extent_t extent;
    uint64_t allocation_identity;
    /* Zero means not computed; stored hash is this value minus one. */
    uint64_t identity_hash_plus_one;
} st_heap_entry_t;

struct st_heap_state {
    uint64_t magic;
    const st_runtime_descriptors_t *descriptors;
    st_runtime_allocator_t allocator;
    st_heap_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    size_t entries_block_size;
    size_t *table;
    size_t table_capacity;
    size_t table_block_size;
    size_t allocated_bytes;
    uint64_t collection_count;
    uint64_t next_allocation_identity;
    bool allocation_identity_exhausted;
    st_heap_reclaim_observer_t reclaim_observer;
    size_t state_block_size;
};

typedef struct {
    st_value_t value;
    uint32_t class_id;
    uint32_t shape_id;
    uintptr_t cookie;
} reclaim_record_t;

typedef struct {
    uint64_t *marks;
    size_t *worklist;
    size_t worklist_count;
    size_t marked_count;
    st_heap_state_t *state;
    st_heap_status_t status;
} mark_context_t;

static bool add_size(size_t left, size_t right, size_t *result_out)
{
    if (left > SIZE_MAX - right) return false;
    *result_out = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) return false;
    *result_out = left * right;
    return true;
}

static bool is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool round_up(size_t value, size_t alignment, size_t *result_out)
{
    size_t mask;
    if (!is_power_of_two(alignment)) return false;
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) return false;
    *result_out = (value + mask) & ~mask;
    return true;
}

static void *default_allocate(void *user, size_t alignment, size_t size)
{
    (void)user;
    return st_runtime_platform.aligned_alloc(alignment, size);
}

static void default_deallocate(void *user, void *pointer, size_t alignment,
                               size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    st_runtime_platform.aligned_free(pointer);
}

static bool normalize_allocator(st_runtime_allocator_t input,
                                st_runtime_allocator_t *output)
{
    if (!output || ((input.allocate == NULL) !=
                    (input.deallocate == NULL)))
        return false;
    if (!input.allocate) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static void *allocate_block(st_runtime_allocator_t allocator,
                            size_t alignment, size_t requested_size,
                            size_t *block_size_out,
                            bool *bad_alignment_out,
                            bool *size_overflow_out)
{
    size_t block_size;
    void *result;
    if (bad_alignment_out) *bad_alignment_out = false;
    if (size_overflow_out) *size_overflow_out = false;
    if (!block_size_out || requested_size == 0u)
        return NULL;
    if (!round_up(requested_size, alignment, &block_size)) {
        if (size_overflow_out) *size_overflow_out = true;
        return NULL;
    }
    result = allocator.allocate(allocator.user, alignment, block_size);
    if (!result) return NULL;
    if (((uintptr_t)result & (alignment - 1u)) != 0u) {
        if (bad_alignment_out) *bad_alignment_out = true;
        allocator.deallocate(allocator.user, result, alignment, block_size);
        return NULL;
    }
    *block_size_out = block_size;
    return result;
}

static void deallocate_block(st_runtime_allocator_t allocator, void *pointer,
                             size_t alignment, size_t block_size)
{
    if (pointer)
        allocator.deallocate(allocator.user, pointer, alignment, block_size);
}

static st_heap_state_t *heap_state(const st_heap_t *heap)
{
    if (!heap || !heap->state || heap->state->magic != ST_HEAP_MAGIC)
        return NULL;
    return heap->state;
}

static uint64_t hash_pointer(uintptr_t pointer)
{
    uint64_t value = (uint64_t)(pointer >> ST_VALUE_TAG_BITS);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static void table_insert(size_t *table, size_t table_capacity,
                         const st_heap_entry_t *entries, size_t entry_index)
{
    size_t slot = (size_t)hash_pointer(
        (uintptr_t)entries[entry_index].extent.base) & (table_capacity - 1u);
    while (table[slot] != 0u)
        slot = (slot + 1u) & (table_capacity - 1u);
    table[slot] = entry_index + 1u;
}

static void table_rebuild(st_heap_state_t *state)
{
    size_t index;
    memset(state->table, 0, state->table_capacity * sizeof(*state->table));
    for (index = 0u; index < state->entry_count; ++index)
        table_insert(state->table, state->table_capacity,
                     state->entries, index);
}

static bool find_entry(const st_heap_state_t *state, st_value_t value,
                       size_t *index_out)
{
    size_t slot;
    size_t probes;
    if (state->table_capacity == 0u ||
        st_value_kind(value) != ST_VALUE_OBJECT)
        return false;
    slot = (size_t)hash_pointer((uintptr_t)value) &
           (state->table_capacity - 1u);
    for (probes = 0u; probes < state->table_capacity; ++probes) {
        size_t encoded_index = state->table[slot];
        size_t index;
        if (encoded_index == 0u) return false;
        index = encoded_index - 1u;
        if (index < state->entry_count &&
            state->entries[index].value == value) {
            if (index_out) *index_out = index;
            return true;
        }
        slot = (slot + 1u) & (state->table_capacity - 1u);
    }
    return false;
}

static st_heap_status_t ensure_insert_capacity(st_heap_state_t *state)
{
    size_t needed_count;
    size_t new_entry_capacity = state->entry_capacity;
    size_t new_table_capacity = state->table_capacity;
    st_heap_entry_t *new_entries = NULL;
    size_t *new_table = NULL;
    size_t new_entries_size = 0u;
    size_t new_table_size = 0u;
    size_t requested;
    size_t index;
    bool bad_alignment = false;
    bool size_overflow = false;
    if (!add_size(state->entry_count, 1u, &needed_count))
        return ST_HEAP_ERR_OVERFLOW;
    if (needed_count > new_entry_capacity) {
        new_entry_capacity = new_entry_capacity == 0u
            ? ST_HEAP_INITIAL_ENTRY_CAPACITY : new_entry_capacity;
        while (new_entry_capacity < needed_count) {
            if (new_entry_capacity > SIZE_MAX / 2u)
                return ST_HEAP_ERR_OVERFLOW;
            new_entry_capacity *= 2u;
        }
    }
    if (new_table_capacity == 0u)
        new_table_capacity = ST_HEAP_INITIAL_TABLE_CAPACITY;
    while (needed_count > new_table_capacity - new_table_capacity / 4u) {
        if (new_table_capacity > SIZE_MAX / 2u)
            return ST_HEAP_ERR_OVERFLOW;
        new_table_capacity *= 2u;
    }

    if (new_entry_capacity != state->entry_capacity) {
        if (!multiply_size(new_entry_capacity, sizeof(*new_entries),
                           &requested))
            return ST_HEAP_ERR_OVERFLOW;
        new_entries = allocate_block(state->allocator,
                                     _Alignof(st_heap_entry_t), requested,
                                     &new_entries_size, &bad_alignment,
                                     &size_overflow);
        if (!new_entries)
            return size_overflow ? ST_HEAP_ERR_OVERFLOW
                : bad_alignment ? ST_HEAP_ERR_BAD_ALIGNMENT
                                : ST_HEAP_ERR_OUT_OF_MEMORY;
        if (state->entry_count != 0u)
            memcpy(new_entries, state->entries,
                   state->entry_count * sizeof(*new_entries));
    }
    if (new_table_capacity != state->table_capacity) {
        if (!multiply_size(new_table_capacity, sizeof(*new_table),
                           &requested)) {
            deallocate_block(state->allocator, new_entries,
                             _Alignof(st_heap_entry_t), new_entries_size);
            return ST_HEAP_ERR_OVERFLOW;
        }
        new_table = allocate_block(state->allocator, _Alignof(size_t),
                                   requested, &new_table_size,
                                   &bad_alignment, &size_overflow);
        if (!new_table) {
            deallocate_block(state->allocator, new_entries,
                             _Alignof(st_heap_entry_t), new_entries_size);
            return size_overflow ? ST_HEAP_ERR_OVERFLOW
                : bad_alignment ? ST_HEAP_ERR_BAD_ALIGNMENT
                                : ST_HEAP_ERR_OUT_OF_MEMORY;
        }
        memset(new_table, 0, new_table_capacity * sizeof(*new_table));
        for (index = 0u; index < state->entry_count; ++index)
            table_insert(new_table, new_table_capacity,
                         new_entries ? new_entries : state->entries, index);
    }

    if (new_entries) {
        deallocate_block(state->allocator, state->entries,
                         _Alignof(st_heap_entry_t), state->entries_block_size);
        state->entries = new_entries;
        state->entry_capacity = new_entry_capacity;
        state->entries_block_size = new_entries_size;
    }
    if (new_table) {
        deallocate_block(state->allocator, state->table,
                         _Alignof(size_t), state->table_block_size);
        state->table = new_table;
        state->table_capacity = new_table_capacity;
        state->table_block_size = new_table_size;
    }
    return ST_HEAP_OK;
}

static st_heap_status_t map_runtime_status(st_runtime_status_t status)
{
    switch (status) {
    case ST_RUNTIME_OK: return ST_HEAP_OK;
    case ST_RUNTIME_ERR_INVALID_ARGUMENT: return ST_HEAP_ERR_INVALID_ARGUMENT;
    case ST_RUNTIME_ERR_INVALID_DESCRIPTOR:
    case ST_RUNTIME_ERR_ID_OUT_OF_RANGE:
    case ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE:
        return ST_HEAP_ERR_INVALID_DESCRIPTOR;
    case ST_RUNTIME_ERR_OVERFLOW: return ST_HEAP_ERR_OVERFLOW;
    case ST_RUNTIME_ERR_OUT_OF_MEMORY: return ST_HEAP_ERR_OUT_OF_MEMORY;
    case ST_RUNTIME_ERR_BAD_ALIGNMENT: return ST_HEAP_ERR_BAD_ALIGNMENT;
    case ST_RUNTIME_ERR_BAD_EXTENT: return ST_HEAP_ERR_BAD_EXTENT;
    case ST_RUNTIME_ERR_BAD_OBJECT:
    case ST_RUNTIME_ERR_IMMUTABLE:
    case ST_RUNTIME_ERR_CONFLICT:
    case ST_RUNTIME_ERR_VISITOR_ABORTED:
        return ST_HEAP_ERR_BAD_OBJECT;
    default: return ST_HEAP_ERR_BAD_OBJECT;
    }
}

st_heap_status_t st_heap_init_with_identity_seed(
    st_heap_t *heap, const st_runtime_descriptors_t *descriptors,
    st_runtime_allocator_t allocator, uint64_t first_allocation_identity)
{
    st_heap_state_t *state;
    st_runtime_allocator_t normalized;
    size_t block_size = 0u;
    bool bad_alignment = false;
    bool size_overflow = false;
    if (!heap || heap->state || first_allocation_identity == 0u ||
        !normalize_allocator(allocator, &normalized))
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (st_runtime_descriptors_validate(descriptors) != ST_RUNTIME_OK)
        return ST_HEAP_ERR_INVALID_DESCRIPTOR;
    state = allocate_block(normalized, _Alignof(max_align_t), sizeof(*state),
                           &block_size, &bad_alignment, &size_overflow);
    if (!state) return size_overflow ? ST_HEAP_ERR_OVERFLOW
        : bad_alignment ? ST_HEAP_ERR_BAD_ALIGNMENT
                        : ST_HEAP_ERR_OUT_OF_MEMORY;
    memset(state, 0, sizeof(*state));
    state->magic = ST_HEAP_MAGIC;
    state->descriptors = descriptors;
    state->allocator = normalized;
    state->next_allocation_identity = first_allocation_identity;
    state->state_block_size = block_size;
    heap->state = state;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_init(
    st_heap_t *heap, const st_runtime_descriptors_t *descriptors,
    st_runtime_allocator_t allocator)
{
    return st_heap_init_with_identity_seed(heap, descriptors, allocator,
                                           UINT64_C(1));
}

void st_heap_destroy(st_heap_t *heap)
{
    st_heap_state_t *state = heap_state(heap);
    st_runtime_allocator_t allocator;
    size_t state_block_size;
    size_t index;
    if (!state) return;
    for (index = 0u; index < state->entry_count; ++index)
        st_object_deallocate(state->allocator, state->entries[index].extent);
    deallocate_block(state->allocator, state->entries,
                     _Alignof(st_heap_entry_t), state->entries_block_size);
    deallocate_block(state->allocator, state->table, _Alignof(size_t),
                     state->table_block_size);
    allocator = state->allocator;
    state_block_size = state->state_block_size;
    state->magic = 0u;
    deallocate_block(allocator, state, _Alignof(max_align_t),
                     state_block_size);
    heap->state = NULL;
}

static bool observer_valid(st_heap_reclaim_observer_t observer)
{
    return observer.prepare != NULL && observer.commit != NULL;
}

static bool observer_equal(st_heap_reclaim_observer_t left,
                           st_heap_reclaim_observer_t right)
{
    return left.prepare == right.prepare && left.commit == right.commit &&
        left.user == right.user;
}

st_heap_status_t st_heap_reclaim_observer_install(
    st_heap_t *heap, st_heap_reclaim_observer_t observer)
{
    st_heap_state_t *state = heap_state(heap);
    if (!state || !observer_valid(observer))
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (observer_valid(state->reclaim_observer))
        return ST_HEAP_ERR_CONFLICT;
    state->reclaim_observer = observer;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_reclaim_observer_remove(
    st_heap_t *heap, st_heap_reclaim_observer_t observer)
{
    st_heap_state_t *state = heap_state(heap);
    if (!state || !observer_valid(observer))
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (!observer_equal(state->reclaim_observer, observer))
        return ST_HEAP_ERR_CONFLICT;
    state->reclaim_observer = (st_heap_reclaim_observer_t){0};
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_allocate(
    st_heap_t *heap, uint32_t class_id, uint32_t shape_id,
    size_t indexed_length, size_t indexed_capacity,
    st_header_flags_t flags, st_value_t *value_out)
{
    st_heap_state_t *state = heap_state(heap);
    const StShapeDescriptor *shape;
    st_object_extent_t extent = {0};
    st_value_t value = 0;
    st_runtime_status_t runtime_status;
    st_heap_status_t status;
    size_t expected_size;
    size_t new_total;
    uint64_t allocation_identity;
    if (value_out) *value_out = 0;
    if (!state || !value_out) return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (state->allocation_identity_exhausted)
        return ST_HEAP_ERR_OVERFLOW;
    allocation_identity = state->next_allocation_identity;
    shape = st_runtime_shape(state->descriptors, shape_id);
    if (!shape || shape->class_id != class_id ||
        !st_shape_descriptor_is_valid(shape))
        return ST_HEAP_ERR_INVALID_DESCRIPTOR;
    if (!st_shape_descriptor_extent(shape, indexed_capacity, &expected_size) ||
        !add_size(state->allocated_bytes, expected_size, &new_total))
        return ST_HEAP_ERR_OVERFLOW;
    status = ensure_insert_capacity(state);
    if (status != ST_HEAP_OK) return status;
    runtime_status = st_object_allocate(
        state->descriptors, class_id, shape_id, indexed_length,
        indexed_capacity, flags, state->allocator, &extent, &value);
    if (runtime_status != ST_RUNTIME_OK)
        return map_runtime_status(runtime_status);
    if (extent.byte_size != expected_size || find_entry(state, value, NULL)) {
        st_object_deallocate(state->allocator, extent);
        return ST_HEAP_ERR_BAD_EXTENT;
    }
    state->entries[state->entry_count] = (st_heap_entry_t){
        .value = value,
        .extent = extent,
        .allocation_identity = allocation_identity
    };
    table_insert(state->table, state->table_capacity, state->entries,
                 state->entry_count);
    ++state->entry_count;
    state->allocated_bytes = new_total;
    if (allocation_identity == UINT64_MAX)
        state->allocation_identity_exhausted = true;
    else
        state->next_allocation_identity = allocation_identity + UINT64_C(1);
    *value_out = value;
    return ST_HEAP_OK;
}

static uint64_t identity_mix(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

st_heap_status_t st_heap_identity_hash(
    st_heap_t *heap, st_value_t value, uint64_t *identity_hash_out)
{
    st_heap_state_t *state = heap_state(heap);
    st_heap_entry_t *entry;
    size_t index;
    uint64_t hash;
    if (identity_hash_out) *identity_hash_out = 0u;
    if (!state || !identity_hash_out) return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (!st_value_has_valid_encoding(value)) return ST_HEAP_ERR_BAD_OBJECT;
    if (st_value_kind(value) != ST_VALUE_OBJECT)
        return ST_HEAP_ERR_NOT_OBJECT;
    if (!find_entry(state, value, &index)) return ST_HEAP_ERR_NOT_MEMBER;
    entry = &state->entries[index];
    if (entry->identity_hash_plus_one == 0u) {
        hash = identity_mix(entry->allocation_identity) &
               (uint64_t)ST_SMALL_INTEGER_MAX;
        entry->identity_hash_plus_one = hash + UINT64_C(1);
    }
    *identity_hash_out = entry->identity_hash_plus_one - UINT64_C(1);
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_authorize(
    const st_heap_t *heap, st_value_t value, st_object_extent_t *extent_out)
{
    st_heap_state_t *state = heap_state(heap);
    size_t index;
    if (extent_out) *extent_out = (st_object_extent_t){0};
    if (!state || !extent_out) return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (!st_value_has_valid_encoding(value)) return ST_HEAP_ERR_BAD_OBJECT;
    if (st_value_kind(value) != ST_VALUE_OBJECT)
        return ST_HEAP_ERR_NOT_OBJECT;
    if (!find_entry(state, value, &index)) return ST_HEAP_ERR_NOT_MEMBER;
    *extent_out = state->entries[index].extent;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_object_view(
    const st_heap_t *heap, st_value_t value, st_object_view_t *view_out)
{
    st_heap_state_t *state = heap_state(heap);
    st_object_extent_t extent;
    st_heap_status_t status;
    st_runtime_status_t runtime_status;
    if (view_out) memset(view_out, 0, sizeof(*view_out));
    if (!state || !view_out) return ST_HEAP_ERR_INVALID_ARGUMENT;
    status = st_heap_authorize(heap, value, &extent);
    if (status != ST_HEAP_OK) return status;
    runtime_status = st_object_validate(state->descriptors, value, extent,
                                        view_out);
    return map_runtime_status(runtime_status);
}

static bool fixed_pointer_slot(const StShapeDescriptor *shape, size_t index)
{
    return shape && index < shape->fixed_word_count
        && shape->fixed_pointer_bitmap
        && index / 64u < shape->fixed_pointer_bitmap_word_count
        && (shape->fixed_pointer_bitmap[index / 64u]
            & (UINT64_C(1) << (index % 64u))) != 0u;
}

static st_heap_status_t authenticate_loaded_reference(
    const st_heap_state_t *state, st_value_t value)
{
    if (!st_value_has_valid_encoding(value)) {
        return ST_HEAP_ERR_BAD_OBJECT;
    }
    if (st_value_kind(value) == ST_VALUE_OBJECT
            && !find_entry(state, value, NULL)) {
        return ST_HEAP_ERR_DANGLING_REFERENCE;
    }
    return ST_HEAP_OK;
}

static st_heap_status_t reference_store(
    st_heap_t *heap, st_object_view_t *target,
    st_value_t *slot, st_value_t value)
{
    uint64_t target_header;

    if (!st_value_has_valid_encoding(value)) {
        return ST_HEAP_ERR_BAD_OBJECT;
    }
    target_header = st_object_header_load(&target->object->header);
    if ((st_object_header_flags(target_header) & ST_HEADER_IMMUTABLE) != 0u) {
        return ST_HEAP_ERR_IMMUTABLE;
    }
    if (st_value_kind(value) == ST_VALUE_OBJECT) {
        st_object_view_t child;
        st_heap_status_t status = st_heap_object_view(heap, value, &child);

        if (status != ST_HEAP_OK) {
            return status == ST_HEAP_ERR_NOT_MEMBER
                ? ST_HEAP_ERR_DANGLING_REFERENCE
                : status;
        }
        uint64_t child_header = st_object_header_load(&child.object->header);
        if (st_object_header_generation(target_header) >= ST_GC_OLD
                && st_object_header_generation(child_header) < ST_GC_OLD
                && !st_object_header_remember(&target->object->header)) {
            return ST_HEAP_ERR_CONFLICT;
        }
    }
    *slot = value;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_fixed_reference_load(
    const st_heap_t *heap, st_value_t owner, size_t index,
    st_value_t *value_out)
{
    st_heap_state_t *state = heap_state(heap);
    st_object_view_t view;
    st_heap_status_t status;
    st_value_t value;
    if (value_out) *value_out = (st_value_t)ST_VALUE_INVALID;
    if (!state || !value_out) return ST_HEAP_ERR_INVALID_ARGUMENT;
    status = st_heap_object_view(heap, owner, &view);
    if (status != ST_HEAP_OK) return status;
    if (!fixed_pointer_slot(view.shape_descriptor, index))
        return ST_HEAP_ERR_BAD_SLOT;
    value = ((const st_value_t *)view.fixed_words)[index];
    status = authenticate_loaded_reference(state, value);
    if (status != ST_HEAP_OK) {
        return status;
    }
    *value_out = value;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_fixed_reference_store(
    st_heap_t *heap, st_value_t owner, size_t index, st_value_t value)
{
    st_heap_state_t *state = heap_state(heap);
    st_object_view_t target;
    st_heap_status_t status;
    if (!state) {
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    }
    status = st_heap_object_view(heap, owner, &target);
    if (status != ST_HEAP_OK) {
        return status;
    }
    if (!fixed_pointer_slot(target.shape_descriptor, index)) {
        return ST_HEAP_ERR_BAD_SLOT;
    }
    return reference_store(
        heap, &target,
        &((st_value_t *)target.fixed_words)[index], value);
}

st_heap_status_t st_heap_indexed_reference_load(
    const st_heap_t *heap, st_value_t owner, size_t index,
    st_value_t *value_out)
{
    st_heap_state_t *state = heap_state(heap);
    st_object_view_t view;
    st_heap_status_t status;
    st_value_t value;

    if (value_out != NULL) {
        *value_out = (st_value_t)ST_VALUE_INVALID;
    }
    if (state == NULL || value_out == NULL) {
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    }
    status = st_heap_object_view(heap, owner, &view);
    if (status != ST_HEAP_OK) {
        return status;
    }
    if (view.shape_descriptor->indexed_format != ST_INDEXED_VALUES
            || index >= view.indexed_length
            || view.indexed_elements == NULL) {
        return ST_HEAP_ERR_BAD_SLOT;
    }
    value = ((const st_value_t *)view.indexed_elements)[index];
    status = authenticate_loaded_reference(state, value);
    if (status != ST_HEAP_OK) {
        return status;
    }
    *value_out = value;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_indexed_reference_store(
    st_heap_t *heap, st_value_t owner, size_t index, st_value_t value)
{
    st_heap_state_t *state = heap_state(heap);
    st_object_view_t target;
    st_heap_status_t status;

    if (state == NULL) {
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    }
    status = st_heap_object_view(heap, owner, &target);
    if (status != ST_HEAP_OK) {
        return status;
    }
    if (target.shape_descriptor->indexed_format != ST_INDEXED_VALUES
            || index >= target.indexed_length
            || target.indexed_elements == NULL) {
        return ST_HEAP_ERR_BAD_SLOT;
    }
    return reference_store(
        heap, &target,
        &((st_value_t *)target.indexed_elements)[index], value);
}

bool st_heap_contains(const st_heap_t *heap, st_value_t value)
{
    st_heap_state_t *state = heap_state(heap);
    return state && find_entry(state, value, NULL);
}

size_t st_heap_object_count(const st_heap_t *heap)
{
    st_heap_state_t *state = heap_state(heap);
    return state ? state->entry_count : 0u;
}

size_t st_heap_allocated_bytes(const st_heap_t *heap)
{
    st_heap_state_t *state = heap_state(heap);
    return state ? state->allocated_bytes : 0u;
}

uint64_t st_heap_collection_count(const st_heap_t *heap)
{
    st_heap_state_t *state = heap_state(heap);
    return state ? state->collection_count : 0u;
}

const st_runtime_descriptors_t *st_heap_descriptors(const st_heap_t *heap)
{
    st_heap_state_t *state = heap_state(heap);
    return state ? state->descriptors : NULL;
}

static const st_root_map_t *find_root_map(const StMethodDescriptor *method,
                                          uint32_t safepoint_id)
{
    size_t low = 0u;
    size_t high = method->root_map_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uint32_t candidate = method->root_maps[middle].safepoint_id;
        if (candidate < safepoint_id)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < method->root_map_count &&
           method->root_maps[low].safepoint_id == safepoint_id
        ? &method->root_maps[low] : NULL;
}

static bool bitmap_test(const uint64_t *bitmap, size_t bit)
{
    return (bitmap[bit >> 6] & (UINT64_C(1) << (bit & 63u))) != 0u;
}

static st_heap_status_t validate_root_value(const st_heap_state_t *state,
                                            st_value_t value)
{
    if (!st_value_has_valid_encoding(value))
        return ST_HEAP_ERR_INVALID_ROOT;
    if (st_value_kind(value) == ST_VALUE_OBJECT &&
        !find_entry(state, value, NULL))
        return ST_HEAP_ERR_DANGLING_REFERENCE;
    return ST_HEAP_OK;
}

typedef struct {
    const st_heap_state_t *state;
    st_heap_status_t status;
} control_validate_context_t;

static bool validate_control_root(void *user, const st_value_t *root_slot)
{
    control_validate_context_t *context = user;
    if (root_slot == NULL) {
        context->status = ST_HEAP_ERR_INVALID_ROOT;
        return false;
    }
    context->status = validate_root_value(context->state, *root_slot);
    return context->status == ST_HEAP_OK;
}

static bool frame_chain_needs_control(const StFrame *top_frame)
{
    const StFrame *frame;
    for (frame = top_frame; frame; frame = frame->caller)
        if (frame->home != NULL || (frame->method != NULL &&
            (frame->method->flags &
             (ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN)) != 0u))
            return true;
    return false;
}

static st_heap_status_t validate_frame_chain(const st_heap_state_t *state,
                                             const StFrame *top_frame)
{
    const StFrame *slow = top_frame;
    const StFrame *fast = top_frame;
    const StFrame *frame;
    void *thread = top_frame ? top_frame->thread : NULL;
    while (fast && fast->caller) {
        slow = slow->caller;
        fast = fast->caller->caller;
        if (slow == fast) return ST_HEAP_ERR_FRAME_CYCLE;
    }
    for (frame = top_frame; frame; frame = frame->caller) {
        const st_root_map_t *map;
        size_t index;
        st_heap_status_t status;
        if (!st_method_descriptor_is_valid(frame->method) ||
            frame->thread == NULL || frame->thread != thread ||
            frame->flags != 0u ||
            frame->argc != frame->method->arity ||
            ((frame->argv == NULL) != (frame->argc == 0u)))
            return ST_HEAP_ERR_INVALID_FRAME;
        map = find_root_map(frame->method, frame->safepoint_id);
        if (!map ||
            frame->root_count != frame->method->frame_root_capacity ||
            map->root_count > frame->root_count ||
            ((frame->roots == NULL) != (frame->root_count == 0u)))
            return ST_HEAP_ERR_INVALID_FRAME;
        status = validate_root_value(state, frame->receiver);
        if (status != ST_HEAP_OK) return status;
        for (index = 0u; index < frame->argc; ++index) {
            status = validate_root_value(state, frame->argv[index]);
            if (status != ST_HEAP_OK) return status;
        }
        for (index = 0u; index < map->root_count; ++index) {
            if (!bitmap_test(map->live_root_bitmap, index)) continue;
            status = validate_root_value(state, frame->roots[index]);
            if (status != ST_HEAP_OK) return status;
        }
    }
    if (frame_chain_needs_control(top_frame)) {
        control_validate_context_t context = { state, ST_HEAP_OK };
        size_t visited = 0u;
        st_control_status_t control_status = st_aot_control_visit_roots(
            top_frame, validate_control_root, &context, &visited);
        (void)visited;
        if (control_status == ST_CONTROL_ERR_VISITOR_ABORTED &&
            context.status != ST_HEAP_OK)
            return context.status;
        if (control_status != ST_CONTROL_OK)
            return ST_HEAP_ERR_INVALID_FRAME;
    }
    return ST_HEAP_OK;
}

static st_heap_status_t mark_value(mark_context_t *context, st_value_t value)
{
    size_t index;
    uint64_t mask;
    if (st_value_kind(value) != ST_VALUE_OBJECT) return ST_HEAP_OK;
    if (!find_entry(context->state, value, &index))
        return ST_HEAP_ERR_DANGLING_REFERENCE;
    mask = UINT64_C(1) << (index & 63u);
    if ((context->marks[index >> 6] & mask) != 0u) return ST_HEAP_OK;
    context->marks[index >> 6] |= mask;
    if (context->worklist_count >= context->state->entry_count)
        return ST_HEAP_ERR_BAD_OBJECT;
    context->worklist[context->worklist_count++] = index;
    ++context->marked_count;
    return ST_HEAP_OK;
}

static bool mark_reference(void *user, st_value_t *slot)
{
    mark_context_t *context = user;
    context->status = mark_value(context, *slot);
    return context->status == ST_HEAP_OK;
}

static bool mark_control_root(void *user, const st_value_t *root_slot)
{
    mark_context_t *context = user;
    if (root_slot == NULL) {
        context->status = ST_HEAP_ERR_INVALID_ROOT;
        return false;
    }
    context->status = mark_value(context, *root_slot);
    return context->status == ST_HEAP_OK;
}

static st_heap_status_t mark_frame_chain(mark_context_t *context,
                                         const StFrame *top_frame)
{
    const StFrame *frame;
    for (frame = top_frame; frame; frame = frame->caller) {
        const st_root_map_t *map = find_root_map(frame->method,
                                                 frame->safepoint_id);
        size_t index;
        if (!map) return ST_HEAP_ERR_INVALID_FRAME;
        st_heap_status_t status = mark_value(context, frame->receiver);
        if (status != ST_HEAP_OK) return status;
        for (index = 0u; index < frame->argc; ++index) {
            status = mark_value(context, frame->argv[index]);
            if (status != ST_HEAP_OK) return status;
        }
        for (index = 0u; index < map->root_count; ++index) {
            if (!bitmap_test(map->live_root_bitmap, index)) continue;
            status = mark_value(context, frame->roots[index]);
            if (status != ST_HEAP_OK) return status;
        }
    }
    if (frame_chain_needs_control(top_frame)) {
        size_t visited = 0u;
        st_control_status_t control_status = st_aot_control_visit_roots(
            top_frame, mark_control_root, context, &visited);
        (void)visited;
        if (control_status == ST_CONTROL_ERR_VISITOR_ABORTED &&
            context->status != ST_HEAP_OK)
            return context->status;
        if (control_status != ST_CONTROL_OK)
            return ST_HEAP_ERR_INVALID_FRAME;
    }
    return ST_HEAP_OK;
}

static st_heap_status_t allocate_mark_workspace(
    st_heap_state_t *state, uint64_t **marks_out, size_t **worklist_out,
    reclaim_record_t **reclaims_out, void **workspace_out,
    size_t *workspace_size_out)
{
    size_t mark_word_count;
    size_t mark_bytes;
    size_t worklist_offset;
    size_t worklist_bytes;
    size_t reclaim_offset;
    size_t reclaim_bytes = 0u;
    size_t total;
    void *workspace;
    bool bad_alignment = false;
    bool size_overflow = false;
    if (state->entry_count == 0u) {
        *marks_out = NULL;
        *worklist_out = NULL;
        *reclaims_out = NULL;
        *workspace_out = NULL;
        *workspace_size_out = 0u;
        return ST_HEAP_OK;
    }
    if (state->entry_count > SIZE_MAX - (ST_HEAP_BITS_PER_WORD - 1u))
        return ST_HEAP_ERR_OVERFLOW;
    mark_word_count = (state->entry_count +
                       (ST_HEAP_BITS_PER_WORD - 1u)) /
                      ST_HEAP_BITS_PER_WORD;
    if (!multiply_size(mark_word_count, sizeof(uint64_t), &mark_bytes) ||
        !round_up(mark_bytes, _Alignof(size_t), &worklist_offset) ||
        !multiply_size(state->entry_count, sizeof(size_t),
                       &worklist_bytes) ||
        !add_size(worklist_offset, worklist_bytes, &total) ||
        !round_up(total, _Alignof(reclaim_record_t), &reclaim_offset) ||
        (observer_valid(state->reclaim_observer) &&
         !multiply_size(state->entry_count, sizeof(reclaim_record_t),
                        &reclaim_bytes)) ||
        !add_size(reclaim_offset, reclaim_bytes, &total))
        return ST_HEAP_ERR_OVERFLOW;
    workspace = allocate_block(state->allocator, _Alignof(max_align_t), total,
                               workspace_size_out, &bad_alignment,
                               &size_overflow);
    if (!workspace) return size_overflow ? ST_HEAP_ERR_OVERFLOW
        : bad_alignment ? ST_HEAP_ERR_BAD_ALIGNMENT
                        : ST_HEAP_ERR_OUT_OF_MEMORY;
    memset(workspace, 0, total);
    *marks_out = workspace;
    *worklist_out = (size_t *)((unsigned char *)workspace + worklist_offset);
    *reclaims_out = reclaim_bytes != 0u
        ? (reclaim_record_t *)((unsigned char *)workspace + reclaim_offset)
        : NULL;
    *workspace_out = workspace;
    return ST_HEAP_OK;
}

static void clear_collector_header_state(st_heap_entry_t *entry)
{
    st_heap_object_t *object = entry->extent.base;
    uint64_t mask = ~(ST_HEADER_COLOR_MASK |
        ((uint64_t)ST_HEADER_REMEMBERED << ST_HEADER_FLAGS_SHIFT));
    atomic_fetch_and_explicit(&object->header.bits, mask,
                              memory_order_relaxed);
}

static st_heap_status_t prepare_reclaims(
    st_heap_state_t *state, const uint64_t *marks,
    reclaim_record_t *records, size_t *record_count_out)
{
    size_t index;
    size_t count = 0u;
    *record_count_out = 0u;
    if (!observer_valid(state->reclaim_observer)) return ST_HEAP_OK;
    if (records == NULL && state->entry_count != 0u)
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    for (index = 0u; index < state->entry_count; index++) {
        bool marked = (marks[index >> 6] &
            (UINT64_C(1) << (index & 63u))) != 0u;
        st_object_view_t view;
        uintptr_t cookie = 0u;
        st_heap_status_t status;
        if (marked) continue;
        if (st_object_validate(state->descriptors,
                state->entries[index].value, state->entries[index].extent,
                &view) != ST_RUNTIME_OK)
            return ST_HEAP_ERR_BAD_OBJECT;
        status = state->reclaim_observer.prepare(
            state->reclaim_observer.user, state->entries[index].value,
            state->entries[index].extent,
            view.class_descriptor->class_id,
            view.shape_descriptor->shape_id, &cookie);
        if (status != ST_HEAP_OK) return status;
        if (cookie != 0u) {
            if (count >= state->entry_count)
                return ST_HEAP_ERR_RECLAIM_PROTOCOL;
            records[count++] = (reclaim_record_t) {
                .value = state->entries[index].value,
                .class_id = view.class_descriptor->class_id,
                .shape_id = view.shape_descriptor->shape_id,
                .cookie = cookie
            };
        }
    }
    *record_count_out = count;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_collect_root_sets(
    st_heap_t *heap, const StFrame *top_frame,
    const st_heap_root_set_t *root_sets, size_t root_set_count,
    st_heap_collection_stats_t *stats_out)
{
    st_heap_state_t *state = heap_state(heap);
    st_heap_collection_stats_t stats = {0};
    mark_context_t context = {0};
    reclaim_record_t *reclaims = NULL;
    size_t reclaim_count = 0u;
    void *workspace = NULL;
    size_t workspace_size = 0u;
    size_t index;
    size_t set_index;
    st_heap_status_t status;
    if (stats_out) *stats_out = (st_heap_collection_stats_t){0};
    if (!state || !stats_out)
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (root_set_count != 0u && root_sets == NULL)
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (root_set_count == 0u && root_sets != NULL)
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (root_sets != NULL)
        for (set_index = 0u; set_index < root_set_count; set_index++)
            if ((root_sets[set_index].values == NULL)
                    != (root_sets[set_index].count == 0u))
                return ST_HEAP_ERR_INVALID_ARGUMENT;
    if (state->collection_count == UINT64_MAX)
        return ST_HEAP_ERR_OVERFLOW;
    status = validate_frame_chain(state, top_frame);
    if (status != ST_HEAP_OK) return status;
    if (root_sets != NULL)
        for (set_index = 0u; set_index < root_set_count; set_index++)
            for (index = 0u; index < root_sets[set_index].count; index++) {
                status = validate_root_value(
                    state, root_sets[set_index].values[index]);
                if (status != ST_HEAP_OK) return status;
            }
    status = allocate_mark_workspace(state, &context.marks,
                                     &context.worklist, &reclaims, &workspace,
                                     &workspace_size);
    if (status != ST_HEAP_OK) return status;
    context.state = state;
    context.status = ST_HEAP_OK;
    status = mark_frame_chain(&context, top_frame);
    if (root_sets != NULL)
        for (set_index = 0u; status == ST_HEAP_OK
                && set_index < root_set_count; set_index++)
            for (index = 0u; status == ST_HEAP_OK
                    && index < root_sets[set_index].count; index++)
                status = mark_value(
                    &context, root_sets[set_index].values[index]);
    while (status == ST_HEAP_OK && context.worklist_count != 0u) {
        size_t entry_index = context.worklist[--context.worklist_count];
        size_t visited = 0u;
        st_runtime_status_t runtime_status = st_object_visit_references(
            state->descriptors, state->entries[entry_index].value,
            state->entries[entry_index].extent, mark_reference, &context,
            &visited);
        (void)visited;
        if (runtime_status != ST_RUNTIME_OK)
            status = context.status != ST_HEAP_OK
                ? context.status : ST_HEAP_ERR_BAD_OBJECT;
    }
    if (status != ST_HEAP_OK) {
        deallocate_block(state->allocator, workspace, _Alignof(max_align_t),
                         workspace_size);
        return status;
    }
    status = prepare_reclaims(state, context.marks, reclaims,
                              &reclaim_count);
    if (status != ST_HEAP_OK) {
        deallocate_block(state->allocator, workspace, _Alignof(max_align_t),
                         workspace_size);
        return status;
    }

    stats.objects_before = state->entry_count;
    stats.marked_objects = context.marked_count;
    stats.bytes_before = state->allocated_bytes;
    {
        size_t write_index = 0u;
        size_t old_count = state->entry_count;
        for (index = 0u; index < old_count; ++index) {
            bool marked = (context.marks[index >> 6] &
                (UINT64_C(1) << (index & 63u))) != 0u;
            if (marked) {
                clear_collector_header_state(&state->entries[index]);
                if (write_index != index)
                    state->entries[write_index] = state->entries[index];
                ++write_index;
            } else {
                stats.reclaimed_bytes +=
                    state->entries[index].extent.byte_size;
                st_object_deallocate(state->allocator,
                                     state->entries[index].extent);
            }
        }
        state->entry_count = write_index;
    }
    state->allocated_bytes -= stats.reclaimed_bytes;
    if (state->table_capacity != 0u) table_rebuild(state);
    ++state->collection_count;
    stats.reclaimed_objects = stats.objects_before - stats.marked_objects;
    stats.bytes_after = state->allocated_bytes;
    stats.collection_index = state->collection_count;
    /* External release happens only after the sweep and registry commit. The
     * prepare phase guarantees that these callbacks cannot fail. */
    for (index = 0u; index < reclaim_count; index++)
        state->reclaim_observer.commit(
            state->reclaim_observer.user, reclaims[index].value,
            reclaims[index].class_id, reclaims[index].shape_id,
            reclaims[index].cookie);
    deallocate_block(state->allocator, workspace, _Alignof(max_align_t),
                     workspace_size);
    *stats_out = stats;
    return ST_HEAP_OK;
}

st_heap_status_t st_heap_collect(
    st_heap_t *heap, const StFrame *top_frame,
    const st_value_t *global_roots, size_t global_root_count,
    st_heap_collection_stats_t *stats_out)
{
    st_heap_root_set_t roots = { global_roots, global_root_count };
    if ((global_roots == NULL) != (global_root_count == 0u)) {
        if (stats_out != NULL)
            *stats_out = (st_heap_collection_stats_t){0};
        return ST_HEAP_ERR_INVALID_ARGUMENT;
    }
    return st_heap_collect_root_sets(
        heap, top_frame, global_root_count == 0u ? NULL : &roots,
        global_root_count == 0u ? 0u : 1u, stats_out);
}

const char *st_heap_status_string(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK: return "ok";
    case ST_HEAP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_HEAP_ERR_INVALID_DESCRIPTOR: return "invalid descriptor";
    case ST_HEAP_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_HEAP_ERR_OVERFLOW: return "size overflow";
    case ST_HEAP_ERR_BAD_ALIGNMENT: return "bad alignment";
    case ST_HEAP_ERR_BAD_EXTENT: return "bad allocation extent";
    case ST_HEAP_ERR_BAD_OBJECT: return "malformed object";
    case ST_HEAP_ERR_NOT_OBJECT: return "value is not a heap object";
    case ST_HEAP_ERR_NOT_MEMBER: return "object is not a heap member";
    case ST_HEAP_ERR_BAD_SLOT: return "slot is not a fixed reference";
    case ST_HEAP_ERR_IMMUTABLE: return "object is immutable";
    case ST_HEAP_ERR_INVALID_ROOT: return "invalid root value";
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return "dangling or foreign object reference";
    case ST_HEAP_ERR_INVALID_FRAME: return "invalid shadow-root frame";
    case ST_HEAP_ERR_FRAME_CYCLE: return "cyclic frame chain";
    case ST_HEAP_ERR_RECLAIM_PROTOCOL: return "invalid reclaim protocol";
    case ST_HEAP_ERR_CONFLICT: return "conflicting heap state";
    default: return "unknown heap status";
    }
}
