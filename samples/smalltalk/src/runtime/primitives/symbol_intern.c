#include "st_symbol_intern.h"
#include "st_send_bridge.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define ST_SYMBOL_INTERN_MAGIC UINT64_C(0x535453594d494e31)
#define ST_SYMBOL_INITIAL_CAPACITY 16u

struct st_symbol_intern_state {
    uint64_t magic;
    const st_runtime_descriptors_t *descriptors;
    st_runtime_allocator_t allocator;
    st_symbol_hash_fn hash;
    void *hash_user;
    uint32_t string_class_id;
    uint32_t string_shapes[3];
    uint32_t symbol_class_id;
    uint32_t symbol_shapes[3];
    st_value_t *roots;
    uint64_t *hashes;
    size_t count;
    size_t entry_capacity;
    void *entry_block;
    size_t entry_block_size;
    size_t *slots;
    size_t table_capacity;
    size_t table_block_size;
    size_t state_block_size;
};

struct st_symbol_intern_batch_state {
    st_symbol_intern_context_t *context;
    st_runtime_allocator_t allocator;
    st_symbol_intern_state_t *expected_state;
    void *expected_entry_block;
    size_t *expected_slots;
    size_t expected_count;
    void *entry_block;
    size_t entry_block_size;
    st_value_t *roots;
    uint64_t *hashes;
    size_t entry_capacity;
    size_t *slots;
    size_t table_capacity;
    size_t table_block_size;
    size_t prepared_count;
    st_value_t *results;
    size_t result_count;
    size_t result_block_size;
    size_t state_block_size;
    bool committed;
};

static bool is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) return false;
    *result_out = left * right;
    return true;
}

static bool add_size(size_t left, size_t right, size_t *result_out)
{
    if (left > SIZE_MAX - right) return false;
    *result_out = left + right;
    return true;
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
                            size_t requested, size_t *block_size_out)
{
    size_t block_size;
    void *block;
    if (block_size_out == NULL || requested == 0u
            || !round_up(requested, _Alignof(max_align_t), &block_size))
        return NULL;
    block = allocator.allocate(allocator.user, _Alignof(max_align_t),
                               block_size);
    if (block == NULL) return NULL;
    if (((uintptr_t)block & (_Alignof(max_align_t) - 1u)) != 0u) {
        allocator.deallocate(allocator.user, block, _Alignof(max_align_t),
                             block_size);
        return NULL;
    }
    memset(block, 0, block_size);
    *block_size_out = block_size;
    return block;
}

static void release_block(st_runtime_allocator_t allocator, void *block,
                          size_t block_size)
{
    if (block != NULL)
        allocator.deallocate(allocator.user, block, _Alignof(max_align_t),
                             block_size);
}

static st_symbol_intern_state_t *basic_state(
    const st_symbol_intern_context_t *context)
{
    if (context == NULL || !context->initialized
            || context->abi_version != ST_SYMBOL_INTERN_ABI_VERSION
            || context->image == NULL || context->state == NULL
            || context->state->magic != ST_SYMBOL_INTERN_MAGIC)
        return NULL;
    return context->state;
}

static st_symbol_intern_state_t *live_state(
    const st_symbol_intern_context_t *context)
{
    st_symbol_intern_state_t *state = basic_state(context);
    if (state == NULL
            || st_image_runtime_heap(context->image) == NULL
            || !st_image_runtime_root_provider_contains(
                context->image, &context->root_provider))
        return NULL;
    return state;
}

static st_image_runtime_status_t symbol_root_span(
    void *owner, const st_value_t **roots_out, size_t *root_count_out)
{
    st_symbol_intern_context_t *context = owner;
    st_symbol_intern_state_t *state = basic_state(context);
    if (roots_out != NULL) *roots_out = NULL;
    if (root_count_out != NULL) *root_count_out = 0u;
    if (state == NULL || roots_out == NULL || root_count_out == NULL)
        return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
    *roots_out = state->count == 0u ? NULL : state->roots;
    *root_count_out = state->count;
    return ST_IMAGE_RUNTIME_OK;
}

static bool shape_matches(const st_runtime_descriptors_t *descriptors,
                          uint32_t class_id, uint32_t shape_id,
                          st_indexed_format_t format)
{
    const StShapeDescriptor *shape = st_runtime_shape(descriptors, shape_id);
    return shape != NULL && shape->class_id == class_id
        && shape->fixed_word_count == 0u
        && shape->fixed_pointer_bitmap == NULL
        && shape->fixed_pointer_bitmap_word_count == 0u
        && shape->indexed_format == format;
}

