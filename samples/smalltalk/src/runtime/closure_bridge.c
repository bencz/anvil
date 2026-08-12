#include "st_closure_bridge.h"

#include <stdlib.h>
#include <string.h>

#define CLOSURE_STATE_MAGIC UINT64_C(0x434c4f5355524531)
#define CLOSURE_OBJECT_MAGIC UINT64_C(0x5354434c4f535231)

typedef struct st_aot_home_entry {
    st_value_t closure;
    StHomeToken *home;
    uint64_t hash;
} st_aot_home_entry_t;

struct st_aot_closure_state {
    uint64_t magic;
    st_heap_t *heap;
    uint32_t closure_class_id;
    uint32_t closure_shape_id;
    uint32_t cell_class_id;
    uint32_t cell_shape_id;
    uint32_t argument_array_class_id;
    uint32_t argument_array_shape_id;
    const st_aot_block_descriptor_t **descriptors;
    size_t descriptor_count;
    st_aot_home_entry_t **entries;
    size_t entry_count;
    size_t entry_capacity;
    st_aot_closure_allocate_fn allocate;
    st_aot_closure_deallocate_fn deallocate;
    void *allocator_user;
    st_heap_reclaim_observer_t observer;
};

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static int descriptor_compare(const void *left, const void *right)
{
    uintptr_t a = (uintptr_t)*(const st_aot_block_descriptor_t *const *)left;
    uintptr_t b = (uintptr_t)*(const st_aot_block_descriptor_t *const *)right;
    return a < b ? -1 : a > b;
}

static bool bitmap_word_is(const StShapeDescriptor *shape, uint64_t word)
{
    return shape && shape->fixed_pointer_bitmap_word_count == 1u
        && shape->fixed_pointer_bitmap
        && shape->fixed_pointer_bitmap[0] == word;
}

static bool descriptor_valid(const st_aot_block_descriptor_t *descriptor)
{
    if (!descriptor
            || descriptor->abi_version != ST_AOT_BLOCK_ABI_VERSION
            || (descriptor->flags & ~(uint32_t)ST_AOT_BLOCK_FLAGS_MASK) != 0u
            || !descriptor->code
            || !st_method_descriptor_is_valid(descriptor->method)
            || descriptor->method->arity != descriptor->arity
            || descriptor->capture_descriptor_count
                != descriptor->capture_count
            || ((descriptor->capture_count != 0u)
                != (descriptor->captures != NULL))
            || (((descriptor->flags & ST_AOT_BLOCK_HAS_HOME) != 0u)
                != ((descriptor->method->flags
                     & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u)))
        return false;
    bool saw_cell = false;
    for (size_t index = 0u; index < descriptor->capture_count; index++) {
        uint32_t kind = descriptor->captures[index].kind;
        if (kind > ST_AOT_CAPTURE_SELF) {
            return false;
        }
        if (kind == ST_AOT_CAPTURE_CELL) saw_cell = true;
    }
    return saw_cell ==
        ((descriptor->flags & ST_AOT_BLOCK_HAS_CELLS) != 0u);
}

static const st_aot_block_descriptor_t *descriptor_find(
    const st_aot_closure_state_t *state,
    const st_aot_block_descriptor_t *candidate)
{
    size_t low = 0u;
    size_t high = state->descriptor_count;
    uintptr_t wanted = (uintptr_t)candidate;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uintptr_t address = (uintptr_t)state->descriptors[middle];
        if (address < wanted) low = middle + 1u;
        else high = middle;
    }
    return low < state->descriptor_count
            && state->descriptors[low] == candidate
        ? state->descriptors[low] : NULL;
}

static uint64_t closure_hash(st_value_t closure)
{
    uint64_t value = closure >> 3u;
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31u;
    return value;
}

static size_t probe_distance(uint64_t hash, size_t slot, size_t mask)
{
    return (slot - ((size_t)hash & mask)) & mask;
}

