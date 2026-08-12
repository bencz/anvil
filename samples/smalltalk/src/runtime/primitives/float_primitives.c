#include "st_float_primitives.h"
#include "float_primitives_internal.h"

#include <float.h>
#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Floating-environment access is required by evaluate_arithmetic().  Clang
 * implements the standard pragma, while GCC 15 still diagnoses that pragma
 * as unknown under -Werror and exposes the equivalent compiler control via
 * its optimize pragma.  Unknown compilers must compile this translation unit
 * in their documented strict-fenv/rounding-math mode, as stated by the public
 * contract. */
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(__GNUC__)
#pragma GCC optimize ("rounding-math")
#endif

#define ST_FLOAT_PRIMITIVE_MAGIC UINT64_C(0x5354464c4f415431)
#define BINARY64_SIGN_MASK UINT64_C(0x8000000000000000)
#define BINARY64_EXPONENT_MASK UINT64_C(0x7ff0000000000000)
#define BINARY64_FRACTION_MASK UINT64_C(0x000fffffffffffff)
#define BINARY64_QUIET_BIT UINT64_C(0x0008000000000000)
#define BINARY64_CANONICAL_NAN UINT64_C(0x7ff8000000000000)

_Static_assert(sizeof(double) == sizeof(uint64_t),
               "BoxedFloat64 requires a 64-bit C double");
_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 &&
               DBL_MIN_EXP == -1021,
               "BoxedFloat64 requires IEC 60559 binary64");

#define FLOAT_PRIMITIVE_LIST(X)                                             \
    X("FloatEqualsPrimitive", 1u, ST_PRIMITIVE_CANNOT_FAIL,                \
      ST_FLOAT_OPERATION_EQUALS, primitive_float_equals,                    \
      "st_aot_float_equals_primitive_execute")                             \
    X("FloatLessThanPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,             \
      ST_FLOAT_OPERATION_LESS_THAN, primitive_float_less,                   \
      "st_aot_float_less_than_primitive_execute")                          \
    X("FloatGreaterThanPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,          \
      ST_FLOAT_OPERATION_GREATER_THAN, primitive_float_greater,             \
      "st_aot_float_greater_than_primitive_execute")                       \
    X("FloatLessEqualsPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,           \
      ST_FLOAT_OPERATION_LESS_EQUALS, primitive_float_less_equal,           \
      "st_aot_float_less_equals_primitive_execute")                        \
    X("FloatGreaterEqualsPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,        \
      ST_FLOAT_OPERATION_GREATER_EQUALS, primitive_float_greater_equal,     \
      "st_aot_float_greater_equals_primitive_execute")                     \
    X("FloatAddPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,                  \
      ST_FLOAT_OPERATION_ADD, primitive_float_add,                          \
      "st_aot_float_add_primitive_execute")                                \
    X("FloatSubPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,                  \
      ST_FLOAT_OPERATION_SUBTRACT, primitive_float_subtract,                \
      "st_aot_float_subtract_primitive_execute")                           \
    X("FloatMulPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,                  \
      ST_FLOAT_OPERATION_MULTIPLY, primitive_float_multiply,                \
      "st_aot_float_multiply_primitive_execute")                           \
    X("FloatDivPrimitive", 1u, ST_PRIMITIVE_FALL_THROUGH,                  \
      ST_FLOAT_OPERATION_DIVIDE, primitive_float_divide,                    \
      "st_aot_float_divide_primitive_execute")                             \
    X("FloatNegPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,                  \
      ST_FLOAT_OPERATION_NEGATE, primitive_float_negate,                    \
      "st_aot_float_negate_primitive_execute")                             \
    X("FloatTruncatedPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,            \
      ST_FLOAT_OPERATION_TRUNCATED, primitive_float_truncated,              \
      "st_aot_float_truncated_primitive_execute")                          \
    X("FloatFloorPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,                \
      ST_FLOAT_OPERATION_FLOOR, primitive_float_floor,                      \
      "st_aot_float_floor_primitive_execute")                              \
    X("FloatCeilingPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,              \
      ST_FLOAT_OPERATION_CEILING, primitive_float_ceiling,                  \
      "st_aot_float_ceiling_primitive_execute")                            \
    X("FloatRoundedPrimitive", 0u, ST_PRIMITIVE_FALL_THROUGH,              \
      ST_FLOAT_OPERATION_ROUNDED, primitive_float_rounded,                  \
      "st_aot_float_rounded_primitive_execute")                            \
    X("FloatHashPrimitive", 0u, ST_PRIMITIVE_CANNOT_FAIL,                  \
      ST_FLOAT_OPERATION_HASH, primitive_float_hash,                        \
      "st_aot_float_hash_primitive_execute")

struct st_float_primitive_state {
    uint64_t magic;
    st_heap_t *heap;
    uint32_t class_id;
    uint32_t shape_id;
    st_primitive_allocator_t allocator;
};

typedef st_float_primitive_status_t (*float_handler_t)(
    st_float_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out);

typedef struct {
    uint32_t id;
    uint32_t arity;
    float_handler_t handler;
} float_definition_t;

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
                    (input.deallocate == NULL))) {
        return false;
    }
    if (!input.allocate) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static st_float_primitive_state_t *context_state(
    const st_float_primitive_context_t *context)
{
    if (!context || !context->state ||
        context->state->magic != ST_FLOAT_PRIMITIVE_MAGIC) {
        return NULL;
    }
    return context->state;
}