static bool layouts_valid(const st_runtime_descriptors_t *descriptors,
                          const st_symbol_intern_options_t *options)
{
    const StClassDescriptor *string_class;
    const StClassDescriptor *symbol_class;
    uint32_t all_shapes[6] = {
        options->string_uint8_shape_id,
        options->string_uint16_shape_id,
        options->string_uint32_shape_id,
        options->symbol_uint8_shape_id,
        options->symbol_uint16_shape_id,
        options->symbol_uint32_shape_id
    };
    if (options->string_class_id == 0u || options->symbol_class_id == 0u
            || options->string_class_id == options->symbol_class_id)
        return false;
    for (size_t index = 0u; index < 6u; index++) {
        if (all_shapes[index] == 0u) return false;
        for (size_t previous = 0u; previous < index; previous++)
            if (all_shapes[previous] == all_shapes[index]) return false;
    }
    string_class = st_runtime_class(descriptors, options->string_class_id);
    symbol_class = st_runtime_class(descriptors, options->symbol_class_id);
    if (string_class == NULL || symbol_class == NULL
            || (string_class->flags & (ST_CLASS_METACLASS
                                       | ST_CLASS_ABSTRACT)) != 0u
            || (symbol_class->flags & ST_CLASS_METACLASS) != 0u
            || (symbol_class->flags & ST_CLASS_ABSTRACT) == 0u)
        return false;
    return shape_matches(descriptors, options->string_class_id,
                         options->string_uint8_shape_id, ST_INDEXED_UINT8)
        && shape_matches(descriptors, options->string_class_id,
                         options->string_uint16_shape_id, ST_INDEXED_UINT16)
        && shape_matches(descriptors, options->string_class_id,
                         options->string_uint32_shape_id, ST_INDEXED_UINT32)
        && shape_matches(descriptors, options->symbol_class_id,
                         options->symbol_uint8_shape_id, ST_INDEXED_UINT8)
        && shape_matches(descriptors, options->symbol_class_id,
                         options->symbol_uint16_shape_id, ST_INDEXED_UINT16)
        && shape_matches(descriptors, options->symbol_class_id,
                         options->symbol_uint32_shape_id, ST_INDEXED_UINT32);
}

static st_symbol_intern_status_t allocate_entries(
    st_runtime_allocator_t allocator, size_t entry_capacity,
    void **block_out, size_t *block_size_out, st_value_t **roots_out,
    uint64_t **hashes_out)
{
    size_t values_size;
    size_t hashes_size;
    size_t total;
    void *block;
    *block_out = NULL;
    *block_size_out = 0u;
    *roots_out = NULL;
    *hashes_out = NULL;
    if (!multiply_size(entry_capacity, sizeof(st_value_t), &values_size)
            || !multiply_size(entry_capacity, sizeof(uint64_t), &hashes_size)
            || !add_size(values_size, hashes_size, &total))
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    block = allocate_block(allocator, total, block_size_out);
    if (block == NULL) return ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
    *block_out = block;
    *roots_out = block;
    *hashes_out = (uint64_t *)((unsigned char *)block + values_size);
    return ST_SYMBOL_INTERN_OK;
}

st_symbol_intern_status_t st_symbol_intern_context_init(
    st_symbol_intern_context_t *context,
    const st_symbol_intern_options_t *options)
{
    st_runtime_allocator_t allocator;
    st_symbol_intern_state_t *state = NULL;
    st_heap_t *heap;
    const st_runtime_descriptors_t *descriptors;
    size_t table_capacity;
    size_t table_bytes;
    size_t entry_capacity;
    st_symbol_intern_status_t status;
    if (context == NULL || options == NULL || context->initialized
            || context->state != NULL || context->image != NULL
            || options->image == NULL
            || !normalize_allocator(options->allocator, &allocator))
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    heap = st_image_runtime_heap(options->image);
    descriptors = heap == NULL ? NULL : st_heap_descriptors(heap);
    if (descriptors == NULL || !layouts_valid(descriptors, options))
        return ST_SYMBOL_INTERN_ERR_INVALID_DESCRIPTOR;
    table_capacity = options->initial_table_capacity == 0u
        ? ST_SYMBOL_INITIAL_CAPACITY : options->initial_table_capacity;
    if (table_capacity < 4u || !is_power_of_two(table_capacity))
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    entry_capacity = table_capacity - table_capacity / 4u;
    if (entry_capacity == 0u
            || !multiply_size(table_capacity, sizeof(size_t), &table_bytes))
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    state = allocate_block(allocator, sizeof(*state), &(size_t){0});
    if (state == NULL) return ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
    if (!round_up(sizeof(*state), _Alignof(max_align_t),
                  &state->state_block_size)) {
        release_block(allocator, state, 0u);
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    }
    state->allocator = allocator;
    state->descriptors = descriptors;
    state->hash = options->hash;
    state->hash_user = options->hash_user;
    state->string_class_id = options->string_class_id;
    state->string_shapes[0] = options->string_uint8_shape_id;
    state->string_shapes[1] = options->string_uint16_shape_id;
    state->string_shapes[2] = options->string_uint32_shape_id;
    state->symbol_class_id = options->symbol_class_id;
    state->symbol_shapes[0] = options->symbol_uint8_shape_id;
    state->symbol_shapes[1] = options->symbol_uint16_shape_id;
    state->symbol_shapes[2] = options->symbol_uint32_shape_id;
    state->entry_capacity = entry_capacity;
    state->table_capacity = table_capacity;
    status = allocate_entries(
        allocator, entry_capacity, &state->entry_block,
        &state->entry_block_size, &state->roots, &state->hashes);
    if (status != ST_SYMBOL_INTERN_OK) goto failure;
    state->slots = allocate_block(
        allocator, table_bytes, &state->table_block_size);
    if (state->slots == NULL) {
        status = ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
        goto failure;
    }
    state->magic = ST_SYMBOL_INTERN_MAGIC;
    context->abi_version = ST_SYMBOL_INTERN_ABI_VERSION;
    context->initialized = true;
    context->image = options->image;
    context->state = state;
    context->root_provider = (st_image_root_provider_t) {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION, context, symbol_root_span
    };
    if (!st_image_runtime_root_provider_attach(
            context->image, &context->root_provider)) {
        status = ST_SYMBOL_INTERN_ERR_CONFLICT;
        memset(context, 0, sizeof(*context));
        goto failure;
    }
    return ST_SYMBOL_INTERN_OK;

failure:
    release_block(allocator, state->slots, state->table_block_size);
    release_block(allocator, state->entry_block, state->entry_block_size);
    release_block(allocator, state, state->state_block_size);
    return status;
}