static st_aot_home_entry_t *entry_find(
    const st_aot_closure_state_t *state, st_value_t closure)
{
    uint64_t hash = closure_hash(closure);
    size_t mask = state->entry_capacity - 1u;
    size_t slot = (size_t)hash & mask;
    size_t distance = 0u;
    for (;;) {
        st_aot_home_entry_t *entry = state->entries[slot];
        if (!entry || probe_distance(entry->hash, slot, mask) < distance)
            return NULL;
        if (entry->hash == hash && entry->closure == closure) {
            return entry;
        }
        slot = (slot + 1u) & mask;
        distance++;
    }
}

static void entry_insert_no_grow(st_aot_closure_state_t *state,
                                 st_aot_home_entry_t *entry)
{
    size_t mask = state->entry_capacity - 1u;
    size_t slot = (size_t)entry->hash & mask;
    size_t distance = 0u;
    for (;;) {
        st_aot_home_entry_t *resident = state->entries[slot];
        if (!resident) {
            state->entries[slot] = entry;
            state->entry_count++;
            return;
        }
        size_t resident_distance = probe_distance(
            resident->hash, slot, mask);
        if (resident_distance < distance) {
            state->entries[slot] = entry;
            entry = resident;
            distance = resident_distance;
        }
        slot = (slot + 1u) & mask;
        distance++;
    }
}

static bool entry_reserve(st_aot_closure_state_t *state)
{
    if (state->entry_count + 1u <= state->entry_capacity * 3u / 4u)
        return true;
    if (state->entry_capacity > SIZE_MAX / 2u
            || state->entry_capacity * 2u
                > SIZE_MAX / sizeof(*state->entries))
        return false;
    size_t old_capacity = state->entry_capacity;
    st_aot_home_entry_t **old_entries = state->entries;
    size_t new_capacity = old_capacity * 2u;
    st_aot_home_entry_t **new_entries = state->allocate(
        state->allocator_user, new_capacity * sizeof(*new_entries));
    if (!new_entries) {
        return false;
    }
    memset(new_entries, 0, new_capacity * sizeof(*new_entries));
    state->entries = new_entries;
    state->entry_capacity = new_capacity;
    state->entry_count = 0u;
    for (size_t slot = 0u; slot < old_capacity; slot++)
        if (old_entries[slot]) entry_insert_no_grow(state, old_entries[slot]);
    state->deallocate(state->allocator_user, old_entries);
    return true;
}

static void entry_remove(st_aot_closure_state_t *state,
                         st_aot_home_entry_t *entry)
{
    size_t mask = state->entry_capacity - 1u;
    size_t slot = (size_t)entry->hash & mask;
    while (state->entries[slot] != entry) {
        if (!state->entries[slot]) {
            abort();
        }
        slot = (slot + 1u) & mask;
    }
    size_t next = (slot + 1u) & mask;
    while (state->entries[next]
            && probe_distance(state->entries[next]->hash, next, mask) != 0u) {
        state->entries[slot] = state->entries[next];
        slot = next;
        next = (next + 1u) & mask;
    }
    state->entries[slot] = NULL;
    state->entry_count--;
}

static bool state_ready(const st_aot_closure_state_t *state)
{
    return state && state->magic == CLOSURE_STATE_MAGIC && state->heap
        && state->closure_class_id && state->closure_shape_id
        && state->descriptors && state->descriptor_count
        && state->entries && state->entry_capacity >= 8u
        && (state->entry_capacity & (state->entry_capacity - 1u)) == 0u
        && state->entry_count <= state->entry_capacity
        && state->allocate && state->deallocate
        && state->observer.prepare && state->observer.commit
        && state->observer.user == state;
}

