#ifndef ANVIL_SMALLTALK_INTEGER_PRIMITIVES_H
#define ANVIL_SMALLTALK_INTEGER_PRIMITIVES_H

#include "st_float_primitives.h"
#include "st_dispatch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_INTEGER_PRIMITIVE_ABI_VERSION UINT32_C(1)
#define ST_LARGE_INTEGER_LIMB_BITS UINT32_C(32)
#define ST_LARGE_INTEGER_LIMB_MASK UINT64_C(0xffffffff)

/* Stable language opcodes used by Integer>>largeOp:with:.  Values 4 and 5
 * are intentionally unused; changing the existing image protocol would make
 * old AOT images silently perform a different operation. */
typedef enum {
    ST_INTEGER_BINARY_ADD = 1,
    ST_INTEGER_BINARY_SUBTRACT = 2,
    ST_INTEGER_BINARY_MULTIPLY = 3,
    ST_INTEGER_BINARY_FLOOR_DIVIDE = 6,
    ST_INTEGER_BINARY_MODULO = 7
} st_integer_binary_operation_t;

typedef enum {
    ST_INTEGER_PRIMITIVE_OK = 0,
    ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_INTEGER_PRIMITIVE_ERR_INVALID_STATE,
    ST_INTEGER_PRIMITIVE_ERR_WRONG_ARITY,
    ST_INTEGER_PRIMITIVE_ERR_INVALID_VALUE,
    ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER,
    ST_INTEGER_PRIMITIVE_ERR_DANGLING_REFERENCE,
    ST_INTEGER_PRIMITIVE_ERR_INVALID_DESCRIPTOR,
    ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT,
    ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL,
    ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION,
    ST_INTEGER_PRIMITIVE_ERR_DIVISION_BY_ZERO,
    ST_INTEGER_PRIMITIVE_ERR_SHIFT_OUT_OF_RANGE,
    ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_INTEGER_PRIMITIVE_ERR_OVERFLOW,
    ST_INTEGER_PRIMITIVE_ERR_NON_FINITE,
    ST_INTEGER_PRIMITIVE_ERR_FLOAT
} st_integer_primitive_status_t;

typedef enum {
    ST_INTEGER_ROUND_TOWARD_ZERO = 0,
    ST_INTEGER_ROUND_FLOOR,
    ST_INTEGER_ROUND_CEILING,
    ST_INTEGER_ROUND_NEAREST_TIES_AWAY
} st_integer_rounding_t;

typedef struct st_numeric_context {
    uint64_t abi_cookie;
    st_heap_t *heap;
    uint32_t large_positive_class_id;
    uint32_t large_positive_shape_id;
    uint32_t large_negative_class_id;
    uint32_t large_negative_shape_id;
    st_float_primitive_context_t *float_primitives;
    st_primitive_allocator_t scratch_allocator;
} st_numeric_context_t;

typedef struct {
    st_heap_t *heap;
    uint32_t large_positive_class_id;
    uint32_t large_positive_shape_id;
    uint32_t large_negative_class_id;
    uint32_t large_negative_shape_id;
    st_float_primitive_context_t *float_primitives;
    /* NULL/NULL selects malloc/free. A half-specified allocator is rejected. */
    st_primitive_allocator_t scratch_allocator;
} st_numeric_options_t;

/*
 * LargeInteger object ABI
 * -----------------------
 *
 * Both concrete sign classes use one non-pointer fixed word followed by a
 * UINT32 indexed magnitude. Logical limb zero is least significant and the
 * radix is exactly 2^32 on every target. The fixed word contains an ABI magic
 * and an explicit sign bit; class, shape and sign must agree. Limbs are
 * canonical: length is nonzero, the most-significant limb is nonzero, and a
 * magnitude representable as a tagged 61-bit SmallInteger is forbidden.
 * Consequently there is only one zero representation: tagged SmallInteger 0.
 * Published LargeIntegers are immutable and contain no GC references.
 */
st_integer_primitive_status_t st_numeric_context_init(
    st_numeric_context_t *context, const st_numeric_options_t *options);
void st_numeric_context_destroy(st_numeric_context_t *context);

/* Constructs a canonical Integer. A small magnitude is deliberately demoted
 * to SmallInteger; leading zero limbs are accepted and normalized. The input
 * is borrowed only for the duration of the call. */
st_integer_primitive_status_t st_integer_from_sign_magnitude(
    st_numeric_context_t *context, bool negative, const uint32_t *limbs,
    size_t limb_count, st_value_t *result_out);

typedef struct {
    bool negative;
    const uint32_t *limbs;
    size_t limb_count;
    bool is_small_integer;
} st_integer_view_t;

/* Authenticates and exposes a borrowed canonical magnitude view. A
 * SmallInteger is represented by an internal one- or two-limb snapshot stored
 * in small_limb_storage, so the view remains valid until that storage changes.
 */
st_integer_primitive_status_t st_integer_view(
    st_numeric_context_t *context, st_value_t value,
    uint32_t small_limb_storage[2], st_integer_view_t *view_out);

st_integer_primitive_status_t st_integer_binary(
    st_numeric_context_t *context, st_value_t receiver,
    st_integer_binary_operation_t operation, st_value_t argument,
    st_value_t *result_out);
st_integer_primitive_status_t st_integer_compare(
    st_numeric_context_t *context, st_value_t receiver, st_value_t argument,
    st_value_t *result_out);
st_integer_primitive_status_t st_integer_shift(
    st_numeric_context_t *context, st_value_t receiver, st_value_t count,
    st_value_t *result_out);
st_integer_primitive_status_t st_integer_as_float(
    st_numeric_context_t *context, st_value_t receiver,
    st_value_t *result_out);
st_integer_primitive_status_t st_integer_hash(
    st_numeric_context_t *context, st_value_t receiver,
    st_value_t *result_out);

/* Converts the value encoded by raw IEEE-754 binary64 bits without passing
 * through the host floating-point environment. Finite values are rounded as
 * requested and then published canonically as SmallInteger or LargeInteger.
 * NaNs and infinities return ERR_NON_FINITE; both signed zeros become tagged
 * SmallInteger zero. */
st_integer_primitive_status_t st_integer_from_binary64_bits(
    st_numeric_context_t *context, uint64_t bits,
    st_integer_rounding_t rounding, st_value_t *result_out);

/* Runtime-symbol specs and their exact generic AOT ABI entry points. */
const st_primitive_spec_t *st_integer_primitive_specs(size_t *count_out);

uint32_t st_aot_large_integer_binary_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);
uint32_t st_aot_large_integer_compare_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);
uint32_t st_aot_large_integer_shift_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);
uint32_t st_aot_large_integer_as_float_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);
uint32_t st_aot_small_integer_as_float_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);
uint32_t st_aot_integer_hash_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out);

const char *st_integer_primitive_status_string(
    st_integer_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
