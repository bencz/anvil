#include "st_heap_primitives.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ST_HEAP_PRIMITIVE_MAGIC UINT64_C(0x5354485052494d31)

#define SPEC(name_, arity_, failure_, id_)                                   \
    {                                                                        \
        (name_), sizeof(name_) - 1u, (arity_), ST_PRIMITIVE_INSTANCE_ONLY,   \
        (failure_), ST_PRIMITIVE_INTRINSIC, (id_), NULL, 0u                  \
    }

static const st_primitive_spec_t heap_specs[] = {
    SPEC("SizePrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
         ST_INTRINSIC_SIZE),
    SPEC("AtPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_AT),
    SPEC("AtPutPrimitive", 2u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_AT_PUT),
    SPEC("InstVarAtPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_INST_VAR_AT),
    SPEC("InstVarAtPutPrimitive", 2u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_INST_VAR_AT_PUT),
    SPEC("BehaviorNewPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
         ST_INTRINSIC_BEHAVIOR_NEW),
    SPEC("BehaviorNewSizePrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_BEHAVIOR_NEW_SIZE),
    SPEC("ClassPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
         ST_INTRINSIC_CLASS),
    SPEC("HashPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
         ST_INTRINSIC_HASH),
    SPEC("ArrayEqualsPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
         ST_INTRINSIC_ARRAY_EQUALS),
    SPEC("StringHashPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
         ST_INTRINSIC_STRING_HASH)
};

#undef SPEC

struct st_heap_primitive_state {
    uint64_t magic;
    st_heap_t *heap;
    const st_runtime_descriptors_t *descriptors;
    st_immediate_class_ids_t immediate_classes;
    st_value_t *class_objects;
    size_t class_object_count;
    st_heap_indexed_access_t *indexed_access;
    size_t indexed_access_count;
    uint32_t *reverse_class_table;
    size_t reverse_class_capacity;
    st_primitive_allocator_t allocator;
};

typedef st_heap_primitive_status_t (*heap_primitive_handler_t)(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out);

typedef struct {
    uint32_t id;
    uint32_t arity;
    heap_primitive_handler_t handler;
} heap_primitive_definition_t;

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

static void release(st_primitive_allocator_t allocator, void *pointer)
{
    if (pointer) allocator.deallocate(allocator.user, pointer);
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) return false;
    *result_out = left * right;
    return true;
}

static st_heap_primitive_state_t *context_state(
    const st_heap_primitive_context_t *context)
{
    if (!context || !context->state ||
        context->state->magic != ST_HEAP_PRIMITIVE_MAGIC)
        return NULL;
    return context->state;
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

static st_heap_primitive_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK: return ST_HEAP_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW: return ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_OBJECT: return ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER: return ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_BAD_SLOT: return ST_HEAP_PRIMITIVE_ERR_BAD_SLOT_FORMAT;
    case ST_HEAP_ERR_IMMUTABLE: return ST_HEAP_PRIMITIVE_ERR_IMMUTABLE;
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_HEAP_ERR_BAD_ALIGNMENT:
    case ST_HEAP_ERR_BAD_EXTENT:
    case ST_HEAP_ERR_BAD_OBJECT:
    case ST_HEAP_ERR_INVALID_ROOT:
    case ST_HEAP_ERR_INVALID_FRAME:
    case ST_HEAP_ERR_FRAME_CYCLE:
    default: return ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

static bool immediate_class_id_is_valid(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id)
{
    const StClassDescriptor *descriptor = st_runtime_class(descriptors,
                                                            class_id);
    return descriptor && (descriptor->flags & ST_CLASS_METACLASS) == 0u;
}

static bool immediate_ids_are_valid(
    const st_runtime_descriptors_t *descriptors,
    st_immediate_class_ids_t ids)
{
    return immediate_class_id_is_valid(descriptors,
                                       ids.small_integer_class_id) &&
           immediate_class_id_is_valid(descriptors,
                                       ids.character_class_id) &&
           immediate_class_id_is_valid(descriptors, ids.nil_class_id) &&
           immediate_class_id_is_valid(descriptors, ids.false_class_id) &&
           immediate_class_id_is_valid(descriptors, ids.true_class_id);
}

static bool indexed_access_matches(const StShapeDescriptor *shape,
                                   st_heap_indexed_access_t access)
{
    switch (access) {
    case ST_HEAP_INDEXED_ACCESS_NONE:
        return shape->indexed_format == ST_INDEXED_NONE;
    case ST_HEAP_INDEXED_ACCESS_VALUES:
        return shape->indexed_format == ST_INDEXED_VALUES;
    case ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER:
        return shape->indexed_format == ST_INDEXED_UINT8 ||
               shape->indexed_format == ST_INDEXED_UINT16 ||
               shape->indexed_format == ST_INDEXED_UINT32 ||
               shape->indexed_format == ST_INDEXED_UINT64;
    case ST_HEAP_INDEXED_ACCESS_CHARACTER:
        return shape->indexed_format == ST_INDEXED_UINT8 ||
               shape->indexed_format == ST_INDEXED_UINT16 ||
               shape->indexed_format == ST_INDEXED_UINT32;
    default: return false;
    }
}

static bool reverse_insert(st_heap_primitive_state_t *state,
                           uint32_t represented_class_id)
{
    st_value_t value = state->class_objects[represented_class_id - 1u];
    size_t slot = (size_t)hash_value(value) &
                  (state->reverse_class_capacity - 1u);
    for (;;) {
        uint32_t existing = state->reverse_class_table[slot];
        if (existing == 0u) {
            state->reverse_class_table[slot] = represented_class_id;
            return true;
        }
        if (state->class_objects[existing - 1u] == value) return false;
        slot = (slot + 1u) & (state->reverse_class_capacity - 1u);
    }
}

static uint32_t reverse_lookup(const st_heap_primitive_state_t *state,
                               st_value_t value)
{
    size_t slot = (size_t)hash_value(value) &
                  (state->reverse_class_capacity - 1u);
    size_t probes;
    for (probes = 0u; probes < state->reverse_class_capacity; ++probes) {
        uint32_t class_id = state->reverse_class_table[slot];
        if (class_id == 0u) return 0u;
        if (state->class_objects[class_id - 1u] == value) return class_id;
        slot = (slot + 1u) & (state->reverse_class_capacity - 1u);
    }
    return 0u;
}

static st_heap_primitive_status_t validate_class_object(
    st_heap_primitive_state_t *state, uint32_t represented_class_id,
    st_value_t class_object)
{
    const StClassDescriptor *represented = st_runtime_class(
        state->descriptors, represented_class_id);
    st_object_view_t view;
    st_heap_status_t heap_status;
    if (!represented) return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    heap_status = st_heap_object_view(state->heap, class_object, &view);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    if (view.class_descriptor->class_id != represented->metaclass_id ||
        (view.class_descriptor->flags & ST_CLASS_METACLASS) == 0u)
        return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    return ST_HEAP_PRIMITIVE_OK;
}

st_heap_primitive_status_t st_heap_primitive_context_init(
    st_heap_primitive_context_t *context,
    const st_heap_primitive_options_t *options)
{
    st_primitive_allocator_t allocator;
    st_heap_primitive_state_t *state = NULL;
    const st_runtime_descriptors_t *descriptors;
    size_t reverse_capacity = 1u;
    size_t bytes;
    size_t index;
    st_heap_primitive_status_t status = ST_HEAP_PRIMITIVE_OK;
    if (!context || context->state || !options || !options->heap ||
        !normalize_allocator(options->allocator, &allocator))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    descriptors = st_heap_descriptors(options->heap);
    if (!descriptors || !options->class_objects ||
        options->class_object_count != descriptors->class_count ||
        !options->indexed_access ||
        options->indexed_access_count != descriptors->shape_count ||
        !immediate_ids_are_valid(descriptors, options->immediate_classes))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    if (descriptors->class_count > SIZE_MAX / 2u)
        return ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
    while (reverse_capacity < descriptors->class_count * 2u) {
        if (reverse_capacity > SIZE_MAX / 2u)
            return ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        reverse_capacity *= 2u;
    }
    state = allocator.allocate(allocator.user, sizeof(*state));
    if (!state) return ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY;
    memset(state, 0, sizeof(*state));
    state->allocator = allocator;
    state->heap = options->heap;
    state->descriptors = descriptors;
    state->immediate_classes = options->immediate_classes;
    state->class_object_count = descriptors->class_count;
    state->indexed_access_count = descriptors->shape_count;
    state->reverse_class_capacity = reverse_capacity;
    if (!multiply_size(state->class_object_count,
                       sizeof(*state->class_objects), &bytes)) {
        status = ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        goto fail;
    }
    state->class_objects = allocator.allocate(allocator.user, bytes);
    if (!state->class_objects) {
        status = ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(state->class_objects, options->class_objects, bytes);
    if (!multiply_size(state->indexed_access_count,
                       sizeof(*state->indexed_access), &bytes)) {
        status = ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        goto fail;
    }
    state->indexed_access = allocator.allocate(allocator.user, bytes);
    if (!state->indexed_access) {
        status = ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(state->indexed_access, options->indexed_access, bytes);
    if (!multiply_size(reverse_capacity, sizeof(*state->reverse_class_table),
                       &bytes)) {
        status = ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        goto fail;
    }
    state->reverse_class_table = allocator.allocate(allocator.user, bytes);
    if (!state->reverse_class_table) {
        status = ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memset(state->reverse_class_table, 0, bytes);
    for (index = 0u; index < state->indexed_access_count; ++index) {
        const StShapeDescriptor *shape = st_runtime_shape(
            descriptors, (uint32_t)index + 1u);
        if (!shape || !indexed_access_matches(shape,
                                               state->indexed_access[index])) {
            status = ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
            goto fail;
        }
    }
    for (index = 0u; index < state->class_object_count; ++index) {
        status = validate_class_object(state, (uint32_t)index + 1u,
                                       state->class_objects[index]);
        if (status != ST_HEAP_PRIMITIVE_OK ||
            !reverse_insert(state, (uint32_t)index + 1u)) {
            if (status == ST_HEAP_PRIMITIVE_OK)
                status = ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
            goto fail;
        }
    }
    state->magic = ST_HEAP_PRIMITIVE_MAGIC;
    context->state = state;
    return ST_HEAP_PRIMITIVE_OK;
fail:
    release(allocator, state->reverse_class_table);
    release(allocator, state->indexed_access);
    release(allocator, state->class_objects);
    release(allocator, state);
    return status;
}

void st_heap_primitive_context_destroy(st_heap_primitive_context_t *context)
{
    st_heap_primitive_state_t *state = context_state(context);
    st_primitive_allocator_t allocator;
    if (!state) return;
    allocator = state->allocator;
    state->magic = 0u;
    release(allocator, state->reverse_class_table);
    release(allocator, state->indexed_access);
    release(allocator, state->class_objects);
    release(allocator, state);
    context->state = NULL;
}

const st_value_t *st_heap_primitive_class_roots(
    const st_heap_primitive_context_t *context, size_t *count_out)
{
    st_heap_primitive_state_t *state = context_state(context);
    if (count_out) *count_out = state ? state->class_object_count : 0u;
    return state ? state->class_objects : NULL;
}

static st_heap_primitive_status_t decode_integer(st_value_t value,
                                                 int64_t *integer_out)
{
    if (!st_value_has_valid_encoding(value))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (!st_value_to_small_integer(value, integer_out))
        return ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t validate_stored_value(
    st_heap_primitive_state_t *state, st_value_t value,
    st_object_view_t *child_view_out)
{
    st_heap_status_t heap_status;
    if (child_view_out) memset(child_view_out, 0, sizeof(*child_view_out));
    if (!st_value_has_valid_encoding(value))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (st_value_kind(value) != ST_VALUE_OBJECT)
        return ST_HEAP_PRIMITIVE_OK;
    if (child_view_out) {
        heap_status = st_heap_object_view(state->heap, value, child_view_out);
    } else {
        st_object_extent_t extent;
        heap_status = st_heap_authorize(state->heap, value, &extent);
    }
    if (heap_status == ST_HEAP_ERR_NOT_MEMBER)
        return ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE;
    return map_heap_status(heap_status);
}

static st_heap_primitive_status_t object_receiver(
    st_heap_primitive_state_t *state, st_value_t receiver,
    st_object_view_t *view_out)
{
    st_heap_status_t status;
    if (!st_value_has_valid_encoding(receiver))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (st_value_kind(receiver) != ST_VALUE_OBJECT)
        return ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH;
    status = st_heap_object_view(state->heap, receiver, view_out);
    return map_heap_status(status);
}

static st_heap_primitive_status_t encode_size(size_t size,
                                               st_value_t *result_out)
{
    if (size > (size_t)ST_SMALL_INTEGER_MAX ||
        !st_value_from_small_integer((int64_t)size, result_out))
        return ST_HEAP_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return ST_HEAP_PRIMITIVE_OK;
}

static uint64_t immediate_hash_mix(uint64_t value)
{
    value ^= UINT64_C(0xd6e8feb86659fd93);
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return (value ^ (value >> 31)) & (uint64_t)ST_SMALL_INTEGER_MAX;
}

static st_heap_primitive_status_t primitive_hash(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    uint64_t hash;
    st_heap_status_t heap_status;
    (void)arguments;
    if (!st_value_has_valid_encoding(receiver))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (st_value_kind(receiver) == ST_VALUE_OBJECT) {
        heap_status = st_heap_identity_hash(state->heap, receiver, &hash);
        if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    } else {
        hash = immediate_hash_mix(receiver);
    }
    if (!st_value_from_small_integer((int64_t)hash, result_out))
        return ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t decode_index(
    st_value_t value, size_t length, size_t *zero_based_out)
{
    int64_t index;
    st_heap_primitive_status_t status = decode_integer(value, &index);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    if (index <= 0 || (uint64_t)index > (uint64_t)length)
        return ST_HEAP_PRIMITIVE_ERR_INDEX_OUT_OF_BOUNDS;
    *zero_based_out = (size_t)(index - 1);
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t store_pointer(
    st_heap_primitive_state_t *state, st_object_view_t *target,
    st_value_t *slot, st_value_t value)
{
    st_object_view_t child;
    uint64_t target_header;
    st_heap_primitive_status_t status = validate_stored_value(
        state, value, &child);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    target_header = st_object_header_load(&target->object->header);
    if ((st_object_header_flags(target_header) & ST_HEADER_IMMUTABLE) != 0u)
        return ST_HEAP_PRIMITIVE_ERR_IMMUTABLE;
    if (st_value_kind(value) == ST_VALUE_OBJECT) {
        uint64_t child_header = st_object_header_load(&child.object->header);
        if (st_object_header_generation(target_header) >= ST_GC_OLD &&
            st_object_header_generation(child_header) < ST_GC_OLD &&
            !st_object_header_remember(&target->object->header))
            return ST_HEAP_PRIMITIVE_ERR_WRITE_BARRIER;
    }
    *slot = value;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_size(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    st_heap_primitive_status_t status;
    (void)arguments;
    if (!st_value_has_valid_encoding(receiver))
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (st_value_kind(receiver) != ST_VALUE_OBJECT)
        return encode_size(0u, result_out);
    status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    return encode_size(view.indexed_length, result_out);
}

static uint64_t load_unsigned(const st_object_view_t *view, size_t index)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        return ((const uint8_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT16:
        return ((const uint16_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT32:
        return ((const uint32_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT64:
        return ((const uint64_t *)view->indexed_elements)[index];
    default: return 0u;
    }
}

static size_t indexed_element_size(st_indexed_format_t format)
{
    switch (format) {
    case ST_INDEXED_UINT8: return sizeof(uint8_t);
    case ST_INDEXED_UINT16: return sizeof(uint16_t);
    case ST_INDEXED_UINT32: return sizeof(uint32_t);
    default: return 0u;
    }
}

static bool unicode_scalar_is_valid(uint64_t code_point)
{
    return code_point <= UINT32_C(0x10ffff) &&
           !(code_point >= UINT32_C(0xd800) &&
             code_point <= UINT32_C(0xdfff));
}

static st_heap_primitive_status_t validate_character_view(
    const st_heap_primitive_state_t *state, const st_object_view_t *view)
{
    st_heap_indexed_access_t access =
        state->indexed_access[view->shape_descriptor->shape_id - 1u];
    size_t index;
    if (access != ST_HEAP_INDEXED_ACCESS_CHARACTER ||
        indexed_element_size(view->shape_descriptor->indexed_format) == 0u)
        return ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT;
    for (index = 0u; index < view->indexed_length; ++index)
        if (!unicode_scalar_is_valid(load_unsigned(view, index)))
            return ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_array_equals(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t left;
    st_object_view_t right;
    st_heap_primitive_status_t status;
    st_heap_indexed_access_t right_access;
    size_t index;
    size_t byte_count;
    status = object_receiver(state, receiver, &left);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    status = validate_character_view(state, &left);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    status = object_receiver(state, arguments[0], &right);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    right_access = state->indexed_access[
        right.shape_descriptor->shape_id - 1u];
    if (right_access != ST_HEAP_INDEXED_ACCESS_CHARACTER) {
        *result_out = st_value_false();
        return ST_HEAP_PRIMITIVE_OK;
    }
    status = validate_character_view(state, &right);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    if (left.indexed_length != right.indexed_length) {
        *result_out = st_value_false();
        return ST_HEAP_PRIMITIVE_OK;
    }
    if (left.shape_descriptor->indexed_format ==
            right.shape_descriptor->indexed_format) {
        size_t element_size = indexed_element_size(
            left.shape_descriptor->indexed_format);
        if (!multiply_size(left.indexed_length, element_size, &byte_count))
            return ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        *result_out = byte_count == 0u ||
                      memcmp(left.indexed_elements, right.indexed_elements,
                             byte_count) == 0
            ? st_value_true() : st_value_false();
        return ST_HEAP_PRIMITIVE_OK;
    }
    for (index = 0u; index < left.indexed_length; ++index) {
        if (load_unsigned(&left, index) != load_unsigned(&right, index)) {
            *result_out = st_value_false();
            return ST_HEAP_PRIMITIVE_OK;
        }
    }
    *result_out = st_value_true();
    return ST_HEAP_PRIMITIVE_OK;
}

static uint64_t fnv1a_byte(uint64_t hash, uint8_t byte)
{
    return (hash ^ byte) * UINT64_C(0x100000001b3);
}

/* Frozen StringHash ABI:
 *   FNV-1a-64 over each logical Unicode scalar encoded canonically as four
 *   little-endian bytes, followed by the logical length as eight canonical
 *   little-endian bytes; then the SplitMix64 finalizer and a 60-bit mask.
 * Explicit shifts, rather than object-memory bytes, make this target-endian
 * independent and identical across UINT8/UINT16/UINT32 representations. */
static uint64_t logical_string_hash(const st_object_view_t *view)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t index;
    unsigned byte_index;
    for (index = 0u; index < view->indexed_length; ++index) {
        uint32_t code_point = (uint32_t)load_unsigned(view, index);
        for (byte_index = 0u; byte_index < 4u; ++byte_index)
            hash = fnv1a_byte(hash,
                (uint8_t)(code_point >> (byte_index * 8u)));
    }
    for (byte_index = 0u; byte_index < 8u; ++byte_index)
        hash = fnv1a_byte(hash,
            (uint8_t)((uint64_t)view->indexed_length >>
                      (byte_index * 8u)));
    hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
    return (hash ^ (hash >> 31)) & (uint64_t)ST_SMALL_INTEGER_MAX;
}

static st_heap_primitive_status_t primitive_string_hash(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    uint64_t hash;
    st_heap_primitive_status_t status;
    (void)arguments;
    status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    status = validate_character_view(state, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    hash = logical_string_hash(&view);
    if (!st_value_from_small_integer((int64_t)hash, result_out))
        return ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT;
    return ST_HEAP_PRIMITIVE_OK;
}

static uint64_t format_maximum(st_indexed_format_t format)
{
    switch (format) {
    case ST_INDEXED_UINT8: return UINT8_MAX;
    case ST_INDEXED_UINT16: return UINT16_MAX;
    case ST_INDEXED_UINT32: return UINT32_MAX;
    case ST_INDEXED_UINT64: return UINT64_MAX;
    default: return 0u;
    }
}

static void store_unsigned(st_object_view_t *view, size_t index,
                           uint64_t value)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        ((uint8_t *)view->indexed_elements)[index] = (uint8_t)value;
        break;
    case ST_INDEXED_UINT16:
        ((uint16_t *)view->indexed_elements)[index] = (uint16_t)value;
        break;
    case ST_INDEXED_UINT32:
        ((uint32_t *)view->indexed_elements)[index] = (uint32_t)value;
        break;
    case ST_INDEXED_UINT64:
        ((uint64_t *)view->indexed_elements)[index] = value;
        break;
    default: break;
    }
}

static st_heap_primitive_status_t primitive_at(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    st_heap_indexed_access_t access;
    size_t index;
    uint64_t raw;
    st_heap_primitive_status_t status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    access = state->indexed_access[view.shape_descriptor->shape_id - 1u];
    if (access == ST_HEAP_INDEXED_ACCESS_NONE)
        return ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT;
    status = decode_index(arguments[0], view.indexed_length, &index);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    if (access == ST_HEAP_INDEXED_ACCESS_VALUES) {
        st_value_t value = ((st_value_t *)view.indexed_elements)[index];
        status = validate_stored_value(state, value, NULL);
        if (status != ST_HEAP_PRIMITIVE_OK) return status;
        *result_out = value;
        return ST_HEAP_PRIMITIVE_OK;
    }
    raw = load_unsigned(&view, index);
    if (access == ST_HEAP_INDEXED_ACCESS_CHARACTER) {
        if (raw > UINT32_MAX ||
            !st_value_from_character((uint32_t)raw, result_out))
            return ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT;
        return ST_HEAP_PRIMITIVE_OK;
    }
    if (raw > (uint64_t)ST_SMALL_INTEGER_MAX ||
        !st_value_from_small_integer((int64_t)raw, result_out))
        return ST_HEAP_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_at_put(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    st_heap_indexed_access_t access;
    size_t index;
    uint64_t header;
    st_heap_primitive_status_t status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    access = state->indexed_access[view.shape_descriptor->shape_id - 1u];
    if (access == ST_HEAP_INDEXED_ACCESS_NONE)
        return ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT;
    status = decode_index(arguments[0], view.indexed_length, &index);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    header = st_object_header_load(&view.object->header);
    if ((st_object_header_flags(header) & ST_HEADER_IMMUTABLE) != 0u)
        return ST_HEAP_PRIMITIVE_ERR_IMMUTABLE;
    if (access == ST_HEAP_INDEXED_ACCESS_VALUES) {
        status = store_pointer(state, &view,
            &((st_value_t *)view.indexed_elements)[index], arguments[1]);
        if (status != ST_HEAP_PRIMITIVE_OK) return status;
    } else if (access == ST_HEAP_INDEXED_ACCESS_CHARACTER) {
        uint32_t character;
        if (!st_value_has_valid_encoding(arguments[1]))
            return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
        if (!st_value_to_character(arguments[1], &character))
            return ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH;
        if ((uint64_t)character >
            format_maximum(view.shape_descriptor->indexed_format))
            return ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE;
        store_unsigned(&view, index, character);
    } else {
        int64_t integer;
        status = decode_integer(arguments[1], &integer);
        if (status != ST_HEAP_PRIMITIVE_OK) return status;
        if (integer < 0 || (uint64_t)integer >
            format_maximum(view.shape_descriptor->indexed_format))
            return ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE;
        store_unsigned(&view, index, (uint64_t)integer);
    }
    *result_out = arguments[1];
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_inst_var_at(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    size_t index;
    st_value_t value = (st_value_t)ST_VALUE_INVALID;
    st_heap_primitive_status_t status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    status = decode_index(arguments[0],
                          view.shape_descriptor->fixed_word_count, &index);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    st_heap_status_t heap_status = st_heap_fixed_reference_load(
        state->heap, receiver, index, &value);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    *result_out = value;
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_inst_var_at_put(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_object_view_t view;
    size_t index;
    st_heap_primitive_status_t status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    status = decode_index(arguments[0],
                          view.shape_descriptor->fixed_word_count, &index);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    st_heap_status_t heap_status = st_heap_fixed_reference_store(
        state->heap, receiver, index, arguments[1]);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    *result_out = arguments[1];
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t represented_class(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const StClassDescriptor **class_out)
{
    st_object_view_t view;
    uint32_t class_id;
    st_heap_primitive_status_t status = object_receiver(state, receiver, &view);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    class_id = reverse_lookup(state, receiver);
    if (class_id == 0u) return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    status = validate_class_object(state, class_id, receiver);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    *class_out = st_runtime_class(state->descriptors, class_id);
    return *class_out ? ST_HEAP_PRIMITIVE_OK
                      : ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
}

static st_heap_primitive_status_t allocate_instance(
    st_heap_primitive_state_t *state, st_value_t receiver,
    bool sized, const st_value_t *arguments, st_value_t *result_out)
{
    const StClassDescriptor *represented;
    const StShapeDescriptor *shape;
    size_t size = 0u;
    st_heap_status_t heap_status;
    st_heap_primitive_status_t status = represented_class(
        state, receiver, &represented);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    if ((represented->flags & ST_CLASS_ABSTRACT) != 0u)
        return ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS;
    shape = st_runtime_shape(state->descriptors,
                             represented->default_shape_id);
    if (!shape) return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    if (sized) {
        int64_t requested;
        status = decode_integer(arguments[0], &requested);
        if (status != ST_HEAP_PRIMITIVE_OK) return status;
        if (requested < 0) return ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE;
        if ((uint64_t)requested > SIZE_MAX)
            return ST_HEAP_PRIMITIVE_ERR_OVERFLOW;
        if (shape->indexed_format == ST_INDEXED_NONE)
            return ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT;
        size = (size_t)requested;
    }
    heap_status = st_heap_allocate(state->heap, represented->class_id,
        shape->shape_id, size, size, 0u, result_out);
    return map_heap_status(heap_status);
}

static st_heap_primitive_status_t primitive_behavior_new(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    return allocate_instance(state, receiver, false, arguments, result_out);
}

static st_heap_primitive_status_t primitive_behavior_new_size(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    return allocate_instance(state, receiver, true, arguments, result_out);
}

static st_heap_primitive_status_t value_class_id(
    st_heap_primitive_state_t *state, st_value_t receiver,
    uint32_t *class_id_out)
{
    st_value_kind_t kind = st_value_kind(receiver);
    if (kind == ST_VALUE_INVALID)
        return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    if (kind == ST_VALUE_OBJECT) {
        st_object_view_t view;
        st_heap_primitive_status_t status = object_receiver(
            state, receiver, &view);
        if (status != ST_HEAP_PRIMITIVE_OK) return status;
        *class_id_out = view.class_descriptor->class_id;
        return ST_HEAP_PRIMITIVE_OK;
    }
    switch (kind) {
    case ST_VALUE_SMALL_INTEGER:
        *class_id_out = state->immediate_classes.small_integer_class_id;
        break;
    case ST_VALUE_CHARACTER:
        *class_id_out = state->immediate_classes.character_class_id;
        break;
    case ST_VALUE_NIL:
        *class_id_out = state->immediate_classes.nil_class_id;
        break;
    case ST_VALUE_FALSE:
        *class_id_out = state->immediate_classes.false_class_id;
        break;
    case ST_VALUE_TRUE:
        *class_id_out = state->immediate_classes.true_class_id;
        break;
    default: return ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE;
    }
    return ST_HEAP_PRIMITIVE_OK;
}

static st_heap_primitive_status_t primitive_class(
    st_heap_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    uint32_t class_id;
    st_heap_primitive_status_t status;
    (void)arguments;
    status = value_class_id(state, receiver, &class_id);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    if (class_id == 0u || class_id > state->class_object_count)
        return ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP;
    status = validate_class_object(state, class_id,
                                   state->class_objects[class_id - 1u]);
    if (status != ST_HEAP_PRIMITIVE_OK) return status;
    *result_out = state->class_objects[class_id - 1u];
    return ST_HEAP_PRIMITIVE_OK;
}

static const heap_primitive_definition_t definitions[] = {
    { ST_INTRINSIC_SIZE, 0u, primitive_size },
    { ST_INTRINSIC_AT, 1u, primitive_at },
    { ST_INTRINSIC_AT_PUT, 2u, primitive_at_put },
    { ST_INTRINSIC_INST_VAR_AT, 1u, primitive_inst_var_at },
    { ST_INTRINSIC_INST_VAR_AT_PUT, 2u, primitive_inst_var_at_put },
    { ST_INTRINSIC_BEHAVIOR_NEW, 0u, primitive_behavior_new },
    { ST_INTRINSIC_BEHAVIOR_NEW_SIZE, 1u,
      primitive_behavior_new_size },
    { ST_INTRINSIC_CLASS, 0u, primitive_class },
    { ST_INTRINSIC_HASH, 0u, primitive_hash },
    { ST_INTRINSIC_ARRAY_EQUALS, 1u, primitive_array_equals },
    { ST_INTRINSIC_STRING_HASH, 0u, primitive_string_hash }
};

_Static_assert(sizeof(heap_specs) / sizeof(heap_specs[0]) ==
                   sizeof(definitions) / sizeof(definitions[0]),
               "every heap primitive spec must have one handler");

st_heap_primitive_status_t st_heap_primitive_execute(
    st_heap_primitive_context_t *context, uint32_t intrinsic_id,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out)
{
    st_heap_primitive_state_t *state = context_state(context);
    size_t index;
    if (!result_out) return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    *result_out = 0u;
    if (!state) return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    for (index = 0u; index < sizeof(definitions) / sizeof(definitions[0]);
         ++index) {
        const heap_primitive_definition_t *definition = &definitions[index];
        if (definition->id != intrinsic_id) continue;
        if (argument_count != definition->arity)
            return ST_HEAP_PRIMITIVE_ERR_WRONG_ARITY;
        if (argument_count != 0u && !arguments)
            return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
        return definition->handler(state, receiver, arguments, result_out);
    }
    return ST_HEAP_PRIMITIVE_ERR_UNKNOWN_INTRINSIC;
}

const st_primitive_spec_t *st_heap_primitive_specs(size_t *count_out)
{
    if (count_out) *count_out = sizeof(heap_specs) / sizeof(heap_specs[0]);
    return heap_specs;
}

const char *st_heap_primitive_status_string(st_heap_primitive_status_t status)
{
    switch (status) {
    case ST_HEAP_PRIMITIVE_OK: return "ok";
    case ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_HEAP_PRIMITIVE_ERR_UNKNOWN_INTRINSIC:
        return "unknown intrinsic";
    case ST_HEAP_PRIMITIVE_ERR_WRONG_ARITY: return "wrong arity";
    case ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH: return "type mismatch";
    case ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER: return "object is not a heap member";
    case ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return "dangling or foreign object reference";
    case ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP:
        return "invalid class object or shape policy map";
    case ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_HEAP_PRIMITIVE_ERR_OVERFLOW: return "size overflow";
    case ST_HEAP_PRIMITIVE_ERR_PROMOTION_REQUIRED:
        return "integer promotion required";
    case ST_HEAP_PRIMITIVE_ERR_INDEX_OUT_OF_BOUNDS:
        return "index out of bounds";
    case ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT:
        return "object has no compatible indexed storage";
    case ST_HEAP_PRIMITIVE_ERR_BAD_SLOT_FORMAT:
        return "instance slot is not an StValue reference";
    case ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE:
        return "value is out of storage range";
    case ST_HEAP_PRIMITIVE_ERR_IMMUTABLE: return "immutable object";
    case ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS:
        return "cannot instantiate abstract class";
    case ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT: return "malformed object";
    case ST_HEAP_PRIMITIVE_ERR_WRITE_BARRIER:
        return "write barrier failed";
    default: return "unknown heap primitive status";
    }
}