static st_heap_status_t reclaim_prepare(
    void *user, st_value_t exact_value, st_object_extent_t extent,
    uint32_t class_id, uint32_t shape_id, uintptr_t *cookie_out)
{
    st_aot_closure_state_t *state = user;
    st_heap_object_t *object;
    uint64_t *fixed;
    const st_aot_block_descriptor_t *candidate;
    const st_aot_block_descriptor_t *descriptor;
    st_aot_home_entry_t *entry;
    if (cookie_out == NULL) {
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    }
    *cookie_out = 0u;
    if (!state_ready(state)) {
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    }
    if (class_id != state->closure_class_id
            || shape_id != state->closure_shape_id)
        return ST_HEAP_OK;
    if (!extent.base || exact_value != (st_value_t)(uintptr_t)extent.base
            || extent.byte_size < offsetof(st_heap_object_t, payload)
                                  + 4u * sizeof(uint64_t))
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    object = extent.base;
    fixed = (uint64_t *)object->payload;
    if (fixed[0] != CLOSURE_OBJECT_MAGIC || fixed[3] != 0u)
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    candidate = (const st_aot_block_descriptor_t *)(uintptr_t)fixed[1];
    descriptor = descriptor_find(state, candidate);
    if (!descriptor || object->indexed_length != descriptor->capture_count)
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    entry = entry_find(state, exact_value);
    if (!entry) {
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    }
    if ((descriptor->flags & ST_AOT_BLOCK_HAS_HOME) == 0u) {
        if (fixed[2] != 0u || entry->home != NULL)
            return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    } else if (!entry->home
            || fixed[2] != (uint64_t)(uintptr_t)entry->home) {
        return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    }
    *cookie_out = (uintptr_t)entry;
    return ST_HEAP_OK;
}

static void reclaim_commit(
    void *user, st_value_t exact_value, uint32_t class_id,
    uint32_t shape_id, uintptr_t cookie)
{
    st_aot_closure_state_t *state = user;
    st_aot_home_entry_t *entry = (st_aot_home_entry_t *)cookie;
    if (!state_ready(state) || !entry || entry->closure != exact_value
            || class_id != state->closure_class_id
            || shape_id != state->closure_shape_id)
        abort();
    if (entry_find(state, exact_value) != entry) {
        abort();
    }
    entry_remove(state, entry);
    if (entry->home) st_home_token_release(entry->home);
    state->deallocate(state->allocator_user, entry);
}

static st_aot_closure_status_t heap_status(st_heap_status_t status)
{
    if (status == ST_HEAP_OK) {
        return ST_AOT_CLOSURE_OK;
    }
    if (status == ST_HEAP_ERR_OUT_OF_MEMORY)
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    return ST_AOT_CLOSURE_ERR_HEAP;
}