void st_symbol_intern_context_destroy(st_symbol_intern_context_t *context)
{
    st_symbol_intern_state_t *state = basic_state(context);
    st_runtime_allocator_t allocator;
    size_t state_size;
    if (state == NULL) return;
    if (st_image_runtime_heap(context->image) != NULL
            && !st_image_runtime_root_provider_detach(
                context->image, &context->root_provider))
        abort();
    state->magic = 0u;
    release_block(state->allocator, state->slots, state->table_block_size);
    release_block(state->allocator, state->entry_block,
                  state->entry_block_size);
    allocator = state->allocator;
    state_size = state->state_block_size;
    memset(context, 0, sizeof(*context));
    release_block(allocator, state, state_size);
}

static uint32_t load_code_point(const st_object_view_t *view, size_t index)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        return ((const uint8_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT16:
        return ((const uint16_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT32:
        return ((const uint32_t *)view->indexed_elements)[index];
    default: return UINT32_MAX;
    }
}

static bool unicode_scalar_is_valid(uint32_t code_point)
{
    return code_point <= UINT32_C(0x10ffff)
        && !(code_point >= UINT32_C(0xd800)
             && code_point <= UINT32_C(0xdfff));
}

static bool shape_in(const uint32_t shapes[3], uint32_t shape_id)
{
    return shape_id == shapes[0] || shape_id == shapes[1]
        || shape_id == shapes[2];
}

static st_symbol_intern_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK: return ST_SYMBOL_INTERN_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_SYMBOL_INTERN_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW: return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_MEMBER: return ST_SYMBOL_INTERN_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_SYMBOL_INTERN_ERR_TYPE_MISMATCH;
    default: return ST_SYMBOL_INTERN_ERR_BAD_OBJECT;
    }
}

static st_symbol_intern_status_t sequence_view(
    const st_symbol_intern_context_t *context, st_value_t value,
    st_object_view_t *view_out, uint32_t *maximum_out)
{
    st_symbol_intern_state_t *state = live_state(context);
    st_heap_status_t heap_status;
    bool symbol;
    uint32_t maximum = 0u;
    if (state == NULL) return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
    if (!st_value_has_valid_encoding(value))
        return ST_SYMBOL_INTERN_ERR_INVALID_VALUE;
    heap_status = st_heap_object_view(
        st_image_runtime_heap(context->image), value, view_out);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    symbol = view_out->class_descriptor->class_id == state->symbol_class_id;
    if (!((view_out->class_descriptor->class_id == state->string_class_id
                && shape_in(state->string_shapes,
                            view_out->shape_descriptor->shape_id))
            || (symbol && shape_in(state->symbol_shapes,
                                   view_out->shape_descriptor->shape_id))))
        return ST_SYMBOL_INTERN_ERR_TYPE_MISMATCH;
    if (symbol && (st_object_header_flags(
            st_object_header_load(&view_out->object->header))
            & ST_HEADER_IMMUTABLE) == 0u)
        return ST_SYMBOL_INTERN_ERR_BAD_OBJECT;
    for (size_t index = 0u; index < view_out->indexed_length; index++) {
        uint32_t code_point = load_code_point(view_out, index);
        if (!unicode_scalar_is_valid(code_point))
            return ST_SYMBOL_INTERN_ERR_BAD_OBJECT;
        if (code_point > maximum) maximum = code_point;
    }
    if (maximum_out != NULL) *maximum_out = maximum;
    return ST_SYMBOL_INTERN_OK;
}

