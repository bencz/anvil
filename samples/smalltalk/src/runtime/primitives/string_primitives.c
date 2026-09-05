#include "st_string_primitives.h"

#include <stdlib.h>
#include <string.h>

#define ST_STRING_PRIMITIVE_COOKIE UINT64_C(0x5354535452494e47)

#define RUNTIME_SPEC(name_, symbol_)                                        \
    {                                                                        \
        (name_), sizeof(name_) - 1u, 1u, ST_PRIMITIVE_INSTANCE_ONLY,         \
        ST_PRIMITIVE_FALL_THROUGH, ST_PRIMITIVE_RUNTIME_SYMBOL,              \
        ST_PRIMITIVE_INVALID_INTRINSIC_ID, (symbol_), sizeof(symbol_) - 1u   \
    }

static const st_primitive_spec_t string_specs[] = {
    RUNTIME_SPEC(
        "StringComparePrimitive",
        "st_aot_string_compare_primitive_execute"),
    RUNTIME_SPEC(
        "StringConcatPrimitive",
        "st_aot_string_concat_primitive_execute"),
    {
        "StringAsSymbolPrimitive", sizeof("StringAsSymbolPrimitive") - 1u,
        0u, ST_PRIMITIVE_INSTANCE_ONLY, ST_PRIMITIVE_FALL_THROUGH,
        ST_PRIMITIVE_RUNTIME_SYMBOL, ST_PRIMITIVE_INVALID_INTRINSIC_ID,
        "st_aot_string_as_symbol_primitive_execute",
        sizeof("st_aot_string_as_symbol_primitive_execute") - 1u
    },
    {
        "StringFromCharactersPrimitive", sizeof("StringFromCharactersPrimitive") - 1u,
        1u, ST_PRIMITIVE_CLASS_ONLY, ST_PRIMITIVE_FALL_THROUGH,
        ST_PRIMITIVE_RUNTIME_SYMBOL, ST_PRIMITIVE_INVALID_INTRINSIC_ID,
        "st_aot_string_from_characters", sizeof("st_aot_string_from_characters") - 1u
    }
};

#undef RUNTIME_SPEC

static bool shape_matches(const st_runtime_descriptors_t *descriptors,
                          uint32_t class_id, uint32_t shape_id,
                          st_indexed_format_t format);

static bool context_is_live(const st_string_primitive_context_t *context)
{
    const st_runtime_descriptors_t *descriptors;
    const StClassDescriptor *class_descriptor;
    if (context == NULL
            || context->abi_cookie != ST_STRING_PRIMITIVE_COOKIE
            || context->heap == NULL || context->string_class_id == 0u
            || context->uint8_shape_id == 0u
            || context->uint16_shape_id == 0u
            || context->uint32_shape_id == 0u
            || context->uint8_shape_id == context->uint16_shape_id
            || context->uint8_shape_id == context->uint32_shape_id
            || context->uint16_shape_id == context->uint32_shape_id)
        return false;
    descriptors = st_heap_descriptors(context->heap);
    class_descriptor = st_runtime_class(descriptors,
                                         context->string_class_id);
    return class_descriptor != NULL
        && (class_descriptor->flags & (ST_CLASS_METACLASS
                                       | ST_CLASS_ABSTRACT)) == 0u
        && shape_matches(descriptors, context->string_class_id,
                         context->uint8_shape_id, ST_INDEXED_UINT8)
        && shape_matches(descriptors, context->string_class_id,
                         context->uint16_shape_id, ST_INDEXED_UINT16)
        && shape_matches(descriptors, context->string_class_id,
                         context->uint32_shape_id, ST_INDEXED_UINT32);
}

static st_string_primitive_status_t map_heap_status(st_heap_status_t status);