static st_float_primitive_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK:
        return ST_FLOAT_PRIMITIVE_OK;
    case ST_HEAP_ERR_INVALID_ARGUMENT:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    case ST_HEAP_ERR_INVALID_DESCRIPTOR:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW:
        return ST_FLOAT_PRIMITIVE_ERR_OVERFLOW;
    case ST_HEAP_ERR_NOT_OBJECT:
        return ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_HEAP_ERR_NOT_MEMBER:
        return ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_HEAP_ERR_DANGLING_REFERENCE:
        return ST_FLOAT_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_HEAP_ERR_BAD_ALIGNMENT:
    case ST_HEAP_ERR_BAD_EXTENT:
    case ST_HEAP_ERR_BAD_OBJECT:
    case ST_HEAP_ERR_INVALID_ROOT:
    case ST_HEAP_ERR_INVALID_FRAME:
    case ST_HEAP_ERR_FRAME_CYCLE:
    case ST_HEAP_ERR_RECLAIM_PROTOCOL:
    case ST_HEAP_ERR_CONFLICT:
    default:
        return ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT;
    }
}

static bool binary64_is_nan(uint64_t bits)
{
    return (bits & BINARY64_EXPONENT_MASK) == BINARY64_EXPONENT_MASK &&
           (bits & BINARY64_FRACTION_MASK) != 0u;
}

static bool binary64_is_infinite(uint64_t bits)
{
    return (bits & ~BINARY64_SIGN_MASK) == BINARY64_EXPONENT_MASK;
}