static uint64_t default_hash(const st_object_view_t *view)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t index = 0u; index < view->indexed_length; index++) {
        uint32_t code_point = load_code_point(view, index);
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            hash ^= (uint8_t)(code_point >> shift);
            hash *= UINT64_C(0x100000001b3);
        }
    }
    hash ^= (uint64_t)view->indexed_length;
    hash *= UINT64_C(0x9e3779b185ebca87);
    hash ^= hash >> 32;
    return hash;
}

static bool sequences_equal(const st_object_view_t *left,
                            const st_object_view_t *right)
{
    if (left->indexed_length != right->indexed_length) return false;
    for (size_t index = 0u; index < left->indexed_length; index++)
        if (load_code_point(left, index) != load_code_point(right, index))
            return false;
    return true;
}

static size_t probe_distance(uint64_t hash, size_t slot, size_t capacity)
{
    return (slot - ((size_t)hash & (capacity - 1u))) & (capacity - 1u);
}

static st_symbol_intern_status_t table_lookup(
    const st_symbol_intern_context_t *context,
    const st_object_view_t *input, uint64_t hash,
    st_value_t *value_out, bool *found_out)
{
    st_symbol_intern_state_t *state = live_state(context);
    size_t slot;
    size_t distance = 0u;
    if (value_out != NULL) *value_out = ST_VALUE_INVALID;
    if (found_out != NULL) *found_out = false;
    if (state == NULL || value_out == NULL || found_out == NULL)
        return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
    slot = (size_t)hash & (state->table_capacity - 1u);
    while (distance < state->table_capacity) {
        size_t encoded = state->slots[slot];
        size_t entry_index;
        st_object_view_t symbol;
        st_symbol_intern_status_t status;
        if (encoded == 0u) return ST_SYMBOL_INTERN_OK;
        entry_index = encoded - 1u;
        if (entry_index >= state->count)
            return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
        if (probe_distance(state->hashes[entry_index], slot,
                           state->table_capacity) < distance)
            return ST_SYMBOL_INTERN_OK;
        if (state->hashes[entry_index] == hash) {
            status = sequence_view(
                context, state->roots[entry_index], &symbol, NULL);
            if (status != ST_SYMBOL_INTERN_OK) return status;
            if (symbol.class_descriptor->class_id != state->symbol_class_id)
                return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
            if (sequences_equal(input, &symbol)) {
                *value_out = state->roots[entry_index];
                *found_out = true;
                return ST_SYMBOL_INTERN_OK;
            }
        }
        slot = (slot + 1u) & (state->table_capacity - 1u);
        distance++;
    }
    return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
}

static bool table_insert(size_t *slots, size_t capacity,
                         const uint64_t *hashes, size_t entry_index)
{
    size_t encoded = entry_index + 1u;
    uint64_t hash = hashes[entry_index];
    size_t slot = (size_t)hash & (capacity - 1u);
    size_t distance = 0u;
    while (distance < capacity) {
        if (slots[slot] == 0u) {
            slots[slot] = encoded;
            return true;
        }
        size_t resident_index = slots[slot] - 1u;
        size_t resident_distance = probe_distance(
            hashes[resident_index], slot, capacity);
        if (resident_distance < distance) {
            size_t displaced = slots[slot];
            slots[slot] = encoded;
            encoded = displaced;
            entry_index = encoded - 1u;
            distance = resident_distance;
        }
        slot = (slot + 1u) & (capacity - 1u);
        distance++;
    }
    return false;
}

static void store_code_point(st_object_view_t *view, size_t index,
                             uint32_t code_point)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        ((uint8_t *)view->indexed_elements)[index] = (uint8_t)code_point;
        return;
    case ST_INDEXED_UINT16:
        ((uint16_t *)view->indexed_elements)[index] = (uint16_t)code_point;
        return;
    case ST_INDEXED_UINT32:
        ((uint32_t *)view->indexed_elements)[index] = code_point;
        return;
    default: abort();
    }
}