st_string_primitive_status_t st_string_primitive_from_characters(st_string_primitive_context_t *context, st_value_t array, st_value_t *result_out)
{
    if (result_out == NULL)
        return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;

    if (!context_is_live(context))
        return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;

    st_object_view_t source;

    if (st_heap_object_view(context->heap, array, &source) != ST_HEAP_OK
            || source.shape_descriptor->fixed_word_count != 0u || source.shape_descriptor->indexed_format != ST_INDEXED_VALUES)
        return ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH;

    const st_value_t *characters = source.indexed_elements;
    uint32_t maximum = 0u;

    for (size_t index = 0u; index < source.indexed_length; index++) {
        uint32_t code_point;

        if (!st_value_to_character(characters[index], &code_point))
            return ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH;

        if (code_point > maximum)
            maximum = code_point;
    }

    uint32_t shape = maximum <= UINT8_MAX ? context->uint8_shape_id : maximum <= UINT16_MAX ? context->uint16_shape_id : context->uint32_shape_id;
    st_value_t value;
    st_heap_status_t status = st_heap_allocate(context->heap, context->string_class_id, shape, source.indexed_length, source.indexed_length, ST_HEADER_IMMUTABLE, &value);

    if (status != ST_HEAP_OK)
        return map_heap_status(status);

    st_object_view_t destination;

    if (st_heap_object_view(context->heap, value, &destination) != ST_HEAP_OK)
        return ST_STRING_PRIMITIVE_ERR_BAD_OBJECT;

    for (size_t index = 0u; index < source.indexed_length; index++) {
        uint32_t code_point;

        st_value_to_character(characters[index], &code_point);

        if (maximum <= UINT8_MAX)
            ((uint8_t *)destination.indexed_elements)[index] = (uint8_t)code_point;
        else if (maximum <= UINT16_MAX)
            ((uint16_t *)destination.indexed_elements)[index] = (uint16_t)code_point;
        else
            ((uint32_t *)destination.indexed_elements)[index] = code_point;
    }

    *result_out = value;
    return ST_STRING_PRIMITIVE_OK;
}

static st_string_primitive_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK: return ST_STRING_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_STRING_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_STRING_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW: return ST_STRING_PRIMITIVE_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER: return ST_STRING_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return ST_STRING_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_HEAP_ERR_BAD_ALIGNMENT:
    case ST_HEAP_ERR_BAD_EXTENT:
    case ST_HEAP_ERR_BAD_OBJECT:
    case ST_HEAP_ERR_INVALID_ROOT:
    case ST_HEAP_ERR_INVALID_FRAME:
    case ST_HEAP_ERR_FRAME_CYCLE:
    case ST_HEAP_ERR_RECLAIM_PROTOCOL:
    case ST_HEAP_ERR_CONFLICT:
    default: return ST_STRING_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

static bool shape_matches(const st_runtime_descriptors_t *descriptors,
                          uint32_t class_id, uint32_t shape_id,
                          st_indexed_format_t format)
{
    const StShapeDescriptor *shape = st_runtime_shape(descriptors, shape_id);
    return shape != NULL && shape->class_id == class_id
        && shape->fixed_word_count == 0u
        && shape->fixed_pointer_bitmap_word_count == 0u
        && shape->fixed_pointer_bitmap == NULL
        && shape->indexed_format == format;
}

