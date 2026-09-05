#include "st_reflection_primitives.h"

#include "../../platform/runtime.h"
#include <stdlib.h>
#include <string.h>

#define ST_REFLECTION_MAGIC UINT64_C(0x53545245464c4543)
#define ST_REFLECTION_INITIAL_CAPACITY 4u

#define ST_COMPILED_METHOD_SELECTOR_SLOT 0u
#define ST_COMPILED_METHOD_CLASS_SLOT 1u
#define ST_COMPILED_METHOD_ARITY_SLOT 2u
#define ST_COMPILED_METHOD_SLOT_COUNT 3u

typedef struct {
    st_value_t key;
    uint32_t id;
    size_t distance;
} identity_entry_t;

typedef struct {
    StMethodEntry *entry;
    const StMethodBinding *binding;
    size_t root_index;
    size_t distance;
} method_cache_entry_t;

struct st_reflection_state {
    uint64_t magic;
    st_platform_mutex_t cache_mutex;
    bool cache_mutex_initialized;
    st_image_runtime_t *image;
    st_lookup_context_t *lookup;
    st_heap_t *heap;
    const st_runtime_descriptors_t *descriptors;
    st_primitive_allocator_t allocator;
    st_value_t *class_objects;
    size_t class_count;
    st_value_t *selector_symbols;
    size_t selector_count;
    identity_entry_t *class_map;
    size_t class_map_capacity;
    identity_entry_t *selector_map;
    size_t selector_map_capacity;
    method_cache_entry_t *method_cache;
    size_t method_cache_capacity;
    st_value_t *method_roots;
    size_t method_count;
    uint32_t symbol_class_id;
    uint32_t compiled_method_class_id;
    uint32_t compiled_method_shape_id;
};

static const st_primitive_spec_t reflection_specs[] = {
    {
        "BehaviorLookupSelectorPrimitive",
        sizeof("BehaviorLookupSelectorPrimitive") - 1u,
        1u,
        ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_FALL_THROUGH,
        ST_PRIMITIVE_RUNTIME_SYMBOL,
        ST_PRIMITIVE_INVALID_INTRINSIC_ID,
        "st_aot_behavior_lookup_selector_primitive_execute",
        sizeof("st_aot_behavior_lookup_selector_primitive_execute") - 1u
    }
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

static bool normalize_allocator(st_primitive_allocator_t input,
                                st_primitive_allocator_t *output)
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

static void release(st_primitive_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) {
        allocator.deallocate(allocator.user, pointer);
    }
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static bool add_size(size_t left, size_t right, size_t *result_out)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result_out = left + right;
    return true;
}

static uint64_t hash_value(st_value_t value)
{
    uint64_t hash = value >> ST_VALUE_TAG_BITS;
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;
    return hash;
}

static bool table_capacity(size_t count, size_t *capacity_out)
{
    size_t capacity = ST_REFLECTION_INITIAL_CAPACITY;
    if (capacity_out == NULL) {
        return false;
    }
    while (count > capacity - capacity / 4u) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    *capacity_out = capacity;
    return true;
}