static bool utf8_next(const unsigned char *bytes, size_t length,
                      size_t *offset_in_out, uint32_t *scalar_out)
{
    size_t offset = *offset_in_out;
    uint32_t scalar;
    unsigned char first;
    if (offset >= length) return false;
    first = bytes[offset++];
    if (first < UINT8_C(0x80)) {
        scalar = first;
    } else if (first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf)) {
        if (offset >= length || (bytes[offset] & UINT8_C(0xc0))
                                  != UINT8_C(0x80))
            return false;
        scalar = ((uint32_t)(first & UINT8_C(0x1f)) << 6)
            | (uint32_t)(bytes[offset++] & UINT8_C(0x3f));
    } else if (first >= UINT8_C(0xe0) && first <= UINT8_C(0xef)) {
        unsigned char second;
        if (length - offset < 2u) return false;
        second = bytes[offset];
        if ((second & UINT8_C(0xc0)) != UINT8_C(0x80)
                || (bytes[offset + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80)
                || (first == UINT8_C(0xe0) && second < UINT8_C(0xa0))
                || (first == UINT8_C(0xed) && second >= UINT8_C(0xa0)))
            return false;
        scalar = ((uint32_t)(first & UINT8_C(0x0f)) << 12)
            | ((uint32_t)(second & UINT8_C(0x3f)) << 6)
            | (uint32_t)(bytes[offset + 1u] & UINT8_C(0x3f));
        offset += 2u;
    } else if (first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4)) {
        unsigned char second;
        if (length - offset < 3u) return false;
        second = bytes[offset];
        if ((second & UINT8_C(0xc0)) != UINT8_C(0x80)
                || (bytes[offset + 1u] & UINT8_C(0xc0)) != UINT8_C(0x80)
                || (bytes[offset + 2u] & UINT8_C(0xc0)) != UINT8_C(0x80)
                || (first == UINT8_C(0xf0) && second < UINT8_C(0x90))
                || (first == UINT8_C(0xf4) && second >= UINT8_C(0x90)))
            return false;
        scalar = ((uint32_t)(first & UINT8_C(0x07)) << 18)
            | ((uint32_t)(second & UINT8_C(0x3f)) << 12)
            | ((uint32_t)(bytes[offset + 1u] & UINT8_C(0x3f)) << 6)
            | (uint32_t)(bytes[offset + 2u] & UINT8_C(0x3f));
        offset += 3u;
    } else {
        return false;
    }
    *offset_in_out = offset;
    *scalar_out = scalar;
    return true;
}

static bool utf8_measure(st_symbol_utf8_t spelling, size_t *count_out,
                         uint32_t *maximum_out)
{
    const unsigned char *bytes = (const unsigned char *)spelling.bytes;
    size_t offset = 0u;
    size_t count = 0u;
    uint32_t maximum = 0u;
    if (spelling.bytes == NULL && spelling.length != 0u) return false;
    while (offset < spelling.length) {
        uint32_t scalar;
        if (!utf8_next(bytes, spelling.length, &offset, &scalar)
                || count == SIZE_MAX)
            return false;
        if (scalar > maximum) maximum = scalar;
        count++;
    }
    *count_out = count;
    *maximum_out = maximum;
    return true;
}

static st_symbol_intern_status_t snapshot_lookup(
    const st_symbol_intern_context_t *context,
    const st_object_view_t *input, uint64_t hash,
    const st_value_t *roots, const uint64_t *hashes, size_t count,
    const size_t *slots, size_t capacity, st_value_t *value_out,
    bool *found_out)
{
    size_t slot = (size_t)hash & (capacity - 1u);
    size_t distance = 0u;
    *value_out = ST_VALUE_INVALID;
    *found_out = false;
    while (distance < capacity) {
        size_t encoded = slots[slot];
        size_t entry_index;
        st_object_view_t symbol;
        st_symbol_intern_status_t status;
        if (encoded == 0u) return ST_SYMBOL_INTERN_OK;
        entry_index = encoded - 1u;
        if (entry_index >= count) return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
        if (probe_distance(hashes[entry_index], slot, capacity) < distance)
            return ST_SYMBOL_INTERN_OK;
        if (hashes[entry_index] == hash) {
            status = sequence_view(context, roots[entry_index], &symbol, NULL);
            if (status != ST_SYMBOL_INTERN_OK) return status;
            if (sequences_equal(input, &symbol)) {
                *value_out = roots[entry_index];
                *found_out = true;
                return ST_SYMBOL_INTERN_OK;
            }
        }
        slot = (slot + 1u) & (capacity - 1u);
        distance++;
    }
    return ST_SYMBOL_INTERN_ERR_INVALID_STATE;
}

void st_symbol_intern_batch_init(st_symbol_intern_batch_t *batch)
{
    if (batch != NULL) memset(batch, 0, sizeof(*batch));
}

static void batch_state_release(st_symbol_intern_batch_state_t *batch)
{
    st_runtime_allocator_t allocator;
    size_t state_size;
    if (batch == NULL) return;
    allocator = batch->allocator;
    release_block(allocator, batch->results, batch->result_block_size);
    release_block(allocator, batch->slots, batch->table_block_size);
    release_block(allocator, batch->entry_block, batch->entry_block_size);
    state_size = batch->state_block_size;
    memset(batch, 0, sizeof(*batch));
    release_block(allocator, batch, state_size);
}

