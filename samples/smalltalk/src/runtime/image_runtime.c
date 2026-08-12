#include "st_image_runtime.h"
#include "st_send_bridge.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define ST_IMAGE_RUNTIME_MAGIC UINT64_C(0x5354494d47525431)
#define ST_IMAGE_BITS_PER_WORD 64u

struct st_image_runtime_state {
    uint64_t magic;
    const st_runtime_descriptors_t *descriptors;
    st_runtime_allocator_t allocator;
    st_heap_t owned_heap;
    st_value_t *roots;
    size_t root_count;
    size_t global_count;
    size_t literal_count;
    uint64_t *global_initialized;
    size_t global_bitmap_words;
    uint64_t *literal_initialized;
    size_t literal_bitmap_words;
    void *root_block;
    size_t root_block_size;
    void *bitmap_block;
    size_t bitmap_block_size;
    size_t state_block_size;
    st_image_string_layout_t string_layout;
    st_image_external_stream_layout_t external_stream_layout;
    const st_image_root_provider_t **root_providers;
    size_t root_provider_count;
    size_t root_provider_capacity;
    void *root_provider_block;
    size_t root_provider_block_size;
    st_heap_root_set_t *collection_root_sets;
    void *collection_root_set_block;
    size_t collection_root_set_block_size;
};

static bool is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool add_size(size_t left, size_t right, size_t *result_out)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result_out = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static bool round_up(size_t value, size_t alignment, size_t *result_out)
{
    size_t mask;
    if (!is_power_of_two(alignment)) {
        return false;
    }
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *result_out = (value + mask) & ~mask;
    return true;
}

static void *default_allocate(void *user, size_t alignment, size_t size)
{
    (void)user;
    return aligned_alloc(alignment, size);
}

static void default_deallocate(void *user, void *pointer, size_t alignment,
                               size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    free(pointer);
}

static bool normalize_allocator(st_runtime_allocator_t input,
                                st_runtime_allocator_t *output)
{
    if (output == NULL
            || ((input.allocate == NULL) != (input.deallocate == NULL)))
        return false;
    if (input.allocate == NULL) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static void *allocate_block(st_runtime_allocator_t allocator,
                            size_t requested_size, size_t *block_size_out)
{
    size_t block_size;
    void *block;
    if (block_size_out == NULL || requested_size == 0u
            || !round_up(requested_size, _Alignof(max_align_t), &block_size))
        return NULL;
    block = allocator.allocate(allocator.user, _Alignof(max_align_t),
                               block_size);
    if (block == NULL) {
        return NULL;
    }
    if (((uintptr_t)block & (_Alignof(max_align_t) - 1u)) != 0u) {
        allocator.deallocate(allocator.user, block, _Alignof(max_align_t),
                             block_size);
        return NULL;
    }
    memset(block, 0, block_size);
    *block_size_out = block_size;
    return block;
}

static void deallocate_block(st_runtime_allocator_t allocator, void *block,
                             size_t block_size)
{
    if (block != NULL) {
        allocator.deallocate(allocator.user, block, _Alignof(max_align_t),
                             block_size);
    }
}

static bool bitmap_get(const uint64_t *bitmap, size_t index)
{
    return (bitmap[index >> 6]
            & (UINT64_C(1) << (index & 63u))) != 0u;
}

static void bitmap_set(uint64_t *bitmap, size_t index)
{
    bitmap[index >> 6] |= UINT64_C(1) << (index & 63u);
}

static st_image_runtime_state_t *runtime_state(
    const st_image_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->initialized
            || runtime->abi_version != ST_IMAGE_RUNTIME_ABI_VERSION
            || runtime->state == NULL
            || runtime->state->magic != ST_IMAGE_RUNTIME_MAGIC
            || runtime->heap == NULL)
        return NULL;
    return runtime->state;
}

static bool layout_pair_is_absent(uint32_t class_id, uint32_t shape_id)
{
    return class_id == 0u && shape_id == 0u;
}

static bool string_layout_is_valid(
    const st_runtime_descriptors_t *descriptors,
    st_image_string_layout_t layout)
{
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    if (layout_pair_is_absent(layout.class_id, layout.shape_id)) {
        return true;
    }
    class_descriptor = st_runtime_class(descriptors, layout.class_id);
    shape = st_runtime_shape(descriptors, layout.shape_id);
    return class_descriptor != NULL && shape != NULL
        && shape->class_id == layout.class_id
        && shape->fixed_word_count == 0u
        && shape->indexed_format == ST_INDEXED_UINT8;
}

