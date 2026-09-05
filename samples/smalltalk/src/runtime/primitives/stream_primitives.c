#include "st_stream_primitives.h"
#include "../../platform/runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ST_STREAM_PRIMITIVE_COOKIE UINT64_C(0x535453545245414d)

static const st_primitive_spec_t stream_specs[] = {
    {
        "StreamWritePrimitive", sizeof("StreamWritePrimitive") - 1u,
        3u, ST_PRIMITIVE_CLASS_ONLY, ST_PRIMITIVE_FALL_THROUGH,
        ST_PRIMITIVE_RUNTIME_SYMBOL, ST_PRIMITIVE_INVALID_INTRINSIC_ID,
        "st_aot_stream_write_primitive_execute",
        sizeof("st_aot_stream_write_primitive_execute") - 1u
    }
};

static bool context_is_live(const st_stream_primitive_context_t *context)
{
    return context != NULL
        && context->abi_cookie == ST_STREAM_PRIMITIVE_COOKIE
        && context->heap != NULL && context->string_class_id != 0u
        && context->string_shape_id != 0u && context->write_bytes != NULL;
}

static st_stream_primitive_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK: return ST_STREAM_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_STREAM_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER:
        return ST_STREAM_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return ST_STREAM_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_HEAP_ERR_BAD_ALIGNMENT:
    case ST_HEAP_ERR_BAD_EXTENT:
    case ST_HEAP_ERR_BAD_OBJECT:
    case ST_HEAP_ERR_INVALID_ROOT:
    case ST_HEAP_ERR_INVALID_FRAME:
    case ST_HEAP_ERR_FRAME_CYCLE:
    case ST_HEAP_ERR_RECLAIM_PROTOCOL:
    case ST_HEAP_ERR_CONFLICT:
    case ST_HEAP_ERR_OUT_OF_MEMORY:
    case ST_HEAP_ERR_OVERFLOW:
    default: return ST_STREAM_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

st_stream_primitive_status_t st_stream_primitive_context_init(
    st_stream_primitive_context_t *context,
    const st_stream_primitive_options_t *options)
{
    const st_runtime_descriptors_t *descriptors;
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    if (context == NULL || options == NULL || options->heap == NULL
            || context->abi_cookie != 0u || context->heap != NULL
            || context->write_bytes != NULL
            || options->string_class_id == 0u
            || options->string_shape_id == 0u)
        return ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    descriptors = st_heap_descriptors(options->heap);
    class_descriptor = st_runtime_class(descriptors,
                                        options->string_class_id);
    shape = st_runtime_shape(descriptors, options->string_shape_id);
    if (descriptors == NULL || class_descriptor == NULL || shape == NULL
            || !st_class_descriptor_is_valid(class_descriptor)
            || !st_shape_descriptor_is_valid(shape)
            || class_descriptor->class_id != options->string_class_id
            || class_descriptor->default_shape_id
                != options->string_shape_id
            || (class_descriptor->flags
                & (ST_CLASS_METACLASS | ST_CLASS_ABSTRACT)) != 0u
            || shape->shape_id != options->string_shape_id
            || shape->class_id != options->string_class_id
            || shape->fixed_word_count != 0u
            || shape->indexed_format != ST_INDEXED_UINT8
            || shape->fixed_pointer_bitmap != NULL
            || shape->fixed_pointer_bitmap_word_count != 0u)
        return ST_STREAM_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    *context = (st_stream_primitive_context_t) {
        .abi_cookie = ST_STREAM_PRIMITIVE_COOKIE,
        .heap = options->heap,
        .string_class_id = options->string_class_id,
        .string_shape_id = options->string_shape_id,
        .write_bytes = options->write_bytes != NULL
            ? options->write_bytes : st_runtime_platform.write_bytes,
        .write_user = options->write_user
    };
    return ST_STREAM_PRIMITIVE_OK;
}

void st_stream_primitive_context_destroy(
    st_stream_primitive_context_t *context)
{
    if (context_is_live(context))
        memset(context, 0, sizeof(*context));
}