st_symbol_intern_status_t st_symbol_intern_batch_prepare_utf8(
    st_symbol_intern_batch_t *batch, st_symbol_intern_context_t *context,
    const st_symbol_utf8_t *spellings, size_t spelling_count)
{
    st_symbol_intern_state_t *state = live_state(context);
    st_symbol_intern_batch_state_t *prepared = NULL;
    size_t upper_count;
    size_t table_capacity;
    size_t entry_capacity;
    size_t table_bytes;
    size_t result_bytes;
    st_symbol_intern_status_t status = ST_SYMBOL_INTERN_OK;
    if (batch == NULL || batch->state != NULL || state == NULL
            || (spellings == NULL) != (spelling_count == 0u))
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    if (!add_size(state->count, spelling_count, &upper_count))
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    table_capacity = state->table_capacity;
    while (upper_count > table_capacity - table_capacity / 4u) {
        if (table_capacity > SIZE_MAX / 2u)
            return ST_SYMBOL_INTERN_ERR_OVERFLOW;
        table_capacity *= 2u;
    }
    entry_capacity = table_capacity - table_capacity / 4u;
    if (!multiply_size(table_capacity, sizeof(size_t), &table_bytes)
            || !multiply_size(spelling_count, sizeof(st_value_t),
                              &result_bytes))
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    prepared = allocate_block(state->allocator, sizeof(*prepared),
                              &(size_t){0});
    if (prepared == NULL) return ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
    prepared->allocator = state->allocator;
    if (!round_up(sizeof(*prepared), _Alignof(max_align_t),
                  &prepared->state_block_size)) {
        status = ST_SYMBOL_INTERN_ERR_OVERFLOW;
        goto failure;
    }
    prepared->context = context;
    prepared->expected_state = state;
    prepared->expected_entry_block = state->entry_block;
    prepared->expected_slots = state->slots;
    prepared->expected_count = state->count;
    prepared->entry_capacity = entry_capacity;
    prepared->table_capacity = table_capacity;
    prepared->result_count = spelling_count;
    status = allocate_entries(
        state->allocator, entry_capacity, &prepared->entry_block,
        &prepared->entry_block_size, &prepared->roots, &prepared->hashes);
    if (status != ST_SYMBOL_INTERN_OK) goto failure;
    prepared->slots = allocate_block(
        state->allocator, table_bytes, &prepared->table_block_size);
    if (prepared->slots == NULL) {
        status = ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
        goto failure;
    }
    if (result_bytes != 0u) {
        prepared->results = allocate_block(
            state->allocator, result_bytes, &prepared->result_block_size);
        if (prepared->results == NULL) {
            status = ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
            goto failure;
        }
    }
    if (state->count != 0u) {
        memcpy(prepared->roots, state->roots,
               state->count * sizeof(*prepared->roots));
        memcpy(prepared->hashes, state->hashes,
               state->count * sizeof(*prepared->hashes));
    }
    size_t next_count = state->count;
    for (size_t index = 0u; index < state->count; index++)
        if (!table_insert(prepared->slots, table_capacity,
                          prepared->hashes, index))
            abort();
    for (size_t index = 0u; index < spelling_count; index++) {
        size_t scalar_count;
        uint32_t maximum;
        uint32_t shape_id;
        st_value_t candidate = ST_VALUE_INVALID;
        st_object_view_t candidate_view;
        uint64_t hash;
        st_value_t canonical;
        bool exists;
        size_t offset = 0u;
        if (!utf8_measure(spellings[index], &scalar_count, &maximum)) {
            status = ST_SYMBOL_INTERN_ERR_INVALID_VALUE;
            goto failure;
        }
        shape_id = maximum <= UINT8_MAX ? state->symbol_shapes[0]
            : maximum <= UINT16_MAX ? state->symbol_shapes[1]
            : state->symbol_shapes[2];
        status = map_heap_status(st_heap_allocate(
            st_image_runtime_heap(context->image), state->symbol_class_id,
            shape_id, scalar_count, scalar_count, ST_HEADER_IMMUTABLE,
            &candidate));
        if (status != ST_SYMBOL_INTERN_OK) goto failure;
        if (st_heap_object_view(st_image_runtime_heap(context->image),
                                candidate, &candidate_view) != ST_HEAP_OK)
            abort();
        for (size_t scalar_index = 0u; scalar_index < scalar_count;
             scalar_index++) {
            uint32_t scalar;
            if (!utf8_next((const unsigned char *)spellings[index].bytes,
                           spellings[index].length, &offset, &scalar))
                abort();
            store_code_point(&candidate_view, scalar_index, scalar);
        }
        hash = state->hash != NULL
            ? state->hash(state->hash_user, &candidate_view)
            : default_hash(&candidate_view);
        status = snapshot_lookup(
            context, &candidate_view, hash, prepared->roots,
            prepared->hashes, next_count, prepared->slots,
            table_capacity, &canonical, &exists);
        if (status != ST_SYMBOL_INTERN_OK) goto failure;
        if (!exists) {
            if (next_count >= entry_capacity) abort();
            canonical = candidate;
            prepared->roots[next_count] = candidate;
            prepared->hashes[next_count] = hash;
            if (!table_insert(prepared->slots, table_capacity,
                              prepared->hashes, next_count))
                abort();
            next_count++;
        }
        prepared->results[index] = canonical;
    }
    /* The committed count may be smaller than the input upper bound because
     * existing and repeated spellings preserve identity. */
    prepared->prepared_count = next_count;
    batch->state = prepared;
    /* Assign after full construction; no externally visible partial batch. */
    return ST_SYMBOL_INTERN_OK;

failure:
    batch_state_release(prepared);
    return status;
}

