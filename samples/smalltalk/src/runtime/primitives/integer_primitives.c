#include "st_integer_primitives.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ST_NUMERIC_CONTEXT_COOKIE UINT64_C(0x53544e554d455231)
#define ST_LARGE_INTEGER_METADATA_MASK UINT64_C(0xfffffffffffffffe)
#define ST_LARGE_INTEGER_METADATA_MAGIC UINT64_C(0x4c494e5400010000)
#define ST_LARGE_INTEGER_NEGATIVE UINT64_C(1)

#define RUNTIME_SPEC(name_, arity_, failure_, symbol_)                      \
    {                                                                       \
        (name_), sizeof(name_) - 1u, (arity_),                              \
        ST_PRIMITIVE_INSTANCE_ONLY, (failure_),                             \
        ST_PRIMITIVE_RUNTIME_SYMBOL, ST_PRIMITIVE_INVALID_INTRINSIC_ID,      \
        (symbol_), sizeof(symbol_) - 1u                                     \
    }

static const st_primitive_spec_t integer_specs[] = {
    RUNTIME_SPEC(
        "LargeIntBinaryPrimitive", 2u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_large_integer_binary_primitive_execute"),
    RUNTIME_SPEC(
        "LargeIntComparePrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_large_integer_compare_primitive_execute"),
    RUNTIME_SPEC(
        "LargeIntShiftPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_large_integer_shift_primitive_execute"),
    RUNTIME_SPEC(
        "LargeIntAsFloatPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_large_integer_as_float_primitive_execute"),
    RUNTIME_SPEC(
        "IntAsFloatPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,
        "st_aot_small_integer_as_float_primitive_execute"),
    RUNTIME_SPEC(
        "IntegerHashPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,
        "st_aot_integer_hash_primitive_execute")
};

#undef RUNTIME_SPEC

typedef struct {
    bool negative;
    const uint32_t *limbs;
    size_t count;
    uint32_t storage[2];
} integer_operand_t;

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

static bool context_is_live(const st_numeric_context_t *context)
{
    return context != NULL
        && context->abi_cookie == ST_NUMERIC_CONTEXT_COOKIE
        && context->heap != NULL
        && context->large_positive_class_id != 0u
        && context->large_positive_shape_id != 0u
        && context->large_negative_class_id != 0u
        && context->large_negative_shape_id != 0u
        && context->large_positive_class_id
            != context->large_negative_class_id
        && context->large_positive_shape_id
            != context->large_negative_shape_id
        && context->float_primitives != NULL
        && context->scratch_allocator.allocate != NULL
        && context->scratch_allocator.deallocate != NULL;
}

static st_integer_primitive_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK:
        return ST_INTEGER_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW:
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER:
        return ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return ST_INTEGER_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_HEAP_ERR_BAD_ALIGNMENT:
    case ST_HEAP_ERR_BAD_EXTENT:
    case ST_HEAP_ERR_BAD_OBJECT:
    case ST_HEAP_ERR_INVALID_ROOT:
    case ST_HEAP_ERR_INVALID_FRAME:
    case ST_HEAP_ERR_FRAME_CYCLE:
    case ST_HEAP_ERR_RECLAIM_PROTOCOL:
    case ST_HEAP_ERR_CONFLICT:
    default:
        return ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

static bool large_shape_is_valid(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id,
    uint32_t shape_id)
{
    const StClassDescriptor *class_descriptor = st_runtime_class(
        descriptors, class_id);
    const StShapeDescriptor *shape = st_runtime_shape(descriptors, shape_id);

    return class_descriptor != NULL && shape != NULL
        && st_class_descriptor_is_valid(class_descriptor)
        && st_shape_descriptor_is_valid(shape)
        && class_descriptor->class_id == class_id
        && class_descriptor->default_shape_id == shape_id
        && (class_descriptor->flags
            & (ST_CLASS_METACLASS | ST_CLASS_ABSTRACT)) == 0u
        && shape->shape_id == shape_id
        && shape->class_id == class_id
        && shape->fixed_word_count == 1u
        && shape->indexed_format == ST_INDEXED_UINT32
        && shape->fixed_pointer_bitmap != NULL
        && shape->fixed_pointer_bitmap_word_count == 1u
        && (shape->fixed_pointer_bitmap[0] & UINT64_C(1)) == 0u;
}

st_integer_primitive_status_t st_numeric_context_init(
    st_numeric_context_t *context, const st_numeric_options_t *options)
{
    const st_runtime_descriptors_t *descriptors;
    st_primitive_allocator_t allocator;

    if (context == NULL || context->abi_cookie != 0u)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    memset(context, 0, sizeof(*context));
    if (options == NULL || options->heap == NULL
            || options->large_positive_class_id == 0u
            || options->large_positive_shape_id == 0u
            || options->large_negative_class_id == 0u
            || options->large_negative_shape_id == 0u
            || options->large_positive_class_id
                == options->large_negative_class_id
            || options->large_positive_shape_id
                == options->large_negative_shape_id
            || options->float_primitives == NULL
            || st_float_primitive_context_heap(options->float_primitives)
                != options->heap
            || !normalize_allocator(options->scratch_allocator, &allocator))
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;

    descriptors = st_heap_descriptors(options->heap);
    if (descriptors == NULL
            || !large_shape_is_valid(
                descriptors, options->large_positive_class_id,
                options->large_positive_shape_id)
            || !large_shape_is_valid(
                descriptors, options->large_negative_class_id,
                options->large_negative_shape_id))
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_DESCRIPTOR;

    context->abi_cookie = ST_NUMERIC_CONTEXT_COOKIE;
    context->heap = options->heap;
    context->large_positive_class_id = options->large_positive_class_id;
    context->large_positive_shape_id = options->large_positive_shape_id;
    context->large_negative_class_id = options->large_negative_class_id;
    context->large_negative_shape_id = options->large_negative_shape_id;
    context->float_primitives = options->float_primitives;
    context->scratch_allocator = allocator;
    return ST_INTEGER_PRIMITIVE_OK;
}

void st_numeric_context_destroy(st_numeric_context_t *context)
{
    if (context != NULL) memset(context, 0, sizeof(*context));
}

static size_t normalized_count(const uint32_t *limbs, size_t count)
{
    while (count != 0u && limbs[count - 1u] == 0u) count--;
    return count;
}

static bool magnitude_fits_small(bool negative, const uint32_t *limbs,
                                 size_t count, int64_t *integer_out)
{
    uint64_t magnitude;
    uint64_t limit = negative
        ? UINT64_C(0x1000000000000000)
        : UINT64_C(0x0fffffffffffffff);

    count = normalized_count(limbs, count);
    if (count > 2u) return false;
    magnitude = count == 0u ? 0u : limbs[0];
    if (count == 2u) magnitude |= (uint64_t)limbs[1] << 32u;
    if (magnitude > limit) return false;
    if (integer_out != NULL) {
        if (!negative || magnitude == 0u)
            *integer_out = (int64_t)magnitude;
        else if (magnitude == (UINT64_C(1) << 60u))
            *integer_out = ST_SMALL_INTEGER_MIN;
        else
            *integer_out = -(int64_t)magnitude;
    }
    return true;
}

static st_integer_primitive_status_t publish_magnitude(
    st_numeric_context_t *context, bool negative, const uint32_t *limbs,
    size_t count, st_value_t *result_out)
{
    st_object_view_t view;
    st_value_t result = ST_VALUE_INVALID;
    st_heap_status_t heap_status;
    int64_t small;
    uint32_t class_id;
    uint32_t shape_id;
    uint64_t metadata;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL
            || (count != 0u && limbs == NULL))
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;

    count = normalized_count(limbs, count);
    if (magnitude_fits_small(negative, limbs, count, &small)) {
        if (!st_value_from_small_integer(small, result_out)) abort();
        return ST_INTEGER_PRIMITIVE_OK;
    }
    if (count == 0u) abort();

    class_id = negative
        ? context->large_negative_class_id
        : context->large_positive_class_id;
    shape_id = negative
        ? context->large_negative_shape_id
        : context->large_positive_shape_id;
    heap_status = st_heap_allocate(
        context->heap, class_id, shape_id, count, count,
        ST_HEADER_IMMUTABLE, &result);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    heap_status = st_heap_object_view(context->heap, result, &view);
    if (heap_status != ST_HEAP_OK
            || view.class_descriptor->class_id != class_id
            || view.shape_descriptor->shape_id != shape_id
            || view.shape_descriptor->fixed_word_count != 1u
            || view.shape_descriptor->indexed_format != ST_INDEXED_UINT32
            || view.indexed_length != count || view.indexed_capacity != count
            || view.fixed_words == NULL || view.indexed_elements == NULL)
        abort();

    metadata = ST_LARGE_INTEGER_METADATA_MAGIC
        | (negative ? ST_LARGE_INTEGER_NEGATIVE : 0u);
    memcpy(view.fixed_words, &metadata, sizeof(metadata));
    memcpy(view.indexed_elements, limbs, count * sizeof(*limbs));
    *result_out = result;
    return ST_INTEGER_PRIMITIVE_OK;
}