st_stream_primitive_status_t st_stream_write_primitive_execute(
    st_stream_primitive_context_t *context, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, int *os_error_out)
{
    st_object_view_t string_view;
    st_heap_status_t heap_status;
    int64_t descriptor_value;
    int64_t requested_value;
    size_t requested;
    size_t written = 0u;
    const unsigned char *bytes;
    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (os_error_out != NULL) *os_error_out = 0;
    if (result_out == NULL || os_error_out == NULL)
        return ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (!context_is_live(context))
        return ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (argument_count != 3u)
        return ST_STREAM_PRIMITIVE_ERR_WRONG_ARITY;
    if (arguments == NULL)
        return ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (!st_value_has_valid_encoding(receiver)
            || !st_value_has_valid_encoding(arguments[0])
            || !st_value_has_valid_encoding(arguments[1])
            || !st_value_has_valid_encoding(arguments[2]))
        return ST_STREAM_PRIMITIVE_ERR_INVALID_VALUE;
    if (!st_value_to_small_integer(arguments[0], &descriptor_value)
            || !st_value_to_small_integer(arguments[1], &requested_value))
        return ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH;
    if (descriptor_value < 0 || descriptor_value > INT_MAX)
        return ST_STREAM_PRIMITIVE_ERR_DESCRIPTOR_OUT_OF_RANGE;
    if (requested_value < 0
            || (uint64_t)requested_value > (uint64_t)SIZE_MAX)
        return ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE;
    requested = (size_t)requested_value;
    heap_status = st_heap_object_view(context->heap, arguments[2],
                                      &string_view);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    if (string_view.class_descriptor->class_id != context->string_class_id
            || string_view.shape_descriptor->shape_id
                != context->string_shape_id
            || string_view.shape_descriptor->indexed_format
                != ST_INDEXED_UINT8)
        return ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH;
    if (requested > string_view.indexed_length)
        return ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE;
    bytes = string_view.indexed_elements;
    while (written < requested) {
        size_t remaining = requested - written;
        size_t chunk = remaining;
        int write_error = 0;
        int64_t amount;
#if defined(_WIN32)
        if (chunk > UINT_MAX) chunk = UINT_MAX;
#elif defined(SSIZE_MAX)
        if (chunk > (size_t)SSIZE_MAX) chunk = (size_t)SSIZE_MAX;
#else
        if (chunk > (SIZE_MAX >> 1u)) chunk = SIZE_MAX >> 1u;
#endif
        amount = context->write_bytes(context->write_user,
                                      (int)descriptor_value,
                                      bytes + written, chunk, &write_error);
        if (amount < 0) {
            if (write_error == EINTR) continue;
            *os_error_out = write_error > 0 ? write_error : EIO;
            return ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED;
        }
        if (amount == 0)
            return ST_STREAM_PRIMITIVE_ERR_ZERO_PROGRESS;
        if ((uint64_t)amount > (uint64_t)chunk)
            return ST_STREAM_PRIMITIVE_ERR_WRITE_CONTRACT;
        written += (size_t)amount;
    }
    if (!st_value_from_small_integer(requested_value, result_out))
        return ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE;
    return ST_STREAM_PRIMITIVE_OK;
}

const st_primitive_spec_t *st_stream_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(stream_specs) / sizeof(stream_specs[0]);
    return stream_specs;
}

const char *st_stream_primitive_status_string(
    st_stream_primitive_status_t status)
{
    switch (status) {
    case ST_STREAM_PRIMITIVE_OK: return "ok";
    case ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_STREAM_PRIMITIVE_ERR_WRONG_ARITY: return "wrong arity";
    case ST_STREAM_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH: return "type mismatch";
    case ST_STREAM_PRIMITIVE_ERR_NOT_MEMBER:
        return "object is not a heap member";
    case ST_STREAM_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return "dangling or foreign object reference";
    case ST_STREAM_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return "invalid String descriptor";
    case ST_STREAM_PRIMITIVE_ERR_BAD_OBJECT: return "malformed object";
    case ST_STREAM_PRIMITIVE_ERR_DESCRIPTOR_OUT_OF_RANGE:
        return "descriptor is outside the host descriptor range";
    case ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE:
        return "byte count is outside the String bounds";
    case ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED: return "write failed";
    case ST_STREAM_PRIMITIVE_ERR_ZERO_PROGRESS:
        return "write made zero progress";
    case ST_STREAM_PRIMITIVE_ERR_WRITE_CONTRACT:
        return "write callback violated its byte-count contract";
    default: return "unknown stream primitive status";
    }
}