static bool external_stream_layout_is_valid(
    const st_runtime_descriptors_t *descriptors,
    st_image_external_stream_layout_t layout)
{
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    size_t word;
    uint64_t mask;
    if (layout_pair_is_absent(layout.class_id, layout.shape_id)) {
        return layout.descriptor_fixed_word_index == 0u;
    }
    class_descriptor = st_runtime_class(descriptors, layout.class_id);
    shape = st_runtime_shape(descriptors, layout.shape_id);
    if (class_descriptor == NULL || shape == NULL
            || shape->class_id != layout.class_id
            || shape->indexed_format != ST_INDEXED_NONE
            || layout.descriptor_fixed_word_index >= shape->fixed_word_count)
        return false;
    word = layout.descriptor_fixed_word_index >> 6;
    mask = UINT64_C(1) << (layout.descriptor_fixed_word_index & 63u);
    return word < shape->fixed_pointer_bitmap_word_count
        && (shape->fixed_pointer_bitmap[word] & mask) != 0u;
}

static st_image_runtime_status_t validate_value(const st_heap_t *heap,
                                                 st_value_t value)
{
    if (!st_value_has_valid_encoding(value)) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_VALUE;
    }
    if (st_value_kind(value) == ST_VALUE_OBJECT) {
        st_object_extent_t extent;
        st_heap_status_t status = st_heap_authorize(heap, value, &extent);
        (void)extent;
        if (status == ST_HEAP_ERR_NOT_MEMBER) {
            return ST_IMAGE_RUNTIME_ERR_NOT_MEMBER;
        }
        if (status != ST_HEAP_OK) {
            return ST_IMAGE_RUNTIME_ERR_HEAP;
        }
    }
    return ST_IMAGE_RUNTIME_OK;
}

static st_image_runtime_status_t populate_table(
    const st_image_runtime_entry_t *entries, size_t count,
    st_value_t *values, uint64_t *initialized, uint64_t *seen,
    const st_heap_t *heap)
{
    size_t index;
    size_t bitmap_words = count == 0u ? 0u
        : (count + (ST_IMAGE_BITS_PER_WORD - 1u)) / ST_IMAGE_BITS_PER_WORD;
    if ((entries == NULL) != (count == 0u)) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (bitmap_words != 0u) {
        memset(seen, 0, bitmap_words * sizeof(*seen));
    }
    for (index = 0u; index < count; index++) {
        uint32_t id = entries[index].id;
        size_t slot;
        st_image_runtime_status_t status;
        if (id == 0u || (size_t)id > count) {
            return ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE;
        }
        slot = (size_t)id - 1u;
        if (bitmap_get(seen, slot)) {
            return ST_IMAGE_RUNTIME_ERR_DUPLICATE_ID;
        }
        bitmap_set(seen, slot);
        values[slot] = st_value_nil();
        if (entries[index].value == (st_value_t)ST_VALUE_INVALID) {
            continue;
        }
        status = validate_value(heap, entries[index].value);
        if (status != ST_IMAGE_RUNTIME_OK) {
            return status;
        }
        values[slot] = entries[index].value;
        bitmap_set(initialized, slot);
    }
    return ST_IMAGE_RUNTIME_OK;
}

static st_image_runtime_status_t heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK:
        return ST_IMAGE_RUNTIME_OK;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW:
        return ST_IMAGE_RUNTIME_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_MEMBER:
        return ST_IMAGE_RUNTIME_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    default:
        return ST_IMAGE_RUNTIME_ERR_HEAP;
    }
}