const st_value_t *st_symbol_intern_batch_values(
    const st_symbol_intern_batch_t *batch, size_t *count_out)
{
    if (count_out != NULL) *count_out = 0u;
    if (batch == NULL || batch->state == NULL || count_out == NULL)
        return NULL;
    *count_out = batch->state->result_count;
    return batch->state->result_count == 0u ? NULL : batch->state->results;
}

st_symbol_intern_status_t st_symbol_intern_batch_commit(
    st_symbol_intern_batch_t *batch)
{
    st_symbol_intern_batch_state_t *prepared = batch == NULL
        ? NULL : batch->state;
    st_symbol_intern_state_t *state;
    void *old_entries;
    size_t old_entries_size;
    size_t *old_slots;
    size_t old_slots_size;
    if (prepared == NULL || prepared->committed)
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    state = live_state(prepared->context);
    if (state == NULL || state != prepared->expected_state
            || state->entry_block != prepared->expected_entry_block
            || state->slots != prepared->expected_slots
            || state->count != prepared->expected_count)
        return ST_SYMBOL_INTERN_ERR_CONFLICT;

    old_entries = state->entry_block;
    old_entries_size = state->entry_block_size;
    old_slots = state->slots;
    old_slots_size = state->table_block_size;
    state->entry_block = prepared->entry_block;
    state->entry_block_size = prepared->entry_block_size;
    state->roots = prepared->roots;
    state->hashes = prepared->hashes;
    state->entry_capacity = prepared->entry_capacity;
    state->slots = prepared->slots;
    state->table_capacity = prepared->table_capacity;
    state->table_block_size = prepared->table_block_size;
    state->count = prepared->prepared_count;
    prepared->entry_block = NULL;
    prepared->entry_block_size = 0u;
    prepared->roots = NULL;
    prepared->hashes = NULL;
    prepared->slots = NULL;
    prepared->table_block_size = 0u;
    prepared->committed = true;
    release_block(state->allocator, old_slots, old_slots_size);
    release_block(state->allocator, old_entries, old_entries_size);
    return ST_SYMBOL_INTERN_OK;
}

void st_symbol_intern_batch_destroy(st_symbol_intern_batch_t *batch)
{
    if (batch == NULL) return;
    batch_state_release(batch->state);
    batch->state = NULL;
}

st_symbol_intern_status_t st_symbol_intern(
    st_symbol_intern_context_t *context, st_value_t string,
    st_value_t *result_out)
{
    st_symbol_intern_state_t *state = live_state(context);
    st_object_view_t input;
    st_object_view_t output;
    st_symbol_intern_status_t status;
    st_value_t found;
    st_value_t symbol;
    bool exists;
    uint32_t maximum;
    uint32_t shape_id;
    uint64_t hash;
    bool grow;
    size_t next_capacity = 0u;
    size_t next_entry_capacity = 0u;
    void *next_entries = NULL;
    size_t next_entries_size = 0u;
    st_value_t *next_roots = NULL;
    uint64_t *next_hashes = NULL;
    size_t *next_slots = NULL;
    size_t next_slots_size = 0u;
    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (state == NULL || result_out == NULL)
        return ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    status = sequence_view(context, string, &input, &maximum);
    if (status != ST_SYMBOL_INTERN_OK) return status;
    hash = state->hash != NULL
        ? state->hash(state->hash_user, &input) : default_hash(&input);
    status = table_lookup(context, &input, hash, &found, &exists);
    if (status != ST_SYMBOL_INTERN_OK) return status;
    if (exists) {
        *result_out = found;
        return ST_SYMBOL_INTERN_OK;
    }
    if (state->count == SIZE_MAX)
        return ST_SYMBOL_INTERN_ERR_OVERFLOW;
    grow = state->count + 1u
        > state->table_capacity - state->table_capacity / 4u;
    if (grow) {
        size_t table_bytes;
        if (state->table_capacity > SIZE_MAX / 2u)
            return ST_SYMBOL_INTERN_ERR_OVERFLOW;
        next_capacity = state->table_capacity * 2u;
        next_entry_capacity = next_capacity - next_capacity / 4u;
        status = allocate_entries(
            state->allocator, next_entry_capacity, &next_entries,
            &next_entries_size, &next_roots, &next_hashes);
        if (status != ST_SYMBOL_INTERN_OK) return status;
        if (!multiply_size(next_capacity, sizeof(size_t), &table_bytes)) {
            status = ST_SYMBOL_INTERN_ERR_OVERFLOW;
            goto failure;
        }
        next_slots = allocate_block(
            state->allocator, table_bytes, &next_slots_size);
        if (next_slots == NULL) {
            status = ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY;
            goto failure;
        }
        memcpy(next_roots, state->roots,
               state->count * sizeof(*next_roots));
        memcpy(next_hashes, state->hashes,
               state->count * sizeof(*next_hashes));
        for (size_t index = 0u; index < state->count; index++)
            if (!table_insert(next_slots, next_capacity,
                              next_hashes, index))
                abort();
    }
    shape_id = maximum <= UINT8_MAX ? state->symbol_shapes[0]
        : maximum <= UINT16_MAX ? state->symbol_shapes[1]
        : state->symbol_shapes[2];
    st_heap_status_t heap_status = st_heap_allocate(
        st_image_runtime_heap(context->image), state->symbol_class_id,
        shape_id, input.indexed_length, input.indexed_length,
        ST_HEADER_IMMUTABLE, &symbol);
    if (heap_status != ST_HEAP_OK) {
        status = map_heap_status(heap_status);
        goto failure;
    }
    if (st_heap_object_view(st_image_runtime_heap(context->image), symbol,
                            &output) != ST_HEAP_OK
            || output.class_descriptor->class_id != state->symbol_class_id
            || output.shape_descriptor->shape_id != shape_id
            || output.indexed_length != input.indexed_length)
        abort();
    for (size_t index = 0u; index < input.indexed_length; index++)
        store_code_point(&output, index, load_code_point(&input, index));
    st_value_t *roots = grow ? next_roots : state->roots;
    uint64_t *hashes = grow ? next_hashes : state->hashes;
    size_t *slots = grow ? next_slots : state->slots;
    size_t capacity = grow ? next_capacity : state->table_capacity;
    roots[state->count] = symbol;
    hashes[state->count] = hash;
    if (!table_insert(slots, capacity, hashes, state->count)) abort();
    if (grow) {
        void *old_entries = state->entry_block;
        size_t old_entries_size = state->entry_block_size;
        size_t *old_slots = state->slots;
        size_t old_slots_size = state->table_block_size;
        state->entry_block = next_entries;
        state->entry_block_size = next_entries_size;
        state->roots = next_roots;
        state->hashes = next_hashes;
        state->entry_capacity = next_entry_capacity;
        state->slots = next_slots;
        state->table_capacity = next_capacity;
        state->table_block_size = next_slots_size;
        next_entries = NULL;
        next_slots = NULL;
        release_block(state->allocator, old_slots, old_slots_size);
        release_block(state->allocator, old_entries, old_entries_size);
    }
    state->count++;
    *result_out = symbol;
    return ST_SYMBOL_INTERN_OK;

failure:
    release_block(state->allocator, next_slots, next_slots_size);
    release_block(state->allocator, next_entries, next_entries_size);
    return status;
}