static uint64_t binary64_quiet_nan(uint64_t bits)
{
    return bits | BINARY64_QUIET_BIT;
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

st_float_primitive_status_t st_float_primitive_context_init(
    st_float_primitive_context_t *context,
    const st_float_primitive_options_t *options)
{
    const st_runtime_descriptors_t *descriptors;
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    st_primitive_allocator_t allocator;
    st_float_primitive_state_t *state;
    if (!context || context->state || !options || !options->heap ||
        options->boxed_float64_class_id == 0u ||
        options->boxed_float64_shape_id == 0u ||
        !normalize_allocator(options->allocator, &allocator)) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    descriptors = st_heap_descriptors(options->heap);
    class_descriptor = st_runtime_class(
        descriptors, options->boxed_float64_class_id);
    shape = st_runtime_shape(descriptors, options->boxed_float64_shape_id);
    if (!descriptors || !class_descriptor || !shape ||
        !st_class_descriptor_is_valid(class_descriptor) ||
        !st_shape_descriptor_is_valid(shape) ||
        class_descriptor->class_id != options->boxed_float64_class_id ||
        class_descriptor->default_shape_id != options->boxed_float64_shape_id ||
        (class_descriptor->flags & (ST_CLASS_METACLASS | ST_CLASS_ABSTRACT)) != 0u ||
        shape->shape_id != options->boxed_float64_shape_id ||
        shape->class_id != options->boxed_float64_class_id ||
        shape->fixed_word_count != 1u ||
        shape->indexed_format != ST_INDEXED_NONE ||
        !shape->fixed_pointer_bitmap ||
        shape->fixed_pointer_bitmap_word_count != 1u ||
        (shape->fixed_pointer_bitmap[0] & UINT64_C(1)) != 0u) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    }
    state = allocator.allocate(allocator.user, sizeof(*state));
    if (!state) {
        return ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    *state = (st_float_primitive_state_t){
        .magic = ST_FLOAT_PRIMITIVE_MAGIC,
        .heap = options->heap,
        .class_id = options->boxed_float64_class_id,
        .shape_id = options->boxed_float64_shape_id,
        .allocator = allocator
    };
    context->state = state;
    return ST_FLOAT_PRIMITIVE_OK;
}

void st_float_primitive_context_destroy(st_float_primitive_context_t *context)
{
    st_float_primitive_state_t *state = context_state(context);
    st_primitive_allocator_t allocator;
    if (!state) {
        return;
    }
    allocator = state->allocator;
    state->magic = 0u;
    allocator.deallocate(allocator.user, state);
    context->state = NULL;
}

st_heap_t *st_float_primitive_context_heap(
    st_float_primitive_context_t *context)
{
    st_float_primitive_state_t *state = context_state(context);
    return state == NULL ? NULL : state->heap;
}

const st_heap_t *st_float_primitive_context_heap_const(
    const st_float_primitive_context_t *context)
{
    st_float_primitive_state_t *state = context_state(context);
    return state == NULL ? NULL : state->heap;
}

st_float_primitive_status_t st_float_primitive_box_bits(
    st_float_primitive_context_t *context, uint64_t ieee_binary64_bits,
    st_value_t *result_out)
{
    st_float_primitive_state_t *state = context_state(context);
    st_object_view_t view;
    st_value_t value = ST_VALUE_INVALID;
    st_heap_status_t heap_status;
    if (result_out) {
        *result_out = ST_VALUE_INVALID;
    }
    if (!state || !result_out) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    heap_status = st_heap_allocate(state->heap, state->class_id,
                                   state->shape_id, 0u, 0u,
                                   ST_HEADER_IMMUTABLE, &value);
    if (heap_status != ST_HEAP_OK) {
        return map_heap_status(heap_status);
    }
    heap_status = st_heap_object_view(state->heap, value, &view);
    if (heap_status != ST_HEAP_OK) {
        abort();
    }
    if (view.class_descriptor->class_id != state->class_id ||
        view.shape_descriptor->shape_id != state->shape_id ||
        view.shape_descriptor->fixed_word_count != 1u ||
        view.indexed_length != 0u || view.indexed_capacity != 0u ||
        !view.fixed_words) {
        abort();
    }
    memcpy(view.fixed_words, &ieee_binary64_bits, sizeof(ieee_binary64_bits));
    *result_out = value;
    return ST_FLOAT_PRIMITIVE_OK;
}

st_float_primitive_status_t st_float_primitive_unbox_bits(
    st_float_primitive_context_t *context, st_value_t value,
    uint64_t *ieee_binary64_bits_out)
{
    st_float_primitive_state_t *state = context_state(context);
    st_object_view_t view;
    st_heap_status_t heap_status;
    if (ieee_binary64_bits_out) {
        *ieee_binary64_bits_out = 0u;
    }
    if (!state || !ieee_binary64_bits_out) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    if (!st_value_has_valid_encoding(value)) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_VALUE;
    }
    if (st_value_kind(value) != ST_VALUE_OBJECT) {
        return ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH;
    }
    heap_status = st_heap_object_view(state->heap, value, &view);
    if (heap_status != ST_HEAP_OK) {
        return map_heap_status(heap_status);
    }
    if (view.class_descriptor->class_id != state->class_id ||
        view.shape_descriptor->shape_id != state->shape_id) {
        return ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH;
    }
    if (view.shape_descriptor->fixed_word_count != 1u ||
        view.shape_descriptor->indexed_format != ST_INDEXED_NONE ||
        view.indexed_length != 0u || view.indexed_capacity != 0u ||
        !view.fixed_words) {
        return ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT;
    }
    memcpy(ieee_binary64_bits_out, view.fixed_words,
           sizeof(*ieee_binary64_bits_out));
    return ST_FLOAT_PRIMITIVE_OK;
}