st_integer_primitive_status_t st_integer_from_sign_magnitude(
    st_numeric_context_t *context, bool negative, const uint32_t *limbs,
    size_t limb_count, st_value_t *result_out)
{
    return publish_magnitude(
        context, negative, limbs, limb_count, result_out);
}

static st_integer_primitive_status_t decode_operand(
    st_numeric_context_t *context, st_value_t value,
    integer_operand_t *operand)
{
    st_object_view_t object;
    st_heap_status_t heap_status;
    uint64_t metadata;
    uint64_t magnitude;
    int64_t small;
    uint64_t header;
    bool negative;

    if (!context_is_live(context) || operand == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    memset(operand, 0, sizeof(*operand));
    if (!st_value_has_valid_encoding(value))
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_VALUE;
    if (st_value_to_small_integer(value, &small)) {
        negative = small < 0;
        magnitude = negative
            ? (uint64_t)(-(small + INT64_C(1))) + UINT64_C(1)
            : (uint64_t)small;
        operand->negative = negative;
        operand->storage[0] = (uint32_t)magnitude;
        operand->storage[1] = (uint32_t)(magnitude >> 32u);
        operand->count = operand->storage[1] == 0u
            ? (operand->storage[0] == 0u ? 0u : 1u)
            : 2u;
        operand->limbs = operand->storage;
        return ST_INTEGER_PRIMITIVE_OK;
    }
    if (st_value_kind(value) != ST_VALUE_OBJECT)
        return ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH;

    heap_status = st_heap_object_view(context->heap, value, &object);
    if (heap_status != ST_HEAP_OK) return map_heap_status(heap_status);
    if (object.class_descriptor->class_id
            == context->large_positive_class_id
            && object.shape_descriptor->shape_id
                == context->large_positive_shape_id) {
        negative = false;
    } else if (object.class_descriptor->class_id
                   == context->large_negative_class_id
            && object.shape_descriptor->shape_id
                == context->large_negative_shape_id) {
        negative = true;
    } else {
        return ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH;
    }

    if (object.shape_descriptor->fixed_word_count != 1u
            || object.shape_descriptor->indexed_format != ST_INDEXED_UINT32
            || object.fixed_words == NULL || object.indexed_elements == NULL
            || object.indexed_length == 0u
            || object.indexed_capacity != object.indexed_length)
        return ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT;
    header = st_object_header_load(&object.object->header);
    if ((st_object_header_flags(header) & ST_HEADER_IMMUTABLE) == 0u)
        return ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT;
    memcpy(&metadata, object.fixed_words, sizeof(metadata));
    if ((metadata & ST_LARGE_INTEGER_METADATA_MASK)
                != ST_LARGE_INTEGER_METADATA_MAGIC
            || ((metadata & ST_LARGE_INTEGER_NEGATIVE) != 0u) != negative)
        return ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT;

    operand->negative = negative;
    operand->limbs = object.indexed_elements;
    operand->count = object.indexed_length;
    if (operand->limbs[operand->count - 1u] == 0u
            || magnitude_fits_small(
                operand->negative, operand->limbs, operand->count, NULL))
        return ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL;
    return ST_INTEGER_PRIMITIVE_OK;
}

st_integer_primitive_status_t st_integer_view(
    st_numeric_context_t *context, st_value_t value,
    uint32_t small_limb_storage[2], st_integer_view_t *view_out)
{
    integer_operand_t operand;
    st_integer_primitive_status_t status;

    if (small_limb_storage == NULL || view_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    memset(view_out, 0, sizeof(*view_out));
    status = decode_operand(context, value, &operand);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    if (operand.limbs == operand.storage) {
        small_limb_storage[0] = operand.storage[0];
        small_limb_storage[1] = operand.storage[1];
        view_out->limbs = small_limb_storage;
        view_out->is_small_integer = true;
    } else {
        view_out->limbs = operand.limbs;
    }
    view_out->negative = operand.negative;
    view_out->limb_count = operand.count;
    return ST_INTEGER_PRIMITIVE_OK;
}

static uint32_t *scratch_allocate(st_numeric_context_t *context,
                                  size_t count)
{
    size_t bytes;
    uint32_t *result;

    if (count == 0u) return NULL;
    if (count > SIZE_MAX / sizeof(*result)) return NULL;
    bytes = count * sizeof(*result);
    result = context->scratch_allocator.allocate(
        context->scratch_allocator.user, bytes);
    if (result != NULL) memset(result, 0, bytes);
    return result;
}

static void scratch_release(st_numeric_context_t *context, uint32_t *limbs)
{
    if (limbs != NULL)
        context->scratch_allocator.deallocate(
            context->scratch_allocator.user, limbs);
}

static int compare_magnitudes(const uint32_t *left, size_t left_count,
                              const uint32_t *right, size_t right_count)
{
    left_count = normalized_count(left, left_count);
    right_count = normalized_count(right, right_count);
    if (left_count != right_count)
        return left_count < right_count ? -1 : 1;
    while (left_count != 0u) {
        size_t index = --left_count;
        if (left[index] != right[index])
            return left[index] < right[index] ? -1 : 1;
    }
    return 0;
}

static size_t add_magnitudes(const uint32_t *left, size_t left_count,
                             const uint32_t *right, size_t right_count,
                             uint32_t *result)
{
    size_t count = left_count > right_count ? left_count : right_count;
    uint64_t carry = 0u;

    for (size_t index = 0u; index < count; index++) {
        uint64_t sum = carry;
        if (index < left_count) sum += left[index];
        if (index < right_count) sum += right[index];
        result[index] = (uint32_t)(sum & ST_LARGE_INTEGER_LIMB_MASK);
        carry = sum >> ST_LARGE_INTEGER_LIMB_BITS;
    }
    if (carry != 0u) result[count++] = (uint32_t)carry;
    return count;
}

/* Precondition: left >= right. */
static size_t subtract_magnitudes(
    const uint32_t *left, size_t left_count,
    const uint32_t *right, size_t right_count, uint32_t *result)
{
    uint64_t borrow = 0u;

    for (size_t index = 0u; index < left_count; index++) {
        uint64_t minuend = left[index];
        uint64_t subtrahend = borrow;
        if (index < right_count) subtrahend += right[index];
        result[index] = (uint32_t)(minuend - subtrahend);
        borrow = minuend < subtrahend;
    }
    if (borrow != 0u) abort();
    return normalized_count(result, left_count);
}

static st_integer_primitive_status_t signed_add(
    st_numeric_context_t *context, const integer_operand_t *left,
    const integer_operand_t *right, bool subtract_right,
    st_value_t *result_out)
{
    bool right_negative = right->negative != subtract_right;
    bool result_negative;
    size_t capacity;
    size_t count;
    uint32_t *result;
    int comparison;
    st_integer_primitive_status_t status;

    if (left->count > SIZE_MAX - 1u || right->count > SIZE_MAX - 1u)
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    capacity = (left->count > right->count ? left->count : right->count) + 1u;
    if (capacity == 1u) capacity = 2u;
    result = scratch_allocate(context, capacity);
    if (result == NULL) return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;

    if (left->negative == right_negative) {
        count = add_magnitudes(
            left->limbs, left->count, right->limbs, right->count, result);
        result_negative = left->negative;
    } else {
        comparison = compare_magnitudes(
            left->limbs, left->count, right->limbs, right->count);
        if (comparison >= 0) {
            count = subtract_magnitudes(
                left->limbs, left->count,
                right->limbs, right->count, result);
            result_negative = left->negative;
        } else {
            count = subtract_magnitudes(
                right->limbs, right->count,
                left->limbs, left->count, result);
            result_negative = right_negative;
        }
    }
    status = publish_magnitude(
        context, result_negative, result, count, result_out);
    scratch_release(context, result);
    return status;
}

static st_integer_primitive_status_t signed_multiply(
    st_numeric_context_t *context, const integer_operand_t *left,
    const integer_operand_t *right, st_value_t *result_out)
{
    size_t count;
    uint32_t *result;
    st_integer_primitive_status_t status;

    if (left->count == 0u || right->count == 0u)
        return publish_magnitude(context, false, NULL, 0u, result_out);
    if (left->count > SIZE_MAX - right->count)
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    count = left->count + right->count;
    result = scratch_allocate(context, count);
    if (result == NULL) return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;

    for (size_t left_index = 0u;
         left_index < left->count; left_index++) {
        uint64_t carry = 0u;
        for (size_t right_index = 0u;
             right_index < right->count; right_index++) {
            size_t output_index = left_index + right_index;
            uint64_t product = (uint64_t)left->limbs[left_index]
                * right->limbs[right_index];
            uint64_t sum = product + result[output_index] + carry;
            result[output_index] =
                (uint32_t)(sum & ST_LARGE_INTEGER_LIMB_MASK);
            carry = sum >> ST_LARGE_INTEGER_LIMB_BITS;
        }
        result[left_index + right->count] = (uint32_t)carry;
    }
    count = normalized_count(result, count);
    status = publish_magnitude(
        context, left->negative != right->negative,
        result, count, result_out);
    scratch_release(context, result);
    return status;
}

static unsigned leading_zero_count32(uint32_t value)
{
    unsigned count = 0u;
    if (value == 0u) return 32u;
    if ((value & UINT32_C(0xffff0000)) == 0u) {
        count += 16u;
        value <<= 16u;
    }
    if ((value & UINT32_C(0xff000000)) == 0u) {
        count += 8u;
        value <<= 8u;
    }
    if ((value & UINT32_C(0xf0000000)) == 0u) {
        count += 4u;
        value <<= 4u;
    }
    if ((value & UINT32_C(0xc0000000)) == 0u) {
        count += 2u;
        value <<= 2u;
    }
    if ((value & UINT32_C(0x80000000)) == 0u) count++;
    return count;
}

static size_t magnitude_bit_length(const uint32_t *limbs, size_t count)
{
    uint32_t high;
    count = normalized_count(limbs, count);
    if (count == 0u) return 0u;
    high = limbs[count - 1u];
    if (count - 1u > (SIZE_MAX - 32u) / 32u) return SIZE_MAX;
    return (count - 1u) * 32u + (32u - leading_zero_count32(high));
}

static uint32_t shifted_limb(const uint32_t *limbs, size_t count,
                             size_t shift, size_t output_index)
{
    size_t word_shift = shift / 32u;
    unsigned bit_shift = (unsigned)(shift % 32u);
    size_t source_index;
    uint64_t value = 0u;

    if (output_index < word_shift) return 0u;
    source_index = output_index - word_shift;
    if (source_index < count)
        value = (uint64_t)limbs[source_index] << bit_shift;
    if (bit_shift != 0u && source_index != 0u
            && source_index - 1u < count)
        value |= (uint64_t)limbs[source_index - 1u]
            >> (32u - bit_shift);
    return (uint32_t)value;
}

static int compare_shifted(
    const uint32_t *left, size_t left_count,
    const uint32_t *right, size_t right_count, size_t shift)
{
    size_t shifted_count;
    size_t word_shift = shift / 32u;
    unsigned bit_shift = (unsigned)(shift % 32u);

    left_count = normalized_count(left, left_count);
    right_count = normalized_count(right, right_count);
    if (right_count > SIZE_MAX - word_shift) return -1;
    shifted_count = right_count + word_shift;
    if (bit_shift != 0u
            && (right[right_count - 1u] >> (32u - bit_shift)) != 0u)
        shifted_count++;
    if (left_count != shifted_count)
        return left_count < shifted_count ? -1 : 1;
    while (left_count != 0u) {
        size_t index = --left_count;
        uint32_t shifted = shifted_limb(
            right, right_count, shift, index);
        if (left[index] != shifted)
            return left[index] < shifted ? -1 : 1;
    }
    return 0;
}

/* Precondition: left >= right << shift. */
static void subtract_shifted(
    uint32_t *left, size_t left_count,
    const uint32_t *right, size_t right_count, size_t shift)
{
    uint64_t borrow = 0u;

    for (size_t index = 0u; index < left_count; index++) {
        uint64_t minuend = left[index];
        uint64_t subtrahend = shifted_limb(
            right, right_count, shift, index);
        subtrahend += borrow;
        left[index] = (uint32_t)(minuend - subtrahend);
        borrow = minuend < subtrahend;
    }
    if (borrow != 0u) abort();
}

static st_integer_primitive_status_t divide_magnitudes(
    st_numeric_context_t *context, const uint32_t *dividend,
    size_t dividend_count, const uint32_t *divisor, size_t divisor_count,
    uint32_t **quotient_out, size_t *quotient_count_out,
    uint32_t **remainder_out, size_t *remainder_count_out)
{
    uint32_t *quotient = NULL;
    uint32_t *remainder = NULL;
    size_t dividend_bits;
    size_t divisor_bits;
    size_t shift;

    *quotient_out = NULL;
    *quotient_count_out = 0u;
    *remainder_out = NULL;
    *remainder_count_out = 0u;
    dividend_count = normalized_count(dividend, dividend_count);
    divisor_count = normalized_count(divisor, divisor_count);
    if (divisor_count == 0u)
        return ST_INTEGER_PRIMITIVE_ERR_DIVISION_BY_ZERO;

    /* The overwhelmingly common LargeInteger divisor fits one base-2^32
     * limb. Handle it in one linear high-to-low pass rather than paying for
     * one shifted comparison/subtraction per dividend bit. */
    if (divisor_count == 1u) {
        uint64_t carry = 0u;
        if (dividend_count != 0u) {
            quotient = scratch_allocate(context, dividend_count);
            if (quotient == NULL)
                return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
        }
        for (size_t index = dividend_count; index != 0u; index--) {
            uint64_t partial = (carry << 32u) | dividend[index - 1u];
            quotient[index - 1u] = (uint32_t)(partial / divisor[0]);
            carry = partial % divisor[0];
        }
        if (carry != 0u) {
            remainder = scratch_allocate(context, 1u);
            if (remainder == NULL) {
                scratch_release(context, quotient);
                return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
            }
            remainder[0] = (uint32_t)carry;
            *remainder_count_out = 1u;
        }
        *quotient_count_out = normalized_count(
            quotient, dividend_count);
        *quotient_out = quotient;
        *remainder_out = remainder;
        return ST_INTEGER_PRIMITIVE_OK;
    }

    if (dividend_count != 0u) {
        remainder = scratch_allocate(context, dividend_count);
        if (remainder == NULL)
            return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
        memcpy(remainder, dividend,
               dividend_count * sizeof(*remainder));
    }
    if (compare_magnitudes(
            dividend, dividend_count, divisor, divisor_count) < 0) {
        *remainder_out = remainder;
        *remainder_count_out = dividend_count;
        return ST_INTEGER_PRIMITIVE_OK;
    }

    dividend_bits = magnitude_bit_length(dividend, dividend_count);
    divisor_bits = magnitude_bit_length(divisor, divisor_count);
    if (dividend_bits == SIZE_MAX || divisor_bits == SIZE_MAX) {
        scratch_release(context, remainder);
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    }
    shift = dividend_bits - divisor_bits;
    if (shift / 32u > SIZE_MAX - 1u) {
        scratch_release(context, remainder);
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    }
    size_t quotient_capacity = shift / 32u + 1u;
    quotient = scratch_allocate(context, quotient_capacity);
    if (quotient == NULL) {
        scratch_release(context, remainder);
        return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }

    for (;;) {
        if (compare_shifted(
                remainder, dividend_count,
                divisor, divisor_count, shift) >= 0) {
            subtract_shifted(
                remainder, dividend_count,
                divisor, divisor_count, shift);
            quotient[shift / 32u] |= UINT32_C(1) << (shift % 32u);
        }
        if (shift == 0u) break;
        shift--;
    }

    *quotient_count_out = normalized_count(quotient, quotient_capacity);
    *remainder_count_out = normalized_count(remainder, dividend_count);
    *quotient_out = quotient;
    *remainder_out = remainder;
    return ST_INTEGER_PRIMITIVE_OK;
}

static st_integer_primitive_status_t increment_magnitude(
    st_numeric_context_t *context, uint32_t **limbs_in_out,
    size_t *count_in_out)
{
    uint32_t *limbs = *limbs_in_out;
    size_t count = *count_in_out;
    uint64_t carry = 1u;

    if (limbs == NULL) {
        if (count != 0u) abort();
        limbs = scratch_allocate(context, 1u);
        if (limbs == NULL)
            return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
        limbs[0] = 1u;
        *limbs_in_out = limbs;
        *count_in_out = 1u;
        return ST_INTEGER_PRIMITIVE_OK;
    }

    for (size_t index = 0u; index < count && carry != 0u; index++) {
        uint64_t sum = (uint64_t)limbs[index] + carry;
        limbs[index] = (uint32_t)sum;
        carry = sum >> 32u;
    }
    if (carry == 0u) return ST_INTEGER_PRIMITIVE_OK;
    if (count == SIZE_MAX)
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    uint32_t *grown = scratch_allocate(context, count + 1u);
    if (grown == NULL) return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
    if (count != 0u) memcpy(grown, limbs, count * sizeof(*grown));
    grown[count++] = 1u;
    scratch_release(context, limbs);
    *limbs_in_out = grown;
    *count_in_out = count;
    return ST_INTEGER_PRIMITIVE_OK;
}

static st_integer_primitive_status_t signed_divide(
    st_numeric_context_t *context, const integer_operand_t *left,
    const integer_operand_t *right, bool modulo, st_value_t *result_out)
{
    uint32_t *quotient;
    uint32_t *remainder;
    size_t quotient_count;
    size_t remainder_count;
    bool signs_differ;
    st_integer_primitive_status_t status;

    status = divide_magnitudes(
        context, left->limbs, left->count, right->limbs, right->count,
        &quotient, &quotient_count, &remainder, &remainder_count);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    signs_differ = left->negative != right->negative;
    if (!modulo) {
        if (signs_differ && remainder_count != 0u) {
            status = increment_magnitude(
                context, &quotient, &quotient_count);
            if (status != ST_INTEGER_PRIMITIVE_OK) goto done;
        }
        status = publish_magnitude(
            context, signs_differ && quotient_count != 0u,
            quotient, quotient_count, result_out);
    } else if (remainder_count == 0u || !signs_differ) {
        status = publish_magnitude(
            context, left->negative && remainder_count != 0u,
            remainder, remainder_count, result_out);
    } else {
        uint32_t *adjusted = scratch_allocate(context, right->count);
        size_t adjusted_count;
        if (adjusted == NULL) {
            status = ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
            goto done;
        }
        adjusted_count = subtract_magnitudes(
            right->limbs, right->count,
            remainder, remainder_count, adjusted);
        status = publish_magnitude(
            context, right->negative, adjusted, adjusted_count, result_out);
        scratch_release(context, adjusted);
    }

done:
    scratch_release(context, remainder);
    scratch_release(context, quotient);
    return status;
}

st_integer_primitive_status_t st_integer_binary(
    st_numeric_context_t *context, st_value_t receiver,
    st_integer_binary_operation_t operation, st_value_t argument,
    st_value_t *result_out)
{
    integer_operand_t left;
    integer_operand_t right;
    st_integer_primitive_status_t status;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = decode_operand(context, receiver, &left);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;
    status = decode_operand(context, argument, &right);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    switch (operation) {
    case ST_INTEGER_BINARY_ADD:
        return signed_add(context, &left, &right, false, result_out);
    case ST_INTEGER_BINARY_SUBTRACT:
        return signed_add(context, &left, &right, true, result_out);
    case ST_INTEGER_BINARY_MULTIPLY:
        return signed_multiply(context, &left, &right, result_out);
    case ST_INTEGER_BINARY_FLOOR_DIVIDE:
        return signed_divide(context, &left, &right, false, result_out);
    case ST_INTEGER_BINARY_MODULO:
        return signed_divide(context, &left, &right, true, result_out);
    default:
        return ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION;
    }
}

st_integer_primitive_status_t st_integer_compare(
    st_numeric_context_t *context, st_value_t receiver, st_value_t argument,
    st_value_t *result_out)
{
    integer_operand_t left;
    integer_operand_t right;
    st_integer_primitive_status_t status;
    int comparison;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = decode_operand(context, receiver, &left);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;
    status = decode_operand(context, argument, &right);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    if (left.negative != right.negative) {
        comparison = left.negative ? -1 : 1;
    } else {
        comparison = compare_magnitudes(
            left.limbs, left.count, right.limbs, right.count);
        if (left.negative) comparison = -comparison;
    }
    if (!st_value_from_small_integer(comparison, result_out)) abort();
    return ST_INTEGER_PRIMITIVE_OK;
}

static bool magnitude_to_size(const integer_operand_t *operand,
                              size_t *value_out)
{
    size_t value = 0u;
    const uint64_t radix = UINT64_C(1) << 32u;

    if (operand->negative) return false;
    if (operand->count > (sizeof(size_t) + sizeof(uint32_t) - 1u)
            / sizeof(uint32_t))
        return false;
    for (size_t index = operand->count; index != 0u; index--) {
        uint32_t limb = operand->limbs[index - 1u];
        if ((uint64_t)value > (uint64_t)SIZE_MAX / radix)
            return false;
        value = (size_t)((uint64_t)value * radix);
        if ((size_t)limb > SIZE_MAX - value) return false;
        value += limb;
    }
    *value_out = value;
    return true;
}

static bool discarded_bits_are_nonzero(
    const uint32_t *limbs, size_t count, size_t bit_count)
{
    size_t whole_limbs = bit_count / 32u;
    unsigned partial_bits = (unsigned)(bit_count % 32u);

    if (whole_limbs > count) whole_limbs = count;
    for (size_t index = 0u; index < whole_limbs; index++)
        if (limbs[index] != 0u) return true;
    if (partial_bits != 0u && whole_limbs < count) {
        uint32_t mask = (UINT32_C(1) << partial_bits) - UINT32_C(1);
        if ((limbs[whole_limbs] & mask) != 0u) return true;
    }
    return false;
}

static st_integer_primitive_status_t shift_left(
    st_numeric_context_t *context, const integer_operand_t *operand,
    size_t count, st_value_t *result_out)
{
    size_t word_shift;
    unsigned bit_shift;
    size_t capacity;
    uint32_t *result;
    uint64_t carry = 0u;
    st_integer_primitive_status_t status;

    if (operand->count == 0u)
        return publish_magnitude(context, false, NULL, 0u, result_out);
    word_shift = count / 32u;
    bit_shift = (unsigned)(count % 32u);
    if (operand->count > SIZE_MAX - word_shift - 1u)
        return ST_INTEGER_PRIMITIVE_ERR_OVERFLOW;
    capacity = operand->count + word_shift + 1u;
    result = scratch_allocate(context, capacity);
    if (result == NULL) return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;

    for (size_t index = 0u; index < operand->count; index++) {
        uint64_t shifted = ((uint64_t)operand->limbs[index] << bit_shift)
            | carry;
        result[index + word_shift] = (uint32_t)shifted;
        carry = shifted >> 32u;
    }
    result[operand->count + word_shift] = (uint32_t)carry;
    status = publish_magnitude(
        context, operand->negative, result, capacity, result_out);
    scratch_release(context, result);
    return status;
}

static st_integer_primitive_status_t shift_right(
    st_numeric_context_t *context, const integer_operand_t *operand,
    size_t count, bool count_is_huge, st_value_t *result_out)
{
    size_t bit_length = magnitude_bit_length(operand->limbs, operand->count);
    size_t word_shift;
    unsigned bit_shift;
    size_t output_count;
    uint32_t *result;
    st_integer_primitive_status_t status;
    bool round_negative;

    if (operand->count == 0u)
        return publish_magnitude(context, false, NULL, 0u, result_out);
    if (count_is_huge || count >= bit_length) {
        uint32_t one = 1u;
        return publish_magnitude(
            context, operand->negative, operand->negative ? &one : NULL,
            operand->negative ? 1u : 0u, result_out);
    }

    word_shift = count / 32u;
    bit_shift = (unsigned)(count % 32u);
    output_count = operand->count - word_shift;
    result = scratch_allocate(context, output_count + 1u);
    if (result == NULL) return ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY;
    for (size_t output_index = 0u;
         output_index < output_count; output_index++) {
        size_t source_index = output_index + word_shift;
        uint64_t value = operand->limbs[source_index];
        if (bit_shift != 0u && source_index + 1u < operand->count)
            value |= (uint64_t)operand->limbs[source_index + 1u] << 32u;
        result[output_index] = (uint32_t)(value >> bit_shift);
    }
    output_count = normalized_count(result, output_count);
    round_negative = operand->negative
        && discarded_bits_are_nonzero(operand->limbs, operand->count, count);
    if (round_negative) {
        status = increment_magnitude(
            context, &result, &output_count);
        if (status != ST_INTEGER_PRIMITIVE_OK) {
            scratch_release(context, result);
            return status;
        }
    }
    status = publish_magnitude(
        context, operand->negative, result, output_count, result_out);
    scratch_release(context, result);
    return status;
}

st_integer_primitive_status_t st_integer_shift(
    st_numeric_context_t *context, st_value_t receiver, st_value_t count,
    st_value_t *result_out)
{
    integer_operand_t operand;
    integer_operand_t shift_count;
    st_integer_primitive_status_t status;
    size_t count_value = 0u;
    bool count_fits;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = decode_operand(context, receiver, &operand);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;
    status = decode_operand(context, count, &shift_count);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    if (shift_count.count == 0u)
        return publish_magnitude(
            context, operand.negative, operand.limbs, operand.count,
            result_out);
    if (shift_count.negative) {
        integer_operand_t absolute_count = shift_count;
        absolute_count.negative = false;
        count_fits = magnitude_to_size(&absolute_count, &count_value);
        return shift_right(
            context, &operand, count_value, !count_fits, result_out);
    }
    count_fits = magnitude_to_size(&shift_count, &count_value);
    if (!count_fits) {
        if (operand.count == 0u)
            return publish_magnitude(context, false, NULL, 0u, result_out);
        return ST_INTEGER_PRIMITIVE_ERR_SHIFT_OUT_OF_RANGE;
    }
    return shift_left(context, &operand, count_value, result_out);
}

static uint64_t magnitude_bits_at(
    const uint32_t *limbs, size_t count, size_t low_bit, unsigned width)
{
    size_t word = low_bit / 32u;
    unsigned offset = (unsigned)(low_bit % 32u);
    unsigned output_shift = 0u;
    uint64_t result = 0u;

    while (width != 0u && word < count) {
        unsigned available = 32u - offset;
        unsigned take = width < available ? width : available;
        uint32_t part = limbs[word] >> offset;
        if (take < 32u)
            part &= (UINT32_C(1) << take) - UINT32_C(1);
        result |= (uint64_t)part << output_shift;
        output_shift += take;
        width -= take;
        word++;
        offset = 0u;
    }
    return result;
}

static uint64_t integer_binary64_bits(const integer_operand_t *operand)
{
    size_t bit_length = magnitude_bit_length(operand->limbs, operand->count);
    uint64_t sign = operand->negative ? UINT64_C(0x8000000000000000) : 0u;
    uint64_t significand;
    uint64_t exponent;

    if (bit_length == 0u) return sign;
    exponent = bit_length - 1u;
    if (bit_length <= 53u) {
        significand = magnitude_bits_at(
            operand->limbs, operand->count, 0u, (unsigned)bit_length);
        significand <<= 53u - (unsigned)bit_length;
    } else {
        size_t discarded = bit_length - 53u;
        bool guard = magnitude_bits_at(
            operand->limbs, operand->count, discarded - 1u, 1u) != 0u;
        bool sticky = discarded > 1u
            && discarded_bits_are_nonzero(
                operand->limbs, operand->count, discarded - 1u);
        significand = magnitude_bits_at(
            operand->limbs, operand->count, discarded, 53u);
        if (guard && (sticky || (significand & UINT64_C(1)) != 0u)) {
            significand++;
            if (significand == (UINT64_C(1) << 53u)) {
                significand >>= 1u;
                exponent++;
            }
        }
    }
    if (exponent > 1023u)
        return sign | UINT64_C(0x7ff0000000000000);
    return sign | ((exponent + UINT64_C(1023)) << 52u)
        | (significand & UINT64_C(0x000fffffffffffff));
}

st_integer_primitive_status_t st_integer_as_float(
    st_numeric_context_t *context, st_value_t receiver,
    st_value_t *result_out)
{
    integer_operand_t operand;
    st_integer_primitive_status_t status;
    st_float_primitive_status_t float_status;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = decode_operand(context, receiver, &operand);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;
    float_status = st_float_primitive_box_bits(
        context->float_primitives, integer_binary64_bits(&operand),
        result_out);
    return float_status == ST_FLOAT_PRIMITIVE_OK
        ? ST_INTEGER_PRIMITIVE_OK : ST_INTEGER_PRIMITIVE_ERR_FLOAT;
}

static uint64_t hash_byte(uint64_t hash, uint8_t byte)
{
    return (hash ^ byte) * UINT64_C(0x100000001b3);
}

st_integer_primitive_status_t st_integer_hash(
    st_numeric_context_t *context, st_value_t receiver,
    st_value_t *result_out)
{
    integer_operand_t operand;
    st_integer_primitive_status_t status;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = decode_operand(context, receiver, &operand);
    if (status != ST_INTEGER_PRIMITIVE_OK) return status;

    hash = hash_byte(hash, operand.negative ? UINT8_C(1) : UINT8_C(0));
    for (size_t index = 0u; index < operand.count; index++) {
        uint32_t limb = operand.limbs[index];
        hash = hash_byte(hash, (uint8_t)limb);
        hash = hash_byte(hash, (uint8_t)(limb >> 8u));
        hash = hash_byte(hash, (uint8_t)(limb >> 16u));
        hash = hash_byte(hash, (uint8_t)(limb >> 24u));
    }
    hash ^= (uint64_t)operand.count * UINT64_C(0x9e3779b185ebca87);
    hash = (hash ^ (hash >> 30u)) * UINT64_C(0xbf58476d1ce4e5b9);
    hash = (hash ^ (hash >> 27u)) * UINT64_C(0x94d049bb133111eb);
    hash = (hash ^ (hash >> 31u)) & (uint64_t)ST_SMALL_INTEGER_MAX;
    if (!st_value_from_small_integer((int64_t)hash, result_out)) abort();
    return ST_INTEGER_PRIMITIVE_OK;
}

st_integer_primitive_status_t st_integer_from_binary64_bits(
    st_numeric_context_t *context, uint64_t bits,
    st_integer_rounding_t rounding, st_value_t *result_out)
{
    uint64_t exponent_field = (bits >> 52u) & UINT64_C(0x7ff);
    uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
    uint64_t significand;
    uint64_t quotient;
    uint64_t remainder;
    uint32_t limbs[2];
    int binary_shift;
    unsigned right_shift;
    bool negative = (bits >> 63u) != 0u;
    bool increment = false;

    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (!context_is_live(context) || result_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (rounding < ST_INTEGER_ROUND_TOWARD_ZERO
            || rounding > ST_INTEGER_ROUND_NEAREST_TIES_AWAY)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    if (exponent_field == UINT64_C(0x7ff))
        return ST_INTEGER_PRIMITIVE_ERR_NON_FINITE;

    if (exponent_field == 0u) {
        significand = fraction;
        binary_shift = -1074;
    } else {
        significand = (UINT64_C(1) << 52u) | fraction;
        binary_shift = (int)exponent_field - 1023 - 52;
    }
    if (significand == 0u)
        return publish_magnitude(context, false, NULL, 0u, result_out);

    if (binary_shift >= 0) {
        integer_operand_t operand;

        memset(&operand, 0, sizeof(operand));
        operand.negative = negative;
        operand.storage[0] = (uint32_t)significand;
        operand.storage[1] = (uint32_t)(significand >> 32u);
        operand.count = operand.storage[1] == 0u ? 1u : 2u;
        operand.limbs = operand.storage;
        return shift_left(
            context, &operand, (size_t)binary_shift, result_out);
    }

    right_shift = (unsigned)-binary_shift;
    if (right_shift >= 64u) {
        quotient = 0u;
        remainder = significand;
    } else {
        uint64_t remainder_mask = (UINT64_C(1) << right_shift) - 1u;
        quotient = significand >> right_shift;
        remainder = significand & remainder_mask;
    }

    if (remainder != 0u) {
        switch (rounding) {
        case ST_INTEGER_ROUND_TOWARD_ZERO:
            break;
        case ST_INTEGER_ROUND_FLOOR:
            increment = negative;
            break;
        case ST_INTEGER_ROUND_CEILING:
            increment = !negative;
            break;
        case ST_INTEGER_ROUND_NEAREST_TIES_AWAY:
            increment = right_shift <= 53u
                && remainder >= (UINT64_C(1) << (right_shift - 1u));
            break;
        default:
            abort();
        }
    }
    if (increment) quotient++;
    limbs[0] = (uint32_t)quotient;
    limbs[1] = (uint32_t)(quotient >> 32u);
    return publish_magnitude(context, negative, limbs, 2u, result_out);
}

const st_primitive_spec_t *st_integer_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(integer_specs) / sizeof(integer_specs[0]);
    return integer_specs;
}

const char *st_integer_primitive_status_string(
    st_integer_primitive_status_t status)
{
    switch (status) {
    case ST_INTEGER_PRIMITIVE_OK: return "ok";
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_STATE:
        return "invalid numeric context";
    case ST_INTEGER_PRIMITIVE_ERR_WRONG_ARITY: return "wrong arity";
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_VALUE: return "invalid StValue";
    case ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH: return "not an Integer";
    case ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER:
        return "not a live heap member";
    case ST_INTEGER_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return "dangling heap reference";
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return "invalid numeric descriptor";
    case ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT:
        return "malformed LargeInteger";
    case ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL:
        return "non-canonical LargeInteger";
    case ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION:
        return "unknown integer operation";
    case ST_INTEGER_PRIMITIVE_ERR_DIVISION_BY_ZERO:
        return "division by zero";
    case ST_INTEGER_PRIMITIVE_ERR_SHIFT_OUT_OF_RANGE:
        return "left shift count cannot be materialized";
    case ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_INTEGER_PRIMITIVE_ERR_OVERFLOW: return "size overflow";
    case ST_INTEGER_PRIMITIVE_ERR_NON_FINITE:
        return "non-finite binary64 value";
    case ST_INTEGER_PRIMITIVE_ERR_FLOAT:
        return "binary64 boxing failed";
    default: return "unknown integer primitive status";
    }
}