st_image_runtime_status_t st_image_runtime_init(
    st_image_runtime_t *runtime, const st_image_runtime_options_t *options)
{
    st_runtime_allocator_t allocator;
    st_image_runtime_state_t *state = NULL;
    st_heap_t *heap;
    size_t total_count;
    size_t root_bytes;
    size_t global_words;
    size_t literal_words;
    size_t max_words;
    size_t bitmap_words;
    size_t bitmap_bytes;
    uint64_t *seen;
    st_image_runtime_status_t status;
    bool owned_heap_ready = false;
    if (runtime == NULL || options == NULL || runtime->initialized
            || runtime->state != NULL || runtime->heap != NULL
            || options->descriptors == NULL
            || st_runtime_descriptors_validate(options->descriptors)
                != ST_RUNTIME_OK
            || !normalize_allocator(options->table_allocator, &allocator)
            || !string_layout_is_valid(options->descriptors,
                                       options->string_layout)
            || !external_stream_layout_is_valid(
                options->descriptors, options->external_stream_layout))
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    if (options->global_count > UINT32_MAX
            || options->literal_count > UINT32_MAX
            || !add_size(options->global_count, options->literal_count,
                         &total_count)
            || !multiply_size(total_count, sizeof(st_value_t), &root_bytes)) {
        return ST_IMAGE_RUNTIME_ERR_OVERFLOW;
    }
    global_words = options->global_count == 0u ? 0u
        : (options->global_count + 63u) / 64u;
    literal_words = options->literal_count == 0u ? 0u
        : (options->literal_count + 63u) / 64u;
    max_words = global_words > literal_words ? global_words : literal_words;
    if (!add_size(global_words, literal_words, &bitmap_words)
            || !add_size(bitmap_words, max_words, &bitmap_words)
            || !multiply_size(bitmap_words, sizeof(uint64_t), &bitmap_bytes)) {
        return ST_IMAGE_RUNTIME_ERR_OVERFLOW;
    }
    state = allocate_block(allocator, sizeof(*state),
                           &(size_t){0});
    if (state == NULL) {
        return ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    /* Preserve the actual rounded size rather than relying on compound
     * storage after allocation. */
    if (!round_up(sizeof(*state), _Alignof(max_align_t),
                  &state->state_block_size)) {
        deallocate_block(allocator, state, 0u);
        return ST_IMAGE_RUNTIME_ERR_OVERFLOW;
    }
    state->allocator = allocator;
    state->descriptors = options->descriptors;
    state->global_count = options->global_count;
    state->literal_count = options->literal_count;
    state->root_count = total_count;
    state->global_bitmap_words = global_words;
    state->literal_bitmap_words = literal_words;
    state->string_layout = options->string_layout;
    state->external_stream_layout = options->external_stream_layout;
    if (options->borrowed_heap != NULL) {
        if (st_heap_descriptors(options->borrowed_heap)
                != options->descriptors) {
            status = ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
            goto failure;
        }
        heap = options->borrowed_heap;
    } else {
        st_heap_status_t heap_init_status = st_heap_init(
            &state->owned_heap, options->descriptors,
            options->heap_allocator);
        if (heap_init_status != ST_HEAP_OK) {
            status = heap_status(heap_init_status);
            goto failure;
        }
        owned_heap_ready = true;
        heap = &state->owned_heap;
    }
    if (root_bytes != 0u) {
        state->root_block = allocate_block(allocator, root_bytes,
                                           &state->root_block_size);
        if (state->root_block == NULL) {
            status = ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY;
            goto failure;
        }
        state->roots = state->root_block;
        for (size_t index = 0u; index < total_count; index++) {
            state->roots[index] = st_value_nil();
        }
    }
    if (bitmap_bytes != 0u) {
        state->bitmap_block = allocate_block(allocator, bitmap_bytes,
                                             &state->bitmap_block_size);
        if (state->bitmap_block == NULL) {
            status = ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY;
            goto failure;
        }
        state->global_initialized = state->bitmap_block;
        state->literal_initialized = state->global_initialized + global_words;
        seen = state->literal_initialized + literal_words;
    } else {
        seen = NULL;
    }
    status = populate_table(options->globals, options->global_count,
                            state->roots, state->global_initialized, seen,
                            heap);
    if (status != ST_IMAGE_RUNTIME_OK) {
        goto failure;
    }
    status = populate_table(options->literals, options->literal_count,
                            state->roots + options->global_count,
                            state->literal_initialized, seen, heap);
    if (status != ST_IMAGE_RUNTIME_OK) {
        goto failure;
    }
    state->magic = ST_IMAGE_RUNTIME_MAGIC;
    runtime->abi_version = ST_IMAGE_RUNTIME_ABI_VERSION;
    runtime->initialized = true;
    runtime->owns_heap = options->borrowed_heap == NULL;
    runtime->heap = heap;
    runtime->state = state;
    return ST_IMAGE_RUNTIME_OK;

failure:
    deallocate_block(allocator, state->bitmap_block,
                     state->bitmap_block_size);
    deallocate_block(allocator, state->root_block, state->root_block_size);
    if (owned_heap_ready) {
        st_heap_destroy(&state->owned_heap);
    }
    deallocate_block(allocator, state, state->state_block_size);
    return status;
}

void st_image_runtime_destroy(st_image_runtime_t *runtime)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    if (state == NULL) {
        return;
    }
    if (state->root_provider_count != 0u) {
        abort();
    }
    state->magic = 0u;
    deallocate_block(state->allocator, state->bitmap_block,
                     state->bitmap_block_size);
    deallocate_block(state->allocator, state->root_block,
                     state->root_block_size);
    deallocate_block(state->allocator, state->root_provider_block,
                     state->root_provider_block_size);
    deallocate_block(state->allocator, state->collection_root_set_block,
                     state->collection_root_set_block_size);
    if (runtime->owns_heap) {
        st_heap_destroy(&state->owned_heap);
    }
    st_runtime_allocator_t allocator = state->allocator;
    size_t state_size = state->state_block_size;
    memset(runtime, 0, sizeof(*runtime));
    deallocate_block(allocator, state, state_size);
}