st_string_primitive_status_t st_string_primitive_context_init(
    st_string_primitive_context_t *context,
    const st_string_primitive_options_t *options)
{
    const st_runtime_descriptors_t *descriptors;
    const StClassDescriptor *class_descriptor;
    if (context == NULL) return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context));
    if (options == NULL || options->heap == NULL
            || options->string_class_id == 0u
            || options->uint8_shape_id == 0u
            || options->uint16_shape_id == 0u
            || options->uint32_shape_id == 0u
            || options->uint8_shape_id == options->uint16_shape_id
            || options->uint8_shape_id == options->uint32_shape_id
            || options->uint16_shape_id == options->uint32_shape_id) {
        return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    descriptors = st_heap_descriptors(options->heap);
    class_descriptor = st_runtime_class(descriptors,
                                         options->string_class_id);
    if (class_descriptor == NULL
            || (class_descriptor->flags & (ST_CLASS_METACLASS
                                            | ST_CLASS_ABSTRACT)) != 0u
            || !shape_matches(descriptors, options->string_class_id,
                              options->uint8_shape_id, ST_INDEXED_UINT8)
            || !shape_matches(descriptors, options->string_class_id,
                              options->uint16_shape_id, ST_INDEXED_UINT16)
            || !shape_matches(descriptors, options->string_class_id,
                              options->uint32_shape_id, ST_INDEXED_UINT32)) {
        return ST_STRING_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    context->heap = options->heap;
    context->string_class_id = options->string_class_id;
    context->uint8_shape_id = options->uint8_shape_id;
    context->uint16_shape_id = options->uint16_shape_id;
    context->uint32_shape_id = options->uint32_shape_id;
    context->abi_cookie = ST_STRING_PRIMITIVE_COOKIE;
    return ST_STRING_PRIMITIVE_OK;
}

void st_string_primitive_context_destroy(
    st_string_primitive_context_t *context)
{
    if (context != NULL) memset(context, 0, sizeof(*context));
}

bool st_string_primitive_combined_length(size_t left, size_t right,
                                         size_t *length_out)
{
    if (length_out == NULL) return false;
    *length_out = 0u;
    if (right > SIZE_MAX - left) return false;
    *length_out = left + right;
    return true;
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

static bool context_shape(const st_string_primitive_context_t *context,
                          const StShapeDescriptor *shape)
{
    return shape != NULL
        && (shape->shape_id == context->uint8_shape_id
            || shape->shape_id == context->uint16_shape_id
            || shape->shape_id == context->uint32_shape_id);
}

static st_string_primitive_status_t string_view(
    const st_string_primitive_context_t *context, st_value_t value,
    st_object_view_t *view_out, uint32_t *maximum_out)
{
    st_heap_status_t heap_status;
    uint32_t maximum = 0u;
    size_t index;
    if (!st_value_has_valid_encoding(value))
        return ST_STRING_PRIMITIVE_ERR_INVALID_VALUE;
    heap_status = st_heap_object_view(context->heap, value, view_out);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    if (view_out->class_descriptor->class_id != context->string_class_id
            || !context_shape(context, view_out->shape_descriptor)) {
        return ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH;
    }
    for (index = 0u; index < view_out->indexed_length; index++) {
        uint32_t code_point = load_code_point(view_out, index);
        if (!unicode_scalar_is_valid(code_point))
            return ST_STRING_PRIMITIVE_ERR_BAD_OBJECT;
        if (code_point > maximum) maximum = code_point;
    }
    if (maximum_out != NULL) *maximum_out = maximum;
    return ST_STRING_PRIMITIVE_OK;
}

static st_string_primitive_status_t compare_strings(
    const st_string_primitive_context_t *context, st_value_t receiver,
    st_value_t argument, st_value_t *result_out)
{
    st_object_view_t left;
    st_object_view_t right;
    st_string_primitive_status_t status;
    size_t limit;
    size_t index;
    int64_t comparison = 0;
    status = string_view(context, receiver, &left, NULL);
    if (status != ST_STRING_PRIMITIVE_OK) return status;
    status = string_view(context, argument, &right, NULL);
    if (status != ST_STRING_PRIMITIVE_OK) return status;
    limit = left.indexed_length < right.indexed_length
        ? left.indexed_length : right.indexed_length;
    for (index = 0u; index < limit; index++) {
        uint32_t lhs = load_code_point(&left, index);
        uint32_t rhs = load_code_point(&right, index);
        if (lhs == rhs) continue;
        comparison = lhs < rhs ? -1 : 1;
        break;
    }
    if (comparison == 0 && left.indexed_length != right.indexed_length)
        comparison = left.indexed_length < right.indexed_length ? -1 : 1;
    if (!st_value_from_small_integer(comparison, result_out))
        return ST_STRING_PRIMITIVE_ERR_BAD_OBJECT;
    return ST_STRING_PRIMITIVE_OK;
}

static void store_code_point(st_object_view_t *view, size_t index,
                             uint32_t code_point)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        ((uint8_t *)view->indexed_elements)[index] = (uint8_t)code_point;
        break;
    case ST_INDEXED_UINT16:
        ((uint16_t *)view->indexed_elements)[index] = (uint16_t)code_point;
        break;
    case ST_INDEXED_UINT32:
        ((uint32_t *)view->indexed_elements)[index] = code_point;
        break;
    default: abort();
    }
}