static bool identity_insert(identity_entry_t *table, size_t capacity,
                            st_value_t key, uint32_t id)
{
    identity_entry_t incoming = { key, id, 0u };
    size_t slot = (size_t)hash_value(key) & (capacity - 1u);
    size_t probes;

    for (probes = 0u; probes < capacity; probes++) {
        identity_entry_t *entry = &table[slot];
        if (entry->id == 0u) {
            *entry = incoming;
            return true;
        }
        if (entry->key == key) {
            return false;
        }
        if (entry->distance < incoming.distance) {
            identity_entry_t displaced = *entry;
            *entry = incoming;
            incoming = displaced;
        }
        incoming.distance++;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return false;
}

static uint32_t identity_lookup(const identity_entry_t *table,
                                size_t capacity, st_value_t key)
{
    size_t slot = (size_t)hash_value(key) & (capacity - 1u);
    size_t distance = 0u;

    while (distance < capacity) {
        const identity_entry_t *entry = &table[slot];
        if (entry->id == 0u || entry->distance < distance) {
            return 0u;
        }
        if (entry->key == key) {
            return entry->id;
        }
        distance++;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return 0u;
}

static uint64_t hash_entry_pointer(const StMethodEntry *entry)
{
    return hash_value((st_value_t)(uintptr_t)entry);
}

static method_cache_entry_t *method_cache_lookup(
    method_cache_entry_t *table, size_t capacity,
    const StMethodEntry *entry)
{
    size_t slot = (size_t)hash_entry_pointer(entry) & (capacity - 1u);
    size_t distance = 0u;
    while (distance < capacity) {
        method_cache_entry_t *candidate = &table[slot];
        if (candidate->entry == NULL || candidate->distance < distance) {
            return NULL;
        }
        if (candidate->entry == entry) {
            return candidate;
        }
        distance++;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return NULL;
}

static bool method_cache_insert(method_cache_entry_t *table,
                                size_t capacity, StMethodEntry *entry,
                                size_t root_index)
{
    method_cache_entry_t incoming = {
        entry, NULL, root_index, 0u
    };
    size_t slot = (size_t)hash_entry_pointer(entry) & (capacity - 1u);
    size_t probes;
    for (probes = 0u; probes < capacity; probes++) {
        method_cache_entry_t *candidate = &table[slot];
        if (candidate->entry == NULL) {
            *candidate = incoming;
            return true;
        }
        if (candidate->entry == entry) {
            return false;
        }
        if (candidate->distance < incoming.distance) {
            method_cache_entry_t displaced = *candidate;
            *candidate = incoming;
            incoming = displaced;
        }
        incoming.distance++;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return false;
}

static bool bitmap_get(const uint64_t *bitmap, size_t index)
{
    return (bitmap[index >> 6u]
            & (UINT64_C(1) << (index & 63u))) != 0u;
}

static void bitmap_set(uint64_t *bitmap, size_t index)
{
    bitmap[index >> 6u] |= UINT64_C(1) << (index & 63u);
}

static st_reflection_state_t *live_state(
    const st_reflection_context_t *context)
{
    if (context == NULL || !context->initialized
            || context->abi_version != ST_REFLECTION_CONTEXT_ABI_VERSION
            || context->image == NULL || context->lookup == NULL
            || context->state == NULL
            || context->state->magic != ST_REFLECTION_MAGIC
            || context->state->image != context->image
            || context->state->lookup != context->lookup
            || st_image_runtime_heap(context->image)
                != context->state->heap
            || !st_image_runtime_root_provider_contains(
                context->image, &context->root_provider))
        return NULL;
    return context->state;
}

static st_reflection_state_t *basic_state(
    const st_reflection_context_t *context)
{
    if (context == NULL || !context->initialized
            || context->abi_version != ST_REFLECTION_CONTEXT_ABI_VERSION
            || context->state == NULL
            || context->state->magic != ST_REFLECTION_MAGIC)
        return NULL;
    return context->state;
}

static st_image_runtime_status_t reflection_root_span(
    void *owner, const st_value_t **roots_out, size_t *root_count_out)
{
    st_reflection_context_t *context = owner;
    st_reflection_state_t *state = basic_state(context);
    if (roots_out != NULL) {
        *roots_out = NULL;
    }
    if (root_count_out != NULL) {
        *root_count_out = 0u;
    }
    if (state == NULL || roots_out == NULL || root_count_out == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
    }
    *roots_out = state->method_count == 0u ? NULL : state->method_roots;
    *root_count_out = state->method_count;
    return ST_IMAGE_RUNTIME_OK;
}

static st_reflection_primitive_status_t map_heap_status(
    st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK:
        return ST_REFLECTION_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW:
        return ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER:
        return ST_REFLECTION_PRIMITIVE_ERR_NOT_MEMBER;
    default:
        return ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

static bool compiled_method_layout_is_valid(
    const st_runtime_descriptors_t *descriptors,
    uint32_t class_id, uint32_t shape_id)
{
    const StClassDescriptor *class_descriptor = st_runtime_class(
        descriptors, class_id);
    const StShapeDescriptor *shape = st_runtime_shape(descriptors, shape_id);

    if (class_descriptor == NULL || shape == NULL
            || (class_descriptor->flags & ST_CLASS_METACLASS) != 0u
            || (class_descriptor->flags & ST_CLASS_ABSTRACT) == 0u
            || shape->class_id != class_id
            || shape->fixed_word_count != ST_COMPILED_METHOD_SLOT_COUNT
            || shape->indexed_format != ST_INDEXED_NONE
            || shape->fixed_pointer_bitmap_word_count != 1u
            || shape->fixed_pointer_bitmap == NULL)
        return false;
    return shape->fixed_pointer_bitmap[0] == UINT64_C(0x7);
}

static bool symbol_layout_is_valid(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id)
{
    const StClassDescriptor *descriptor = st_runtime_class(
        descriptors, class_id);
    return descriptor != NULL
        && (descriptor->flags & ST_CLASS_METACLASS) == 0u;
}

static st_reflection_primitive_status_t validate_class_objects(
    st_reflection_state_t *state)
{
    size_t index;
    for (index = 0u; index < state->class_count; index++) {
        uint32_t represented_id = (uint32_t)index + 1u;
        const StClassDescriptor *represented = st_runtime_class(
            state->descriptors, represented_id);
        st_object_view_t view;
        st_heap_status_t heap_status = st_heap_object_view(
            state->heap, state->class_objects[index], &view);
        if (heap_status != ST_HEAP_OK) {
            return map_heap_status(heap_status);
        }
        if (represented == NULL
                || view.class_descriptor->class_id
                    != represented->metaclass_id
                || (view.class_descriptor->flags & ST_CLASS_METACLASS) == 0u
                || !identity_insert(state->class_map,
                                    state->class_map_capacity,
                                    state->class_objects[index],
                                    represented_id))
            return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

static bool symbol_view_is_valid(const st_reflection_state_t *state,
                                 st_value_t symbol)
{
    st_object_view_t view;
    uint64_t header;
    st_indexed_format_t format;
    if (st_heap_object_view(state->heap, symbol, &view) != ST_HEAP_OK
            || view.class_descriptor->class_id != state->symbol_class_id
            || view.shape_descriptor->fixed_word_count != 0u)
        return false;
    format = view.shape_descriptor->indexed_format;
    if (format != ST_INDEXED_UINT8 && format != ST_INDEXED_UINT16
            && format != ST_INDEXED_UINT32)
        return false;
    header = st_object_header_load(&view.object->header);
    return (st_object_header_flags(header) & ST_HEADER_IMMUTABLE) != 0u;
}

static st_reflection_primitive_status_t validate_selector_symbols(
    st_reflection_state_t *state)
{
    size_t index;
    for (index = 0u; index < state->selector_count; index++) {
        if (!symbol_view_is_valid(state, state->selector_symbols[index])
                || !identity_insert(state->selector_map,
                                    state->selector_map_capacity,
                                    state->selector_symbols[index],
                                    (uint32_t)index + 1u)) {
            return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
        }
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

static bool lookup_selectors_are_covered(const st_reflection_state_t *state)
{
    size_t class_index;
    for (class_index = 0u; class_index < state->descriptors->class_count;
         class_index++) {
        const StClassDescriptor *descriptor =
            state->descriptors->classes[class_index];
        size_t slot_index;
        for (slot_index = 0u; slot_index < descriptor->method_slot_count;
             slot_index++) {
            uint32_t selector_id = descriptor->method_slots[slot_index]
                                       .selector_id;
            if (selector_id == 0u
                    || (size_t)selector_id > state->selector_count)
                return false;
        }
    }
    return true;
}

typedef struct {
    const st_reflection_state_t *state;
    uint64_t *class_bits;
    uint64_t *selector_bits;
} root_probe_t;

static bool mark_bootstrap_root(void *user, const st_value_t *root_slot)
{
    root_probe_t *probe = user;
    uint32_t id;

    id = identity_lookup(probe->state->class_map,
                         probe->state->class_map_capacity, *root_slot);
    if (id != 0u) {
        bitmap_set(probe->class_bits, (size_t)id - 1u);
    }
    id = identity_lookup(probe->state->selector_map,
                         probe->state->selector_map_capacity, *root_slot);
    if (id != 0u) {
        bitmap_set(probe->selector_bits, (size_t)id - 1u);
    }
    return true;
}

static st_reflection_primitive_status_t verify_image_roots(
    st_reflection_state_t *state)
{
    size_t class_words = (state->class_count + 63u) / 64u;
    size_t selector_words = (state->selector_count + 63u) / 64u;
    size_t word_count;
    size_t bytes;
    uint64_t *bits;
    root_probe_t probe;
    st_image_runtime_status_t visit_status;
    size_t visited;
    size_t index;

    if (!add_size(class_words, selector_words, &word_count)
            || !multiply_size(word_count, sizeof(*bits), &bytes)) {
        return ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW;
    }
    bits = state->allocator.allocate(state->allocator.user, bytes);
    if (bits == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    memset(bits, 0, bytes);
    probe = (root_probe_t) {
        state,
        bits,
        bits + class_words
    };
    visit_status = st_image_runtime_visit_roots(
        state->image, mark_bootstrap_root, &probe, &visited);
    if (visit_status != ST_IMAGE_RUNTIME_OK) {
        release(state->allocator, bits);
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }
    for (index = 0u; index < state->class_count; index++) {
        if (!bitmap_get(probe.class_bits, index)) {
            release(state->allocator, bits);
            return ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT;
        }
    }
    for (index = 0u; index < state->selector_count; index++) {
        if (!bitmap_get(probe.selector_bits, index)) {
            release(state->allocator, bits);
            return ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT;
        }
    }
    release(state->allocator, bits);
    return ST_REFLECTION_PRIMITIVE_OK;
}

static void destroy_state(st_reflection_state_t *state)
{
    st_primitive_allocator_t allocator;
    if (state == NULL) {
        return;
    }
    allocator = state->allocator;
    if (state->cache_mutex_initialized) {
        if (st_runtime_platform.mutex_destroy(&state->cache_mutex) != 0)
        {
            abort();
        }
        state->cache_mutex_initialized = false;
    }
    release(allocator, state->method_roots);
    release(allocator, state->method_cache);
    release(allocator, state->selector_map);
    release(allocator, state->class_map);
    release(allocator, state->selector_symbols);
    release(allocator, state->class_objects);
    memset(state, 0, sizeof(*state));
    release(allocator, state);
}

static st_reflection_primitive_status_t allocate_method_cache(
    st_reflection_state_t *state)
{
    size_t slot_count = 0u;
    size_t cache_bytes;
    size_t root_bytes;
    size_t root_index = 0u;
    size_t class_index;

    for (class_index = 0u; class_index < state->descriptors->class_count;
         class_index++) {
        const StClassDescriptor *descriptor =
            state->descriptors->classes[class_index];
        if (descriptor->method_slot_count > SIZE_MAX - slot_count) {
            return ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW;
        }
        slot_count += descriptor->method_slot_count;
    }
    if (!table_capacity(slot_count, &state->method_cache_capacity)
            || !multiply_size(state->method_cache_capacity,
                              sizeof(*state->method_cache), &cache_bytes)
            || !multiply_size(slot_count, sizeof(*state->method_roots),
                              &root_bytes)) {
        return ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW;
    }
    state->method_cache = state->allocator.allocate(
        state->allocator.user, cache_bytes);
    if (state->method_cache == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    memset(state->method_cache, 0, cache_bytes);
    if (root_bytes != 0u) {
        state->method_roots = state->allocator.allocate(
            state->allocator.user, root_bytes);
        if (state->method_roots == NULL) {
            return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
        }
        for (size_t index = 0u; index < slot_count; index++) {
            state->method_roots[index] = st_value_nil();
        }
    }

    for (class_index = 0u; class_index < state->descriptors->class_count;
         class_index++) {
        const StClassDescriptor *descriptor =
            state->descriptors->classes[class_index];
        for (size_t slot_index = 0u;
             slot_index < descriptor->method_slot_count; slot_index++) {
            StMethodEntry *entry = descriptor->method_slots[slot_index].entry;
            if (method_cache_lookup(state->method_cache,
                                    state->method_cache_capacity,
                                    entry) != NULL)
                continue;
            if (!method_cache_insert(state->method_cache,
                                     state->method_cache_capacity,
                                     entry, root_index))
                return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
            root_index++;
        }
    }
    state->method_count = root_index;
    return ST_REFLECTION_PRIMITIVE_OK;
}

static st_reflection_primitive_status_t allocate_compiled_method(
    st_reflection_state_t *state, st_value_t selector,
    uint32_t defining_class_id, uint32_t arity, st_value_t *result_out);

static st_reflection_primitive_status_t materialize_emitted_methods(
    st_reflection_state_t *state)
{
    for (size_t index = 0u; index < state->method_cache_capacity; index++) {
        method_cache_entry_t *cache = &state->method_cache[index];
        const StMethodBinding *binding;
        const StMethodDescriptor *descriptor;
        st_value_t method = ST_VALUE_INVALID;
        st_reflection_primitive_status_t status;
        if (cache->entry == NULL) {
            continue;
        }
        binding = st_method_entry_load(cache->entry);
        if (!st_method_binding_is_valid(binding)) {
            return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
        }
        descriptor = binding->descriptor;
        if (descriptor->selector_id == 0u
                || descriptor->selector_id > state->selector_count
                || descriptor->owner_class_id == 0u
                || descriptor->owner_class_id > state->class_count)
            return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
        status = allocate_compiled_method(
            state,
            state->selector_symbols[descriptor->selector_id - 1u],
            descriptor->owner_class_id, descriptor->arity, &method);
        if (status != ST_REFLECTION_PRIMITIVE_OK) {
            return status;
        }
        state->method_roots[cache->root_index] = method;
        cache->binding = binding;
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

static st_reflection_primitive_status_t allocate_state_tables(
    st_reflection_state_t *state,
    const st_reflection_context_options_t *options)
{
    size_t class_bytes;
    size_t selector_bytes;
    size_t class_map_bytes;
    size_t selector_map_bytes;
    if (!table_capacity(options->class_object_count,
                        &state->class_map_capacity)
            || !table_capacity(options->selector_symbol_count,
                               &state->selector_map_capacity)
            || !multiply_size(options->class_object_count,
                              sizeof(*state->class_objects), &class_bytes)
            || !multiply_size(options->selector_symbol_count,
                              sizeof(*state->selector_symbols),
                              &selector_bytes)
            || !multiply_size(state->class_map_capacity,
                              sizeof(*state->class_map), &class_map_bytes)
            || !multiply_size(state->selector_map_capacity,
                              sizeof(*state->selector_map),
                              &selector_map_bytes))
        return ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW;

    state->class_objects = state->allocator.allocate(
        state->allocator.user, class_bytes);
    state->selector_symbols = state->allocator.allocate(
        state->allocator.user, selector_bytes);
    state->class_map = state->allocator.allocate(
        state->allocator.user, class_map_bytes);
    state->selector_map = state->allocator.allocate(
        state->allocator.user, selector_map_bytes);
    if (state->class_objects == NULL || state->selector_symbols == NULL
            || state->class_map == NULL || state->selector_map == NULL)
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;

    memcpy(state->class_objects, options->class_objects_by_id, class_bytes);
    memcpy(state->selector_symbols, options->selector_symbols_by_id,
           selector_bytes);
    memset(state->class_map, 0, class_map_bytes);
    memset(state->selector_map, 0, selector_map_bytes);
    return ST_REFLECTION_PRIMITIVE_OK;
}

st_reflection_primitive_status_t st_reflection_context_init(
    st_reflection_context_t *context,
    const st_reflection_context_options_t *options)
{
    st_primitive_allocator_t allocator;
    st_reflection_state_t *state;
    st_reflection_primitive_status_t status;
    st_heap_t *heap;
    const st_runtime_descriptors_t *descriptors;

    if (context == NULL || options == NULL || context->initialized
            || context->state != NULL || context->image != NULL
            || context->lookup != NULL || options->image == NULL
            || options->lookup == NULL || !options->lookup->initialized
            || options->class_objects_by_id == NULL
            || options->selector_symbols_by_id == NULL
            || options->class_object_count == 0u
            || options->selector_symbol_count == 0u
            || options->class_object_count > UINT32_MAX
            || options->selector_symbol_count > UINT32_MAX
            || !normalize_allocator(options->allocator, &allocator))
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;

    heap = st_image_runtime_heap(options->image);
    descriptors = heap == NULL ? NULL : st_heap_descriptors(heap);
    if (descriptors == NULL
            || options->lookup->descriptors != descriptors
            || options->class_object_count != descriptors->class_count
            || !symbol_layout_is_valid(descriptors, options->symbol_class_id)
            || !compiled_method_layout_is_valid(
                descriptors, options->compiled_method_class_id,
                options->compiled_method_shape_id))
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;

    state = allocator.allocate(allocator.user, sizeof(*state));
    if (state == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->allocator = allocator;
    if (st_runtime_platform.mutex_init(&state->cache_mutex) != 0)
    {
        destroy_state(state);
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }
    state->cache_mutex_initialized = true;
    state->image = options->image;
    state->lookup = options->lookup;
    state->heap = heap;
    state->descriptors = descriptors;
    state->class_count = options->class_object_count;
    state->selector_count = options->selector_symbol_count;
    state->symbol_class_id = options->symbol_class_id;
    state->compiled_method_class_id = options->compiled_method_class_id;
    state->compiled_method_shape_id = options->compiled_method_shape_id;

    status = allocate_state_tables(state, options);
    if (status == ST_REFLECTION_PRIMITIVE_OK) {
        status = validate_class_objects(state);
    }
    if (status == ST_REFLECTION_PRIMITIVE_OK) {
        status = validate_selector_symbols(state);
    }
    if (status == ST_REFLECTION_PRIMITIVE_OK) {
        status = allocate_method_cache(state);
    }
    if (status == ST_REFLECTION_PRIMITIVE_OK
            && !lookup_selectors_are_covered(state))
        status = ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    if (status == ST_REFLECTION_PRIMITIVE_OK) {
        status = verify_image_roots(state);
    }
    if (status == ST_REFLECTION_PRIMITIVE_OK) {
        status = materialize_emitted_methods(state);
    }
    if (status != ST_REFLECTION_PRIMITIVE_OK) {
        destroy_state(state);
        return status;
    }

    state->magic = ST_REFLECTION_MAGIC;
    context->abi_version = ST_REFLECTION_CONTEXT_ABI_VERSION;
    context->initialized = true;
    context->image = options->image;
    context->lookup = options->lookup;
    context->state = state;
    context->root_provider = (st_image_root_provider_t) {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION,
        context,
        reflection_root_span
    };
    if (!st_image_runtime_root_provider_attach(
            context->image, &context->root_provider)) {
        memset(context, 0, sizeof(*context));
        destroy_state(state);
        return ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

void st_reflection_context_destroy(st_reflection_context_t *context)
{
    st_reflection_state_t *state = basic_state(context);
    if (context == NULL) {
        return;
    }
    if (state != NULL) {
        if (st_image_runtime_heap(context->image) != NULL
                && !st_image_runtime_root_provider_detach(
                    context->image, &context->root_provider))
            abort();
        destroy_state(state);
    }
    memset(context, 0, sizeof(*context));
}

bool st_reflection_context_matches(
    const st_reflection_context_t *context,
    const st_image_runtime_t *image,
    const st_lookup_context_t *lookup)
{
    st_reflection_state_t *state = live_state(context);
    return state != NULL && image != NULL && lookup != NULL
        && state->image == image && state->lookup == lookup
        && lookup->descriptors == state->descriptors;
}

static st_reflection_primitive_status_t allocate_compiled_method(
    st_reflection_state_t *state, st_value_t selector,
    uint32_t defining_class_id, uint32_t arity, st_value_t *result_out)
{
    st_value_t arity_value;
    st_value_t method = ST_VALUE_INVALID;
    st_object_view_t view;
    st_heap_status_t heap_status;

    if (defining_class_id == 0u || defining_class_id > state->class_count
            || !st_value_from_small_integer((int64_t)arity, &arity_value))
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    heap_status = st_heap_allocate(
        state->heap, state->compiled_method_class_id,
        state->compiled_method_shape_id, 0u, 0u, ST_HEADER_IMMUTABLE,
        &method);
    if (heap_status != ST_HEAP_OK) {
        return map_heap_status(heap_status);
    }
    heap_status = st_heap_object_view(state->heap, method, &view);
    if (heap_status != ST_HEAP_OK || view.fixed_words == NULL
            || view.shape_descriptor->fixed_word_count
                != ST_COMPILED_METHOD_SLOT_COUNT)
        abort();

    st_value_t *slots = view.fixed_words;
    slots[ST_COMPILED_METHOD_SELECTOR_SLOT] = selector;
    slots[ST_COMPILED_METHOD_CLASS_SLOT] =
        state->class_objects[defining_class_id - 1u];
    slots[ST_COMPILED_METHOD_ARITY_SLOT] = arity_value;
    *result_out = method;
    return ST_REFLECTION_PRIMITIVE_OK;
}

static bool compiled_method_matches(
    const st_reflection_state_t *state, st_value_t method,
    st_value_t selector, uint32_t defining_class_id, uint32_t arity)
{
    st_object_view_t view;
    const st_value_t *slots;
    int64_t actual_arity;
    uint64_t header;

    if (defining_class_id == 0u || defining_class_id > state->class_count
            || st_heap_object_view(state->heap, method, &view) != ST_HEAP_OK
            || view.class_descriptor->class_id
                != state->compiled_method_class_id
            || view.shape_descriptor->shape_id
                != state->compiled_method_shape_id
            || view.fixed_words == NULL) {
        return false;
    }
    slots = view.fixed_words;
    if (slots[ST_COMPILED_METHOD_SELECTOR_SLOT] != selector
            || slots[ST_COMPILED_METHOD_CLASS_SLOT]
                != state->class_objects[defining_class_id - 1u]
            || !st_value_to_small_integer(
                slots[ST_COMPILED_METHOD_ARITY_SLOT], &actual_arity)
            || actual_arity != (int64_t)arity) {
        return false;
    }
    header = st_object_header_load(&view.object->header);
    return (st_object_header_flags(header) & ST_HEADER_IMMUTABLE) != 0u;
}

st_reflection_primitive_status_t st_reflection_lookup_selector(
    st_reflection_context_t *context, st_value_t behavior,
    st_value_t selector, st_value_t *result_out)
{
    st_reflection_state_t *state = live_state(context);
    uint32_t class_id;
    uint32_t selector_id;
    st_lookup_result_t lookup_result;
    st_lookup_status_t lookup_status;
    method_cache_entry_t *cache;
    st_value_t method = ST_VALUE_INVALID;

    if (result_out == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    *result_out = ST_VALUE_INVALID;
    if (state == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }
    if (!st_value_has_valid_encoding(behavior)
            || !st_value_has_valid_encoding(selector)) {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_VALUE;
    }

    class_id = identity_lookup(state->class_map,
                               state->class_map_capacity, behavior);
    if (class_id == 0u) {
        return ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH;
    }
    if (!symbol_view_is_valid(state, selector)) {
        return ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH;
    }

    selector_id = identity_lookup(state->selector_map,
                                  state->selector_map_capacity, selector);
    if (selector_id == 0u) {
        *result_out = st_value_nil();
        return ST_REFLECTION_PRIMITIVE_OK;
    }

    if (st_runtime_platform.mutex_lock(&state->cache_mutex) != 0)
    {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }

    lookup_status = st_lookup_inherited(
        state->lookup, class_id, selector_id, &lookup_result);
    if (lookup_status == ST_LOOKUP_NOT_FOUND) {
        *result_out = st_value_nil();
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_OK;
    }
    if (lookup_status != ST_LOOKUP_FOUND) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_ERR_LOOKUP;
    }
    if (lookup_result.binding == NULL
            || !st_method_binding_is_valid(lookup_result.binding)
            || lookup_result.binding->descriptor->selector_id != selector_id
            || lookup_result.binding->descriptor->owner_class_id
                != lookup_result.defining_class_id
            || lookup_result.defining_class_id == 0u
            || lookup_result.defining_class_id > state->class_count) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    cache = method_cache_lookup(
        state->method_cache, state->method_cache_capacity,
        lookup_result.entry);
    if (cache == NULL || cache->root_index >= state->method_count) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    if (cache->binding == lookup_result.binding) {
        method = state->method_roots[cache->root_index];
        if (!compiled_method_matches(
                state, method, selector, lookup_result.defining_class_id,
                lookup_result.binding->descriptor->arity)) {
            if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
            {
                abort();
            }
            return ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT;
        }
        *result_out = method;
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_OK;
    }
    st_reflection_primitive_status_t status = allocate_compiled_method(
        state, selector, lookup_result.defining_class_id,
        lookup_result.binding->descriptor->arity, &method);
    if (status != ST_REFLECTION_PRIMITIVE_OK) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return status;
    }
    state->method_roots[cache->root_index] = method;
    cache->binding = lookup_result.binding;
    *result_out = method;
    if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
    {
        abort();
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

st_reflection_primitive_status_t st_reflection_compiled_method_for_entry(
    st_reflection_context_t *context, const StMethodEntry *entry,
    st_value_t *result_out)
{
    st_reflection_state_t *state = live_state(context);
    method_cache_entry_t *cache;
    st_value_t method;
    if (result_out == NULL || entry == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    *result_out = ST_VALUE_INVALID;
    if (state == NULL) {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }
    if (st_runtime_platform.mutex_lock(&state->cache_mutex) != 0)
    {
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE;
    }
    cache = method_cache_lookup(
        state->method_cache, state->method_cache_capacity, entry);
    if (cache == NULL || cache->root_index >= state->method_count
            || cache->binding != st_method_entry_load(entry)) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    method = state->method_roots[cache->root_index];
    const StMethodDescriptor *descriptor = cache->binding->descriptor;
    if (!compiled_method_matches(
            state, method,
            state->selector_symbols[descriptor->selector_id - 1u],
            descriptor->owner_class_id, descriptor->arity)) {
        if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
        {
            abort();
        }
        return ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT;
    }
    *result_out = method;
    if (st_runtime_platform.mutex_unlock(&state->cache_mutex) != 0)
    {
        abort();
    }
    return ST_REFLECTION_PRIMITIVE_OK;
}

const st_primitive_spec_t *st_reflection_primitive_specs(
    size_t *count_out)
{
    if (count_out != NULL) {
        *count_out = sizeof(reflection_specs) / sizeof(reflection_specs[0]);
    }
    return reflection_specs;
}

const char *st_reflection_primitive_status_string(
    st_reflection_primitive_status_t status)
{
    switch (status) {
    case ST_REFLECTION_PRIMITIVE_OK:
        return "ok";
    case ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE:
        return "invalid state";
    case ST_REFLECTION_PRIMITIVE_ERR_WRONG_ARITY:
        return "wrong arity";
    case ST_REFLECTION_PRIMITIVE_ERR_INVALID_VALUE:
        return "invalid value";
    case ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH:
        return "type mismatch";
    case ST_REFLECTION_PRIMITIVE_ERR_NOT_MEMBER:
        return "object is not a heap member";
    case ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return "invalid descriptor or bootstrap map";
    case ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT:
        return "bootstrap object is not an image root";
    case ST_REFLECTION_PRIMITIVE_ERR_LOOKUP:
        return "method lookup failed";
    case ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW:
        return "size overflow";
    case ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT:
        return "malformed object";
    default:
        return "unknown reflection primitive status";
    }
}