bool st_image_runtime_root_provider_attach(
    st_image_runtime_t *runtime, const st_image_root_provider_t *provider)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    const st_image_root_provider_t **providers;
    st_heap_root_set_t *root_sets;
    size_t capacity;
    size_t provider_bytes;
    size_t root_set_bytes;
    size_t provider_block_size;
    size_t root_set_block_size;
    const st_value_t *roots = NULL;
    size_t count = 0u;
    if (state == NULL || provider == NULL
            || provider->abi_version != ST_IMAGE_ROOT_PROVIDER_ABI_VERSION
            || provider->owner == NULL || provider->roots == NULL
            || provider->roots(provider->owner, &roots, &count)
                != ST_IMAGE_RUNTIME_OK
            || ((roots == NULL) != (count == 0u)))
        return false;
    for (size_t index = 0u; index < state->root_provider_count; index++) {
        if (state->root_providers[index] == provider) {
            return false;
        }
    }
    if (state->root_provider_count == state->root_provider_capacity) {
        capacity = state->root_provider_capacity == 0u
            ? 4u : state->root_provider_capacity * 2u;
        if (capacity < state->root_provider_capacity
                || !multiply_size(capacity, sizeof(*providers),
                                  &provider_bytes)
                || !multiply_size(capacity + 1u, sizeof(*root_sets),
                                  &root_set_bytes))
            return false;
        providers = allocate_block(state->allocator, provider_bytes,
                                   &provider_block_size);
        if (providers == NULL) {
            return false;
        }
        root_sets = allocate_block(state->allocator, root_set_bytes,
                                  &root_set_block_size);
        if (root_sets == NULL) {
            deallocate_block(state->allocator, providers,
                             provider_block_size);
            return false;
        }
        if (state->root_provider_count != 0u) {
            memcpy(providers, state->root_providers,
                   state->root_provider_count * sizeof(*providers));
        }
        deallocate_block(state->allocator, state->root_provider_block,
                         state->root_provider_block_size);
        deallocate_block(state->allocator,
                         state->collection_root_set_block,
                         state->collection_root_set_block_size);
        state->root_providers = providers;
        state->root_provider_block = providers;
        state->root_provider_block_size = provider_block_size;
        state->collection_root_sets = root_sets;
        state->collection_root_set_block = root_sets;
        state->collection_root_set_block_size = root_set_block_size;
        state->root_provider_capacity = capacity;
    }
    state->root_providers[state->root_provider_count++] = provider;
    return true;
}

bool st_image_runtime_root_provider_detach(
    st_image_runtime_t *runtime, const st_image_root_provider_t *provider)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    size_t index;
    if (state == NULL || provider == NULL) {
        return false;
    }
    for (index = 0u; index < state->root_provider_count; index++) {
        if (state->root_providers[index] != provider) {
            continue;
        }
        state->root_provider_count--;
        if (index != state->root_provider_count) {
            memmove(&state->root_providers[index],
                    &state->root_providers[index + 1u],
                    (state->root_provider_count - index)
                        * sizeof(*state->root_providers));
        }
        state->root_providers[state->root_provider_count] = NULL;
        return true;
    }
    return false;
}