static st_string_primitive_status_t concatenate_strings(
    st_string_primitive_context_t *context, st_value_t receiver,
    st_value_t argument, st_value_t *result_out)
{
    st_object_view_t left;
    st_object_view_t right;
    st_object_view_t output;
    st_string_primitive_status_t status;
    st_heap_status_t heap_status;
    st_value_t value;
    size_t length;
    size_t index;
    uint32_t left_maximum;
    uint32_t right_maximum;
    uint32_t maximum;
    uint32_t shape_id;
    status = string_view(context, receiver, &left, &left_maximum);
    if (status != ST_STRING_PRIMITIVE_OK) return status;
    status = string_view(context, argument, &right, &right_maximum);
    if (status != ST_STRING_PRIMITIVE_OK) return status;
    if (!st_string_primitive_combined_length(
            left.indexed_length, right.indexed_length, &length)) {
        return ST_STRING_PRIMITIVE_ERR_OVERFLOW;
    }
    maximum = left_maximum > right_maximum ? left_maximum : right_maximum;
    shape_id = maximum <= UINT8_MAX ? context->uint8_shape_id
        : maximum <= UINT16_MAX ? context->uint16_shape_id
        : context->uint32_shape_id;
    heap_status = st_heap_allocate(context->heap, context->string_class_id,
                                   shape_id, length, length,
                                   ST_HEADER_IMMUTABLE, &value);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    heap_status = st_heap_object_view(context->heap, value, &output);
    if (heap_status != ST_HEAP_OK || output.indexed_length != length
            || output.shape_descriptor->shape_id != shape_id) {
        /* A heap which cannot authenticate its own just-published allocation
         * has violated the exact-base registry invariant. */
        abort();
    }
    for (index = 0u; index < left.indexed_length; index++)
        store_code_point(&output, index, load_code_point(&left, index));
    for (index = 0u; index < right.indexed_length; index++)
        store_code_point(&output, left.indexed_length + index,
                         load_code_point(&right, index));
    *result_out = value;
    return ST_STRING_PRIMITIVE_OK;
}

st_string_primitive_status_t st_string_primitive_execute(
    st_string_primitive_context_t *context, st_string_operation_t operation,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out)
{
    if (result_out == NULL) return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context))
        return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (operation != ST_STRING_OPERATION_COMPARE
            && operation != ST_STRING_OPERATION_CONCAT)
        return ST_STRING_PRIMITIVE_ERR_UNKNOWN_INTRINSIC;
    if (argument_count != 1u) return ST_STRING_PRIMITIVE_ERR_WRONG_ARITY;
    if (arguments == NULL) return ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (operation == ST_STRING_OPERATION_COMPARE)
        return compare_strings(context, receiver, arguments[0], result_out);
    return concatenate_strings(context, receiver, arguments[0], result_out);
}

const st_primitive_spec_t *st_string_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(string_specs) / sizeof(string_specs[0]);
    return string_specs;
}

const char *st_string_primitive_status_string(
    st_string_primitive_status_t status)
{
    switch (status) {
    case ST_STRING_PRIMITIVE_OK: return "ok";
    case ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_STRING_PRIMITIVE_ERR_UNKNOWN_INTRINSIC:
        return "unknown intrinsic";
    case ST_STRING_PRIMITIVE_ERR_WRONG_ARITY: return "wrong arity";
    case ST_STRING_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH: return "not an exact String";
    case ST_STRING_PRIMITIVE_ERR_NOT_MEMBER:
        return "object is not a heap member";
    case ST_STRING_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return "dangling or foreign object reference";
    case ST_STRING_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return "invalid String class or shape descriptor";
    case ST_STRING_PRIMITIVE_ERR_BAD_OBJECT:
        return "malformed String object or Unicode scalar";
    case ST_STRING_PRIMITIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_STRING_PRIMITIVE_ERR_OVERFLOW: return "size overflow";
    default: return "unknown String primitive status";
    }
}