static st_float_primitive_status_t encode_boolean(bool boolean,
                                                   st_value_t *result_out)
{
    *result_out = boolean ? st_value_true() : st_value_false();
    return ST_FLOAT_PRIMITIVE_OK;
}

typedef enum {
    FLOAT_COMPARE_EQUAL,
    FLOAT_COMPARE_LESS,
    FLOAT_COMPARE_GREATER,
    FLOAT_COMPARE_LESS_EQUAL,
    FLOAT_COMPARE_GREATER_EQUAL
} float_compare_kind_t;

static st_float_primitive_status_t compare_float_values(
    st_float_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out,
    float_compare_kind_t operation)
{
    st_float_primitive_context_t context = { state };
    uint64_t left_bits;
    uint64_t right_bits;
    st_float_primitive_status_t status = st_float_primitive_unbox_bits(
        &context, receiver, &left_bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    status = st_float_primitive_unbox_bits(&context, arguments[0], &right_bits);
    if (status == ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH &&
        operation == FLOAT_COMPARE_EQUAL) {
        return encode_boolean(false, result_out);
    }
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    if (binary64_is_nan(left_bits) || binary64_is_nan(right_bits)) {
        return encode_boolean(false, result_out);
    }
    double left = double_from_bits(left_bits);
    double right = double_from_bits(right_bits);
    bool result;
    switch (operation) {
    case FLOAT_COMPARE_EQUAL:
        result = left == right;
        break;
    case FLOAT_COMPARE_LESS:
        result = left < right;
        break;
    case FLOAT_COMPARE_GREATER:
        result = left > right;
        break;
    case FLOAT_COMPARE_LESS_EQUAL:
        result = left <= right;
        break;
    case FLOAT_COMPARE_GREATER_EQUAL:
        result = left >= right;
        break;
    default:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    return encode_boolean(result, result_out);
}

#define DEFINE_COMPARE(name_, kind_)                                        \
    static st_float_primitive_status_t name_(                               \
        st_float_primitive_state_t *state, st_value_t receiver,             \
        const st_value_t *arguments, st_value_t *result_out)                \
    {                                                                       \
        return compare_float_values(state, receiver, arguments, result_out, \
                                    (kind_));                               \
    }

DEFINE_COMPARE(primitive_float_equals, FLOAT_COMPARE_EQUAL)
DEFINE_COMPARE(primitive_float_less, FLOAT_COMPARE_LESS)
DEFINE_COMPARE(primitive_float_greater, FLOAT_COMPARE_GREATER)
DEFINE_COMPARE(primitive_float_less_equal, FLOAT_COMPARE_LESS_EQUAL)
DEFINE_COMPARE(primitive_float_greater_equal, FLOAT_COMPARE_GREATER_EQUAL)

#undef DEFINE_COMPARE

typedef enum {
    FLOAT_ARITHMETIC_ADD,
    FLOAT_ARITHMETIC_SUBTRACT,
    FLOAT_ARITHMETIC_MULTIPLY,
    FLOAT_ARITHMETIC_DIVIDE
} float_arithmetic_kind_t;

static st_float_primitive_status_t evaluate_arithmetic(
    double left, double right, float_arithmetic_kind_t operation,
    double *result_out)
{
    fenv_t saved_environment;
    volatile double volatile_left = left;
    volatile double volatile_right = right;
    volatile double volatile_result;
    if (!result_out || feholdexcept(&saved_environment) != 0) {
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    if (fesetround(FE_TONEAREST) != 0) {
        (void)fesetenv(&saved_environment);
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    switch (operation) {
    case FLOAT_ARITHMETIC_ADD:
        volatile_result = volatile_left + volatile_right;
        break;
    case FLOAT_ARITHMETIC_SUBTRACT:
        volatile_result = volatile_left - volatile_right;
        break;
    case FLOAT_ARITHMETIC_MULTIPLY:
        volatile_result = volatile_left * volatile_right;
        break;
    case FLOAT_ARITHMETIC_DIVIDE:
        volatile_result = volatile_left / volatile_right;
        break;
    default:
        (void)fesetenv(&saved_environment);
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    *result_out = volatile_result;
    if (fesetenv(&saved_environment) != 0) {
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    return ST_FLOAT_PRIMITIVE_OK;
}

static st_float_primitive_status_t arithmetic_float_values(
    st_float_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out,
    float_arithmetic_kind_t operation)
{
    st_float_primitive_context_t context = { state };
    uint64_t left_bits;
    uint64_t right_bits;
    uint64_t result_bits;
    double result;
    st_float_primitive_status_t status = st_float_primitive_unbox_bits(
        &context, receiver, &left_bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    status = st_float_primitive_unbox_bits(&context, arguments[0], &right_bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    if (binary64_is_nan(left_bits)) {
        result_bits = binary64_quiet_nan(left_bits);
    } else if (binary64_is_nan(right_bits)) {
        result_bits = binary64_quiet_nan(right_bits);
    } else {
        status = evaluate_arithmetic(double_from_bits(left_bits),
                                     double_from_bits(right_bits), operation,
                                     &result);
        if (status != ST_FLOAT_PRIMITIVE_OK) {
            return status;
        }
        result_bits = double_bits(result);
        if (binary64_is_nan(result_bits)) {
            result_bits = BINARY64_CANONICAL_NAN;
        }
    }
    return st_float_primitive_box_bits(&context, result_bits, result_out);
}

#define DEFINE_ARITHMETIC(name_, operation_)                                \
    static st_float_primitive_status_t name_(                               \
        st_float_primitive_state_t *state, st_value_t receiver,             \
        const st_value_t *arguments, st_value_t *result_out)                \
    {                                                                       \
        return arithmetic_float_values(state, receiver, arguments,          \
                                       result_out, (operation_));            \
    }

DEFINE_ARITHMETIC(primitive_float_add, FLOAT_ARITHMETIC_ADD)
DEFINE_ARITHMETIC(primitive_float_subtract, FLOAT_ARITHMETIC_SUBTRACT)
DEFINE_ARITHMETIC(primitive_float_multiply, FLOAT_ARITHMETIC_MULTIPLY)
DEFINE_ARITHMETIC(primitive_float_divide, FLOAT_ARITHMETIC_DIVIDE)

#undef DEFINE_ARITHMETIC

static st_float_primitive_status_t primitive_float_negate(
    st_float_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_float_primitive_context_t context = { state };
    uint64_t bits;
    st_float_primitive_status_t status;
    (void)arguments;
    status = st_float_primitive_unbox_bits(&context, receiver, &bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    return st_float_primitive_box_bits(&context, bits ^ BINARY64_SIGN_MASK,
                                       result_out);
}

static uint64_t float_hash_mix(uint64_t bits)
{
    bits ^= UINT64_C(0xd6e8feb86659fd93);
    bits += UINT64_C(0x9e3779b97f4a7c15);
    bits = (bits ^ (bits >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    bits = (bits ^ (bits >> 27)) * UINT64_C(0x94d049bb133111eb);
    return (bits ^ (bits >> 31)) & (uint64_t)ST_SMALL_INTEGER_MAX;
}

static st_float_primitive_status_t primitive_float_hash(
    st_float_primitive_state_t *state, st_value_t receiver,
    const st_value_t *arguments, st_value_t *result_out)
{
    st_float_primitive_context_t context = { state };
    uint64_t bits;
    st_float_primitive_status_t status;
    (void)arguments;

    status = st_float_primitive_unbox_bits(&context, receiver, &bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }

    /* IEEE equality identifies +0.0 and -0.0.  Every other pair of equal,
     * non-NaN binary64 values has identical bits.  NaNs are unequal even to
     * themselves, so retaining payload/sign in their hash is valid. */
    if ((bits & ~BINARY64_SIGN_MASK) == 0u) {
        bits = 0u;
    }
    if (!st_value_from_small_integer(
            (int64_t)float_hash_mix(bits), result_out)) {
        return ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT;
    }
    return ST_FLOAT_PRIMITIVE_OK;
}

typedef enum {
    FLOAT_INTEGER_TRUNCATED,
    FLOAT_INTEGER_FLOOR,
    FLOAT_INTEGER_CEILING,
    FLOAT_INTEGER_ROUNDED
} float_integer_kind_t;

static st_float_primitive_status_t evaluate_integer_rounding(
    double value, float_integer_kind_t operation, double *result_out)
{
    fenv_t saved_environment;
    volatile double volatile_value = value;
    double result;
    if (!result_out || feholdexcept(&saved_environment) != 0) {
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    if (fesetround(FE_TONEAREST) != 0) {
        (void)fesetenv(&saved_environment);
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    switch (operation) {
    case FLOAT_INTEGER_TRUNCATED:
        result = trunc(volatile_value);
        break;
    case FLOAT_INTEGER_FLOOR:
        result = floor(volatile_value);
        break;
    case FLOAT_INTEGER_CEILING:
        result = ceil(volatile_value);
        break;
    case FLOAT_INTEGER_ROUNDED:
        result = round(volatile_value);
        break;
    default:
        (void)fesetenv(&saved_environment);
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    *result_out = result;
    if (fesetenv(&saved_environment) != 0) {
        return ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT;
    }
    return ST_FLOAT_PRIMITIVE_OK;
}

static st_float_primitive_status_t float_to_small_integer(
    st_float_primitive_state_t *state, st_value_t receiver,
    st_value_t *result_out, float_integer_kind_t operation)
{
    st_float_primitive_context_t context = { state };
    uint64_t bits;
    double rounded;
    int64_t integer;
    st_float_primitive_status_t status = st_float_primitive_unbox_bits(
        &context, receiver, &bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    if (binary64_is_nan(bits) || binary64_is_infinite(bits)) {
        return ST_FLOAT_PRIMITIVE_ERR_NON_FINITE;
    }
    status = evaluate_integer_rounding(double_from_bits(bits), operation,
                                       &rounded);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return status;
    }
    /* These hexadecimal bounds are represented exactly.  The strict upper
       comparison avoids converting +2^60, while -2^60 is representable. */
    if (rounded < -0x1p60 || rounded >= 0x1p60) {
        return ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    }
    integer = (int64_t)rounded;
    if (!st_value_from_small_integer(integer, result_out)) {
        return ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED;
    }
    return ST_FLOAT_PRIMITIVE_OK;
}

#define DEFINE_INTEGER_CONVERSION(name_, operation_)                        \
    static st_float_primitive_status_t name_(                               \
        st_float_primitive_state_t *state, st_value_t receiver,             \
        const st_value_t *arguments, st_value_t *result_out)                \
    {                                                                       \
        (void)arguments;                                                    \
        return float_to_small_integer(state, receiver, result_out,          \
                                      (operation_));                        \
    }

DEFINE_INTEGER_CONVERSION(primitive_float_truncated, FLOAT_INTEGER_TRUNCATED)
DEFINE_INTEGER_CONVERSION(primitive_float_floor, FLOAT_INTEGER_FLOOR)
DEFINE_INTEGER_CONVERSION(primitive_float_ceiling, FLOAT_INTEGER_CEILING)
DEFINE_INTEGER_CONVERSION(primitive_float_rounded, FLOAT_INTEGER_ROUNDED)

#undef DEFINE_INTEGER_CONVERSION

#define FLOAT_SPEC(name_, arity_, failure_, id_, handler_, symbol_)         \
    {                                                                       \
        (name_), sizeof(name_) - 1u, (arity_), ST_PRIMITIVE_INSTANCE_ONLY,  \
        (failure_), ST_PRIMITIVE_RUNTIME_SYMBOL,                            \
        ST_PRIMITIVE_INVALID_INTRINSIC_ID, (symbol_), sizeof(symbol_) - 1u  \
    },
static const st_primitive_spec_t float_specs[] = {
    FLOAT_PRIMITIVE_LIST(FLOAT_SPEC)
};
#undef FLOAT_SPEC

#define FLOAT_DEFINITION(name_, arity_, failure_, id_, handler_, symbol_)   \
    { (id_), (arity_), (handler_) },
static const float_definition_t float_definitions[] = {
    FLOAT_PRIMITIVE_LIST(FLOAT_DEFINITION)
};
#undef FLOAT_DEFINITION

#define FLOAT_PRIMITIVE_COUNT                                               \
    (sizeof(float_definitions) / sizeof(float_definitions[0]))
_Static_assert(sizeof(float_specs) / sizeof(float_specs[0]) ==
               FLOAT_PRIMITIVE_COUNT,
               "Float spec and handler tables must remain aligned");
_Static_assert(ST_FLOAT_OPERATION_HASH - ST_FLOAT_OPERATION_EQUALS + 1 ==
               FLOAT_PRIMITIVE_COUNT,
               "Float intrinsic IDs must remain dense");

static const float_definition_t *definition_lookup(uint32_t operation_id)
{
    size_t index;
    if (operation_id < ST_FLOAT_OPERATION_EQUALS) {
        return NULL;
    }
    index = (size_t)(operation_id - ST_FLOAT_OPERATION_EQUALS);
    if (index >= FLOAT_PRIMITIVE_COUNT ||
        float_definitions[index].id != operation_id ||
        float_specs[index].method_arity != float_definitions[index].arity) {
        return NULL;
    }
    return &float_definitions[index];
}

st_float_primitive_status_t st_float_primitive_execute_internal(
    st_float_primitive_context_t *context, st_float_operation_t operation,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out)
{
    st_float_primitive_state_t *state = context_state(context);
    const float_definition_t *definition;
    if (result_out) {
        *result_out = ST_VALUE_INVALID;
    }
    if (!state || !result_out || (argument_count != 0u && !arguments)) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    definition = definition_lookup((uint32_t)operation);
    if (!definition) {
        return ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION;
    }
    if (argument_count != definition->arity) {
        return ST_FLOAT_PRIMITIVE_ERR_WRONG_ARITY;
    }
    if (!definition->handler) {
        return ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION;
    }
    return definition->handler(state, receiver, arguments, result_out);
}

const st_primitive_spec_t *st_float_primitive_specs(size_t *count_out)
{
    if (count_out) {
        *count_out = FLOAT_PRIMITIVE_COUNT;
    }
    return float_specs;
}

const char *st_float_primitive_status_string(
    st_float_primitive_status_t status)
{
    switch (status) {
    case ST_FLOAT_PRIMITIVE_OK:
        return "ok";
    case ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION:
        return "unknown Float operation";
    case ST_FLOAT_PRIMITIVE_ERR_WRONG_ARITY:
        return "wrong arity";
    case ST_FLOAT_PRIMITIVE_ERR_INVALID_VALUE:
        return "invalid value encoding";
    case ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH:
        return "type mismatch";
    case ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER:
        return "object is not a heap member";
    case ST_FLOAT_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return "dangling reference";
    case ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return "invalid Float64 descriptor";
    case ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT:
        return "invalid heap object";
    case ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ST_FLOAT_PRIMITIVE_ERR_OVERFLOW:
        return "allocation overflow";
    case ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED:
        return "integer promotion required";
    case ST_FLOAT_PRIMITIVE_ERR_NON_FINITE:
        return "non-finite value cannot convert to integer";
    case ST_FLOAT_PRIMITIVE_ERR_FLOAT_ENVIRONMENT:
        return "floating-point environment failure";
    default:
        return "unknown Float primitive status";
    }
}