const st_image_root_provider_t *st_image_runtime_root_provider(
    const st_image_runtime_t *runtime)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    return state == NULL || state->root_provider_count == 0u
        ? NULL : state->root_providers[0];
}

bool st_image_runtime_root_provider_contains(
    const st_image_runtime_t *runtime,
    const st_image_root_provider_t *provider)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    if (state == NULL || provider == NULL) {
        return false;
    }
    for (size_t index = 0u; index < state->root_provider_count; index++) {
        if (state->root_providers[index] == provider) {
            return true;
        }
    }
    return false;
}

const st_image_root_provider_t *st_image_runtime_root_provider_find(
    const st_image_runtime_t *runtime, st_image_root_span_fn roots)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    if (state == NULL || roots == NULL) {
        return NULL;
    }
    for (size_t index = 0u; index < state->root_provider_count; index++) {
        if (state->root_providers[index]->roots == roots) {
            return state->root_providers[index];
        }
    }
    return NULL;
}

static st_image_runtime_status_t load_from_frame(
    StFrame *frame, uint32_t index, bool literal, st_value_t *result_out)
{
    st_aot_thread_t *thread;
    st_image_runtime_state_t *state;
    size_t count;
    size_t offset;
    uint64_t *initialized;
    st_image_runtime_status_t status;
    if (result_out != NULL) {
        *result_out = (st_value_t)ST_VALUE_INVALID;
    }
    if (frame == NULL || result_out == NULL || frame->thread == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    thread = frame->thread;
    if (thread->abi_version != ST_AOT_THREAD_ABI_VERSION
            || !thread->initialized || thread->image == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
    state = runtime_state(thread->image);
    if (state == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
    }
    count = literal ? state->literal_count : state->global_count;
    offset = literal ? state->global_count : 0u;
    initialized = literal ? state->literal_initialized
                          : state->global_initialized;
    if ((size_t)index >= count) {
        return ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE;
    }
    if (!bitmap_get(initialized, index)) {
        return ST_IMAGE_RUNTIME_ERR_UNINITIALIZED;
    }
    status = validate_value(thread->image->heap, state->roots[offset + index]);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status;
    }
    *result_out = state->roots[offset + index];
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_global_load(
    StFrame *frame, uint32_t index, st_value_t *result_out)
{
    return load_from_frame(frame, index, false, result_out);
}

st_image_runtime_status_t st_image_runtime_literal_load(
    StFrame *frame, uint32_t index, st_value_t *result_out)
{
    return load_from_frame(frame, index, true, result_out);
}

_Noreturn st_value_t st_aot_image_runtime_contract_violation(
    st_image_runtime_status_t status, const StFrame *frame)
{
    (void)status;
    (void)frame;
    abort();
}

static st_image_runtime_status_t publish_allocated(
    st_image_runtime_t *runtime, bool literal, uint32_t index,
    uint32_t class_id, uint32_t shape_id, size_t indexed_length,
    st_header_flags_t flags, st_value_t *value_out, st_object_view_t *view_out)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    size_t count;
    size_t offset;
    uint64_t *initialized;
    st_value_t value = (st_value_t)ST_VALUE_INVALID;
    st_heap_status_t allocation_status;
    if (value_out != NULL) {
        *value_out = (st_value_t)ST_VALUE_INVALID;
    }
    if (view_out != NULL) {
        memset(view_out, 0, sizeof(*view_out));
    }
    if (state == NULL || value_out == NULL || view_out == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    count = literal ? state->literal_count : state->global_count;
    offset = literal ? state->global_count : 0u;
    initialized = literal ? state->literal_initialized
                          : state->global_initialized;
    if ((size_t)index >= count) {
        return ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE;
    }
    if (bitmap_get(initialized, index)) {
        return ST_IMAGE_RUNTIME_ERR_CONFLICT;
    }
    allocation_status = st_heap_allocate(runtime->heap, class_id, shape_id,
                                         indexed_length, indexed_length,
                                         flags, &value);
    if (allocation_status != ST_HEAP_OK) {
        return heap_status(allocation_status);
    }
    if (st_heap_object_view(runtime->heap, value, view_out) != ST_HEAP_OK) {
        return ST_IMAGE_RUNTIME_ERR_HEAP;
    }
    state->roots[offset + index] = value;
    *value_out = value;
    /* Publication is deliberately last. Callers finish infallible payload
     * initialization before setting this bit. */
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_bootstrap_string_literal(
    st_image_runtime_t *runtime, uint32_t literal_index,
    const void *bytes, size_t byte_count, st_value_t *result_out)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    st_object_view_t view;
    st_value_t value;
    st_image_runtime_status_t status;
    if (result_out != NULL) {
        *result_out = (st_value_t)ST_VALUE_INVALID;
    }
    if (state == NULL || result_out == NULL
            || (bytes == NULL && byte_count != 0u)
            || layout_pair_is_absent(state->string_layout.class_id,
                                     state->string_layout.shape_id))
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    status = publish_allocated(runtime, true, literal_index,
                               state->string_layout.class_id,
                               state->string_layout.shape_id,
                               byte_count, ST_HEADER_IMMUTABLE,
                               &value, &view);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status;
    }
    if (byte_count != 0u) {
        memcpy(view.indexed_elements, bytes, byte_count);
    }
    bitmap_set(state->literal_initialized, literal_index);
    *result_out = value;
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_bootstrap_global_value(
    st_image_runtime_t *runtime, uint32_t global_index, st_value_t value)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    st_image_runtime_status_t status;

    if (state == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)global_index >= state->global_count) {
        return ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE;
    }
    if (bitmap_get(state->global_initialized, global_index)) {
        return ST_IMAGE_RUNTIME_ERR_CONFLICT;
    }
    status = validate_value(runtime->heap, value);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status;
    }
    state->roots[global_index] = value;
    bitmap_set(state->global_initialized, global_index);
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_bootstrap_transcript(
    st_image_runtime_t *runtime, uint32_t global_index,
    st_value_t *result_out)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    st_object_view_t view;
    st_value_t value;
    st_value_t descriptor;
    st_image_runtime_status_t status;
    if (result_out != NULL) {
        *result_out = (st_value_t)ST_VALUE_INVALID;
    }
    if (state == NULL || result_out == NULL
            || layout_pair_is_absent(state->external_stream_layout.class_id,
                                     state->external_stream_layout.shape_id)
            || !st_value_from_small_integer(1, &descriptor))
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    status = publish_allocated(runtime, false, global_index,
                               state->external_stream_layout.class_id,
                               state->external_stream_layout.shape_id,
                               0u, 0u, &value, &view);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status;
    }
    ((st_value_t *)view.fixed_words)
        [state->external_stream_layout.descriptor_fixed_word_index]
            = descriptor;
    bitmap_set(state->global_initialized, global_index);
    *result_out = value;
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_visit_roots(
    const st_image_runtime_t *runtime,
    st_image_runtime_root_visitor_fn visitor, void *user,
    size_t *visited_out)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    size_t visited = 0u;
    if (visited_out != NULL) {
        *visited_out = 0u;
    }
    if (state == NULL || visitor == NULL || visited_out == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    for (size_t index = 0u; index < state->root_count; index++) {
        bool initialized = index < state->global_count
            ? bitmap_get(state->global_initialized, index)
            : bitmap_get(state->literal_initialized,
                         index - state->global_count);
        st_image_runtime_status_t status;
        if (!initialized) {
            continue;
        }
        status = validate_value(runtime->heap, state->roots[index]);
        if (status != ST_IMAGE_RUNTIME_OK) {
            return status;
        }
        if (!visitor(user, &state->roots[index])) {
            return ST_IMAGE_RUNTIME_ERR_VISITOR_ABORTED;
        }
        visited++;
    }
    for (size_t provider_index = 0u;
         provider_index < state->root_provider_count; provider_index++) {
        const st_image_root_provider_t *provider =
            state->root_providers[provider_index];
        const st_value_t *roots = NULL;
        size_t count = 0u;
        st_image_runtime_status_t provider_status = provider->roots(
            provider->owner, &roots, &count);
        if (provider_status != ST_IMAGE_RUNTIME_OK) {
            return provider_status;
        }
        if ((roots == NULL) != (count == 0u)) {
            return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
        }
        for (size_t index = 0u; index < count; index++) {
            st_image_runtime_status_t status =
                validate_value(runtime->heap, roots[index]);
            if (status != ST_IMAGE_RUNTIME_OK) {
                return status;
            }
            if (!visitor(user, &roots[index])) {
                return ST_IMAGE_RUNTIME_ERR_VISITOR_ABORTED;
            }
            visited++;
        }
    }
    *visited_out = visited;
    return ST_IMAGE_RUNTIME_OK;
}