st_aot_closure_status_t st_aot_closure_context_init(
    st_aot_closure_context_t *context,
    const st_aot_closure_options_t *options)
{
    st_aot_closure_state_t *state = NULL;
    st_aot_closure_allocate_fn allocate = default_allocate;
    st_aot_closure_deallocate_fn deallocate = default_deallocate;
    const st_runtime_descriptors_t *runtime;
    const StClassDescriptor *closure_class;
    const StShapeDescriptor *closure_shape;
    if (!context || context->state || !options || !options->heap
            || !options->descriptors || options->descriptor_count == 0u
            || ((options->allocate == NULL) != (options->deallocate == NULL)))
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    if (options->allocate) {
        allocate = options->allocate;
        deallocate = options->deallocate;
    }
    runtime = st_heap_descriptors(options->heap);
    closure_class = st_runtime_class(runtime, options->closure_class_id);
    closure_shape = st_runtime_shape(runtime, options->closure_shape_id);
    if (!runtime || !closure_class || !closure_shape
            || closure_shape->class_id != closure_class->class_id
            || closure_shape->fixed_word_count != 4u
            || !bitmap_word_is(closure_shape, 0u)
            || closure_shape->indexed_format != ST_INDEXED_VALUES
            || ((options->cell_class_id == 0u)
                != (options->cell_shape_id == 0u))
            || ((options->argument_array_class_id == 0u)
                != (options->argument_array_shape_id == 0u)))
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    if (options->cell_class_id != 0u) {
        const StClassDescriptor *cell_class = st_runtime_class(
            runtime, options->cell_class_id);
        const StShapeDescriptor *cell_shape = st_runtime_shape(
            runtime, options->cell_shape_id);
        if (!cell_class || !cell_shape
                || cell_shape->class_id != cell_class->class_id
                || cell_shape->fixed_word_count != 1u
                || !bitmap_word_is(cell_shape, UINT64_C(1))
                || cell_shape->indexed_format != ST_INDEXED_NONE)
            return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    if (options->argument_array_class_id != 0u) {
        const StClassDescriptor *array_class = st_runtime_class(
            runtime, options->argument_array_class_id);
        const StShapeDescriptor *array_shape = st_runtime_shape(
            runtime, options->argument_array_shape_id);
        if (!array_class || !array_shape
                || array_shape->class_id != array_class->class_id
                || array_shape->fixed_word_count != 0u
                || array_shape->fixed_pointer_bitmap != NULL
                || array_shape->fixed_pointer_bitmap_word_count != 0u
                || array_shape->indexed_format != ST_INDEXED_VALUES)
            return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    state = allocate(options->allocator_user, sizeof(*state));
    if (!state) {
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    if (options->descriptor_count
            > SIZE_MAX / sizeof(*state->descriptors)) {
        deallocate(options->allocator_user, state);
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    state->descriptors = allocate(
        options->allocator_user,
        options->descriptor_count * sizeof(*state->descriptors));
    if (!state->descriptors) {
        deallocate(options->allocator_user, state);
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    }
    bool needs_cells = false;
    for (size_t index = 0u; index < options->descriptor_count; index++) {
        if (!descriptor_valid(options->descriptors[index])) {
            deallocate(options->allocator_user, state->descriptors);
            deallocate(options->allocator_user, state);
            return ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR;
        }
        needs_cells = needs_cells || (options->descriptors[index]->flags
            & ST_AOT_BLOCK_HAS_CELLS) != 0u;
        state->descriptors[index] = options->descriptors[index];
    }
    if (needs_cells && options->cell_class_id == 0u) {
        deallocate(options->allocator_user, state->descriptors);
        deallocate(options->allocator_user, state);
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    }
    qsort(state->descriptors, options->descriptor_count,
          sizeof(*state->descriptors), descriptor_compare);
    for (size_t index = 1u; index < options->descriptor_count; index++) {
        if (state->descriptors[index - 1u] == state->descriptors[index]) {
            deallocate(options->allocator_user, state->descriptors);
            deallocate(options->allocator_user, state);
            return ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR;
        }
    }
    state->magic = CLOSURE_STATE_MAGIC;
    state->heap = options->heap;
    state->closure_class_id = options->closure_class_id;
    state->closure_shape_id = options->closure_shape_id;
    state->cell_class_id = options->cell_class_id;
    state->cell_shape_id = options->cell_shape_id;
    state->argument_array_class_id = options->argument_array_class_id;
    state->argument_array_shape_id = options->argument_array_shape_id;
    state->descriptor_count = options->descriptor_count;
    state->allocate = allocate;
    state->deallocate = deallocate;
    state->allocator_user = options->allocator_user;
    state->entry_capacity = 8u;
    state->entries = allocate(options->allocator_user,
                              state->entry_capacity
                                  * sizeof(*state->entries));
    if (!state->entries) {
        state->magic = 0u;
        deallocate(options->allocator_user, state->descriptors);
        deallocate(options->allocator_user, state);
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    }
    memset(state->entries, 0,
           state->entry_capacity * sizeof(*state->entries));
    state->observer = (st_heap_reclaim_observer_t) {
        reclaim_prepare, reclaim_commit, state
    };
    st_heap_status_t install = st_heap_reclaim_observer_install(
        state->heap, state->observer);
    if (install != ST_HEAP_OK) {
        state->magic = 0u;
        deallocate(options->allocator_user, state->entries);
        deallocate(options->allocator_user, state->descriptors);
        deallocate(options->allocator_user, state);
        return heap_status(install);
    }
    context->state = state;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_context_destroy(
    st_aot_closure_context_t *context)
{
    st_aot_closure_state_t *state;
    st_heap_status_t remove;
    if (!context || !state_ready(context->state))
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    state = context->state;
    if (state->entry_count != 0u) {
        return ST_AOT_CLOSURE_ERR_BUSY;
    }
    remove = st_heap_reclaim_observer_remove(state->heap, state->observer);
    if (remove != ST_HEAP_OK) {
        return heap_status(remove);
    }
    context->state = NULL;
    state->magic = 0u;
    state->deallocate(state->allocator_user, state->entries);
    state->deallocate(state->allocator_user, state->descriptors);
    state->deallocate(state->allocator_user, state);
    return ST_AOT_CLOSURE_OK;
}

static st_aot_closure_status_t context_for_frame(
    StFrame *frame, st_aot_closure_state_t **state_out)
{
    st_aot_thread_t *thread;
    if (!state_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *state_out = NULL;
    if (!frame || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_AOT_CLOSURE_ERR_INVALID_FRAME;
    thread = frame->thread;
    if (!thread->closures || !state_ready(thread->closures->state))
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    *state_out = thread->closures->state;
    return ST_AOT_CLOSURE_OK;
}

static bool value_valid(const st_aot_closure_state_t *state,
                        st_value_t value)
{
    st_value_kind_t kind = st_value_kind(value);
    return kind != ST_VALUE_INVALID
        && (kind != ST_VALUE_OBJECT || st_heap_contains(state->heap, value));
}

st_aot_closure_status_t st_aot_closure_create(
    StFrame *frame, const st_aot_block_descriptor_t *candidate,
    st_value_t lexical_self, const st_value_t *captures,
    uint32_t capture_count,
    st_value_t *closure_out)
{
    st_aot_closure_state_t *state;
    const st_aot_block_descriptor_t *descriptor;
    st_aot_home_entry_t *entry = NULL;
    st_aot_closure_status_t status;
    st_value_t closure = st_value_nil();
    if (!closure_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *closure_out = st_value_nil();
    status = context_for_frame(frame, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    descriptor = descriptor_find(state, candidate);
    if (!descriptor) {
        return ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR;
    }
    if (!value_valid(state, lexical_self)
            || capture_count != descriptor->capture_count
            || (capture_count != 0u && !captures))
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    for (uint32_t index = 0u; index < capture_count; index++)
        if (!value_valid(state, captures[index])
                || (descriptor->captures[index].kind == ST_AOT_CAPTURE_SELF
                    && captures[index] != lexical_self))
            return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    if (!entry_reserve(state)) {
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    }
    entry = state->allocate(state->allocator_user, sizeof(*entry));
    if (!entry) {
        return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
    }
    memset(entry, 0, sizeof(*entry));
    if ((descriptor->flags & ST_AOT_BLOCK_HAS_HOME) != 0u) {
        if (!frame->home) {
            state->deallocate(state->allocator_user, entry);
            return ST_AOT_CLOSURE_ERR_HOME_REQUIRED;
        }
        if (!st_home_token_is_active(frame->home)) {
            state->deallocate(state->allocator_user, entry);
            return ST_AOT_CLOSURE_ERR_HOME_RETURNED;
        }
        if (st_home_token_retain(frame->home) != ST_CONTROL_OK) {
            state->deallocate(state->allocator_user, entry);
            return ST_AOT_CLOSURE_ERR_HOME_RETURNED;
        }
        entry->home = frame->home;
    }
    st_heap_status_t allocation = st_heap_allocate(
        state->heap, state->closure_class_id, state->closure_shape_id,
        capture_count, capture_count, ST_HEADER_IMMUTABLE, &closure);
    if (allocation != ST_HEAP_OK) {
        if (entry->home) st_home_token_release(entry->home);
        state->deallocate(state->allocator_user, entry);
        return heap_status(allocation);
    }
    st_object_view_t view;
    if (st_heap_object_view(state->heap, closure, &view) != ST_HEAP_OK
            || view.shape_descriptor->shape_id != state->closure_shape_id
            || view.indexed_length != capture_count)
        abort();
    uint64_t *fixed = view.fixed_words;
    fixed[0] = CLOSURE_OBJECT_MAGIC;
    fixed[1] = (uint64_t)(uintptr_t)descriptor;
    fixed[2] = (uint64_t)(uintptr_t)entry->home;
    fixed[3] = 0u;
    if (capture_count != 0u)
        memcpy(view.indexed_elements, captures,
               (size_t)capture_count * sizeof(*captures));
    entry->closure = closure;
    entry->hash = closure_hash(closure);
    entry_insert_no_grow(state, entry);
    *closure_out = closure;
    return ST_AOT_CLOSURE_OK;
}

static st_aot_closure_status_t cell_view(
    st_aot_closure_state_t *state, st_value_t cell)
{
    st_object_view_t view;
    if (state->cell_class_id == 0u || state->cell_shape_id == 0u)
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    if (st_heap_object_view(state->heap, cell, &view) != ST_HEAP_OK
            || view.class_descriptor->class_id != state->cell_class_id
            || view.shape_descriptor->shape_id != state->cell_shape_id
            || view.shape_descriptor->fixed_word_count != 1u
            || view.shape_descriptor->indexed_format != ST_INDEXED_NONE)
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_cell_create(
    StFrame *frame, st_value_t initial_value, st_value_t *cell_out)
{
    st_aot_closure_state_t *state;
    st_aot_closure_status_t status;
    st_value_t cell = st_value_nil();
    if (!cell_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *cell_out = st_value_nil();
    status = context_for_frame(frame, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (state->cell_class_id == 0u || state->cell_shape_id == 0u)
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    if (!value_valid(state, initial_value))
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    st_heap_status_t heap_result = st_heap_allocate(
        state->heap, state->cell_class_id, state->cell_shape_id,
        0u, 0u, 0u, &cell);
    if (heap_result != ST_HEAP_OK) {
        return heap_status(heap_result);
    }
    heap_result = st_heap_fixed_reference_store(
        state->heap, cell, 0u, initial_value);
    if (heap_result != ST_HEAP_OK) {
        return heap_status(heap_result);
    }
    *cell_out = cell;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_cell_load(
    StFrame *frame, st_value_t cell, st_value_t *value_out)
{
    st_aot_closure_state_t *state;
    st_aot_closure_status_t status;
    if (!value_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *value_out = st_value_nil();
    status = context_for_frame(frame, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    status = cell_view(state, cell);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    st_heap_status_t heap_result = st_heap_fixed_reference_load(
        state->heap, cell, 0u, value_out);
    return heap_result == ST_HEAP_OK ? ST_AOT_CLOSURE_OK
                                    : heap_status(heap_result);
}

st_aot_closure_status_t st_aot_closure_cell_store(
    StFrame *frame, st_value_t cell, st_value_t value)
{
    st_aot_closure_state_t *state;
    st_aot_closure_status_t status = context_for_frame(frame, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    status = cell_view(state, cell);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (!value_valid(state, value)) {
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    }
    st_heap_status_t heap_result = st_heap_fixed_reference_store(
        state->heap, cell, 0u, value);
    return heap_result == ST_HEAP_OK ? ST_AOT_CLOSURE_OK
                                    : heap_status(heap_result);
}

static st_aot_closure_status_t closure_view(
    StFrame *caller, st_value_t closure, st_aot_closure_state_t **state_out,
    st_object_view_t *view_out,
    const st_aot_block_descriptor_t **descriptor_out)
{
    st_aot_closure_state_t *state;
    st_aot_closure_status_t status = context_for_frame(caller, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (st_heap_object_view(state->heap, closure, view_out) != ST_HEAP_OK
            || view_out->class_descriptor->class_id
                != state->closure_class_id
            || view_out->shape_descriptor->shape_id
                != state->closure_shape_id
            || view_out->shape_descriptor->fixed_word_count != 4u)
        return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    uint64_t *fixed = view_out->fixed_words;
    if (fixed[0] != CLOSURE_OBJECT_MAGIC || fixed[3] != 0u)
        return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    const st_aot_block_descriptor_t *candidate =
        (const st_aot_block_descriptor_t *)(uintptr_t)fixed[1];
    const st_aot_block_descriptor_t *descriptor = descriptor_find(
        state, candidate);
    if (!descriptor
            || descriptor->capture_count != view_out->indexed_length)
        return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    st_aot_home_entry_t *entry = entry_find(state, closure);
    if (!entry) {
        return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    }
    if ((descriptor->flags & ST_AOT_BLOCK_HAS_HOME) != 0u) {
        if (!entry->home || fixed[2] != (uint64_t)(uintptr_t)entry->home)
            return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    } else if (entry->home || fixed[2] != 0u) {
        return ST_AOT_CLOSURE_ERR_INVALID_CLOSURE;
    }
    *state_out = state;
    *descriptor_out = descriptor;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_resolve(
    StFrame *caller, st_value_t closure, uint32_t arity,
    st_aot_closure_target_t *target_out)
{
    st_aot_closure_state_t *state = NULL;
    st_object_view_t view;
    const st_aot_block_descriptor_t *descriptor = NULL;
    if (!target_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    memset(target_out, 0, sizeof(*target_out));
    st_aot_closure_status_t status = closure_view(
        caller, closure, &state, &view, &descriptor);
    (void)state;
    (void)view;
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (descriptor->arity != arity)
        return ST_AOT_CLOSURE_ERR_WRONG_ARITY;
    target_out->code = descriptor->code;
    target_out->method = descriptor->method;
    target_out->home = (descriptor->flags & ST_AOT_BLOCK_HAS_HOME) != 0u
        ? entry_find(state, closure)->home : NULL;
    target_out->frame_root_capacity = descriptor->method->frame_root_capacity;
    target_out->flags = descriptor->method->flags;
    target_out->capture_count = descriptor->capture_count;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_capture_load(
    StFrame *caller, st_value_t closure, uint32_t capture_index,
    st_value_t *value_out)
{
    st_aot_closure_state_t *state = NULL;
    st_object_view_t view;
    const st_aot_block_descriptor_t *descriptor = NULL;
    if (!value_out) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *value_out = st_value_nil();
    st_aot_closure_status_t status = closure_view(
        caller, closure, &state, &view, &descriptor);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (capture_index >= descriptor->capture_count)
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    st_value_t value = ((const st_value_t *)view.indexed_elements)[
        capture_index];
    if (!value_valid(state, value))
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    *value_out = value;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_invoke(
    StFrame *caller, st_value_t closure, const st_value_t *arguments,
    uint32_t argument_count, st_value_t *result_out)
{
    st_aot_closure_state_t *state = NULL;
    st_aot_closure_target_t target;
    st_value_t *roots = NULL;
    st_value_t result;
    StFrame child;
    st_aot_closure_status_t status;

    if (result_out == NULL) {
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    }
    *result_out = (st_value_t)ST_VALUE_INVALID;
    if (argument_count != 0u && arguments == NULL)
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;

    status = context_for_frame(caller, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    for (uint32_t index = 0u; index < argument_count; ++index) {
        if (!value_valid(state, arguments[index]))
            return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    }
    status = st_aot_closure_resolve(
        caller, closure, argument_count, &target);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (target.home != NULL && !st_home_token_is_active(target.home))
        return ST_AOT_CLOSURE_ERR_BLOCK_RETURNED;
    if (argument_count == UINT32_MAX
            || target.frame_root_capacity < argument_count + 1u
            || target.frame_root_capacity > ST_AOT_MAX_DYNAMIC_ROOTS)
        return ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR;

    if (target.frame_root_capacity != 0u) {
        roots = state->allocate(
            state->allocator_user,
            (size_t)target.frame_root_capacity * sizeof(*roots));
        if (roots == NULL) {
            return ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY;
        }
        if (st_aot_frame_roots_initialize(
                roots, target.frame_root_capacity) != ST_AOT_SEND_OK) {
            state->deallocate(state->allocator_user, roots);
            return ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR;
        }
        roots[0] = closure;
        for (uint32_t index = 0u; index < argument_count; ++index)
            roots[index + 1u] = arguments[index];
    }

    child = (StFrame) {
        .thread = caller->thread,
        .caller = caller,
        .method = target.method,
        .home = target.home,
        .receiver = closure,
        .argv = arguments,
        .roots = roots,
        .argc = argument_count,
        .root_count = target.frame_root_capacity,
        .safepoint_id = 0u,
        .flags = 0u
    };
    result = target.code(&child);
    if (roots != NULL) state->deallocate(state->allocator_user, roots);
    if (!value_valid(state, result))
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    *result_out = result;
    return ST_AOT_CLOSURE_OK;
}

st_aot_closure_status_t st_aot_closure_argument_array_view(
    StFrame *caller, st_value_t array, const st_value_t **arguments_out,
    uint32_t *argument_count_out)
{
    st_aot_closure_state_t *state = NULL;
    st_object_view_t view;
    st_aot_closure_status_t status;

    if (arguments_out == NULL || argument_count_out == NULL)
        return ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT;
    *arguments_out = NULL;
    *argument_count_out = 0u;
    status = context_for_frame(caller, &state);
    if (status != ST_AOT_CLOSURE_OK) {
        return status;
    }
    if (state->argument_array_class_id == 0u
            || state->argument_array_shape_id == 0u)
        return ST_AOT_CLOSURE_ERR_INVALID_CONTEXT;
    if (st_heap_object_view(state->heap, array, &view) != ST_HEAP_OK
            || view.class_descriptor->class_id
                != state->argument_array_class_id
            || view.shape_descriptor->shape_id
                != state->argument_array_shape_id
            || view.indexed_length > UINT32_MAX)
        return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;

    const st_value_t *values = view.indexed_elements;
    for (size_t index = 0u; index < view.indexed_length; ++index) {
        if (!value_valid(state, values[index]))
            return ST_AOT_CLOSURE_ERR_INVALID_CAPTURE;
    }
    *arguments_out = values;
    *argument_count_out = (uint32_t)view.indexed_length;
    return ST_AOT_CLOSURE_OK;
}

_Noreturn st_value_t st_aot_closure_contract_violation(
    st_aot_closure_status_t status, StFrame *frame)
{
    (void)frame;
    (void)status;
    abort();
}

const char *st_aot_closure_status_string(st_aot_closure_status_t status)
{
    switch (status) {
    case ST_AOT_CLOSURE_OK: return "ok";
    case ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_AOT_CLOSURE_ERR_INVALID_CONTEXT: return "invalid closure context";
    case ST_AOT_CLOSURE_ERR_INVALID_FRAME: return "invalid frame";
    case ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR: return "invalid block descriptor";
    case ST_AOT_CLOSURE_ERR_INVALID_CLOSURE: return "invalid closure";
    case ST_AOT_CLOSURE_ERR_WRONG_ARITY: return "wrong closure arity";
    case ST_AOT_CLOSURE_ERR_INVALID_CAPTURE: return "invalid capture";
    case ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_AOT_CLOSURE_ERR_HOME_REQUIRED: return "home is required";
    case ST_AOT_CLOSURE_ERR_HOME_RETURNED: return "home has returned";
    case ST_AOT_CLOSURE_ERR_BLOCK_RETURNED: return "block home has returned";
    case ST_AOT_CLOSURE_ERR_UNSUPPORTED: return "unsupported closure feature";
    case ST_AOT_CLOSURE_ERR_BUSY: return "closure context is busy";
    case ST_AOT_CLOSURE_ERR_HEAP: return "heap operation failed";
    }
    return "invalid closure status";
}