size_t st_symbol_intern_count(const st_symbol_intern_context_t *context)
{
    st_symbol_intern_state_t *state = live_state(context);
    return state == NULL ? 0u : state->count;
}

size_t st_symbol_intern_table_capacity(
    const st_symbol_intern_context_t *context)
{
    st_symbol_intern_state_t *state = live_state(context);
    return state == NULL ? 0u : state->table_capacity;
}

uint32_t st_aot_string_as_symbol_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    const st_image_root_provider_t *provider;
    st_symbol_intern_context_t *context;
    st_symbol_intern_status_t status;
    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (detail_out != NULL) *detail_out = 0u;
    if (frame == NULL || result_out == NULL || detail_out == NULL
            || argument_count != 0u || arguments != NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return (uint32_t)ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT;
    st_aot_thread_t *thread = frame->thread;
    if (thread->image == NULL)
        return (uint32_t)ST_SYMBOL_INTERN_ERR_INVALID_STATE;
    provider = st_image_runtime_root_provider_find(
        thread->image, symbol_root_span);
    if (provider == NULL || provider->roots != symbol_root_span
            || provider->owner == NULL)
        return (uint32_t)ST_SYMBOL_INTERN_ERR_INVALID_STATE;
    context = provider->owner;
    status = st_symbol_intern(context, receiver, result_out);
    return (uint32_t)status;
}

const char *st_symbol_intern_status_string(st_symbol_intern_status_t status)
{
    switch (status) {
    case ST_SYMBOL_INTERN_OK: return "ok";
    case ST_SYMBOL_INTERN_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_SYMBOL_INTERN_ERR_INVALID_STATE: return "invalid interner state";
    case ST_SYMBOL_INTERN_ERR_INVALID_DESCRIPTOR:
        return "invalid String/Symbol descriptor";
    case ST_SYMBOL_INTERN_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_SYMBOL_INTERN_ERR_TYPE_MISMATCH:
        return "not a configured String or Symbol";
    case ST_SYMBOL_INTERN_ERR_NOT_MEMBER: return "not a live heap member";
    case ST_SYMBOL_INTERN_ERR_BAD_OBJECT:
        return "malformed String/Symbol or Unicode scalar";
    case ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_SYMBOL_INTERN_ERR_OVERFLOW: return "size overflow";
    case ST_SYMBOL_INTERN_ERR_CONFLICT:
        return "image already has a dynamic root provider";
    default: return "unknown Symbol interning status";
    }
}