st_image_runtime_status_t st_image_runtime_collect(
    st_image_runtime_t *runtime, const StFrame *top_frame,
    st_heap_collection_stats_t *stats_out)
{
    st_image_runtime_state_t *state = runtime_state(runtime);
    st_heap_status_t status;
    st_heap_root_set_t *root_sets;
    size_t root_set_count = 0u;
    if (state == NULL || stats_out == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (top_frame != NULL) {
        const st_aot_thread_t *thread = top_frame->thread;
        if (thread == NULL || thread->abi_version != ST_AOT_THREAD_ABI_VERSION
                || !thread->initialized || thread->image != runtime) {
            return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
        }
    }
    root_sets = state->collection_root_sets;
    if (state->root_count != 0u) {
        if (root_sets == NULL) {
            /* Provider-free images need no persistent scratch allocation. */
            st_heap_root_set_t image_roots = {
                state->roots, state->root_count
            };
            status = st_heap_collect_root_sets(
                runtime->heap, top_frame, &image_roots, 1u, stats_out);
            return heap_status(status);
        }
        root_sets[root_set_count++] = (st_heap_root_set_t) {
            state->roots, state->root_count
        };
    }
    for (size_t provider_index = 0u;
         provider_index < state->root_provider_count; provider_index++) {
        const st_image_root_provider_t *provider =
            state->root_providers[provider_index];
        const st_value_t *roots = NULL;
        size_t count = 0u;
        st_image_runtime_status_t provider_status = provider->roots(
            provider->owner, &roots, &count);
        if (provider_status != ST_IMAGE_RUNTIME_OK) {
            return provider_status;
        }
        if ((roots == NULL) != (count == 0u)) {
            return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
        }
        if (count != 0u) {
            root_sets[root_set_count++] = (st_heap_root_set_t) {
                roots, count
            };
        }
    }
    status = st_heap_collect_root_sets(
        runtime->heap, top_frame,
        root_set_count == 0u ? NULL : root_sets,
        root_set_count, stats_out);
    return heap_status(status);
}

st_heap_t *st_image_runtime_heap(st_image_runtime_t *runtime)
{
    return runtime_state(runtime) == NULL ? NULL : runtime->heap;
}

const st_heap_t *st_image_runtime_heap_const(
    const st_image_runtime_t *runtime)
{
    return runtime_state(runtime) == NULL ? NULL : runtime->heap;
}

const char *st_image_runtime_status_string(st_image_runtime_status_t status)
{
    switch (status) {
    case ST_IMAGE_RUNTIME_OK:
        return "ok";
    case ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_IMAGE_RUNTIME_ERR_INVALID_STATE:
        return "invalid image state";
    case ST_IMAGE_RUNTIME_ERR_INVALID_DESCRIPTOR:
        return "invalid descriptor";
    case ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE:
        return "ID out of range";
    case ST_IMAGE_RUNTIME_ERR_DUPLICATE_ID:
        return "duplicate ID";
    case ST_IMAGE_RUNTIME_ERR_INVALID_VALUE:
        return "invalid tagged value";
    case ST_IMAGE_RUNTIME_ERR_NOT_MEMBER:
        return "value is not a heap member";
    case ST_IMAGE_RUNTIME_ERR_UNINITIALIZED:
        return "uninitialized slot";
    case ST_IMAGE_RUNTIME_ERR_CONFLICT:
        return "slot already initialized";
    case ST_IMAGE_RUNTIME_ERR_OVERFLOW:
        return "size overflow";
    case ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ST_IMAGE_RUNTIME_ERR_HEAP:
        return "heap failure";
    case ST_IMAGE_RUNTIME_ERR_VISITOR_ABORTED:
        return "visitor aborted";
    default:
        return "unknown image runtime status";
    }
}
