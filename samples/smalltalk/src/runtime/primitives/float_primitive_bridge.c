#include "st_float_primitive_bridge.h"

#include "st_float_primitives.h"
#include "float_primitives_internal.h"
#include "st_integer_primitives.h"
#include "st_send_bridge.h"

#include <stdbool.h>

static void clear_outputs(st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out != NULL) {
        *result_out = ST_VALUE_INVALID;
    }
    if (detail_out != NULL) {
        *detail_out = 0u;
    }
}

static st_float_primitive_status_t bridge_context(
    StFrame *frame, size_t expected_arity, const st_value_t *arguments,
    size_t argument_count, st_numeric_context_t **numeric_out)
{
    st_aot_thread_t *thread;

    *numeric_out = NULL;
    if (frame == NULL || argument_count != expected_arity
            || (argument_count == 0u) != (arguments == NULL)
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    thread = frame->thread;
    if (thread->numeric == NULL || thread->numeric->float_primitives == NULL)
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    *numeric_out = thread->numeric;
    return ST_FLOAT_PRIMITIVE_OK;
}

static bool conversion_rounding(
    st_float_operation_t operation, st_integer_rounding_t *rounding_out)
{
    switch (operation) {
    case ST_FLOAT_OPERATION_TRUNCATED:
        *rounding_out = ST_INTEGER_ROUND_TOWARD_ZERO;
        return true;
    case ST_FLOAT_OPERATION_FLOOR:
        *rounding_out = ST_INTEGER_ROUND_FLOOR;
        return true;
    case ST_FLOAT_OPERATION_CEILING:
        *rounding_out = ST_INTEGER_ROUND_CEILING;
        return true;
    case ST_FLOAT_OPERATION_ROUNDED:
        *rounding_out = ST_INTEGER_ROUND_NEAREST_TIES_AWAY;
        return true;
    default:
        return false;
    }
}

static st_float_primitive_status_t map_integer_status(
    st_integer_primitive_status_t status)
{
    switch (status) {
    case ST_INTEGER_PRIMITIVE_OK:
        return ST_FLOAT_PRIMITIVE_OK;
    case ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY:
        return ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY;
    case ST_INTEGER_PRIMITIVE_ERR_OVERFLOW:
        return ST_FLOAT_PRIMITIVE_ERR_OVERFLOW;
    case ST_INTEGER_PRIMITIVE_ERR_NON_FINITE:
        return ST_FLOAT_PRIMITIVE_ERR_NON_FINITE;
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_VALUE:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_VALUE;
    case ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH:
        return ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH;
    case ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER:
        return ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER;
    case ST_INTEGER_PRIMITIVE_ERR_DANGLING_REFERENCE:
        return ST_FLOAT_PRIMITIVE_ERR_DANGLING_REFERENCE;
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_DESCRIPTOR:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR;
    case ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT:
    case ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL:
        return ST_FLOAT_PRIMITIVE_ERR_BAD_OBJECT;
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT:
    case ST_INTEGER_PRIMITIVE_ERR_INVALID_STATE:
    case ST_INTEGER_PRIMITIVE_ERR_WRONG_ARITY:
    case ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION:
    case ST_INTEGER_PRIMITIVE_ERR_DIVISION_BY_ZERO:
    case ST_INTEGER_PRIMITIVE_ERR_SHIFT_OUT_OF_RANGE:
    case ST_INTEGER_PRIMITIVE_ERR_FLOAT:
    default:
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
}

static uint32_t execute_bridge(
    StFrame *frame, st_float_operation_t operation, size_t expected_arity,
    st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    st_numeric_context_t *numeric;
    st_float_primitive_status_t status;
    st_integer_rounding_t rounding;
    st_integer_primitive_status_t integer_status;
    uint64_t bits;

    clear_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL) {
        return ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    status = bridge_context(
        frame, expected_arity, arguments, argument_count, &numeric);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return (uint32_t)status;
    }

    status = st_float_primitive_execute_internal(
        numeric->float_primitives, operation, receiver, arguments,
        argument_count, result_out);
    if (status != ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED) {
        return (uint32_t)status;
    }

    /* Only integer conversions can request promotion.  The first attempt
     * performed no allocation, so this is a continuation of that operation,
     * never a retry after OOM. */
    if (!conversion_rounding(operation, &rounding)) {
        return (uint32_t)status;
    }
    status = st_float_primitive_unbox_bits(
        numeric->float_primitives, receiver, &bits);
    if (status != ST_FLOAT_PRIMITIVE_OK) {
        return (uint32_t)status;
    }

    integer_status = st_integer_from_binary64_bits(
        numeric, bits, rounding, result_out);
    *detail_out = (uint32_t)integer_status;
    return (uint32_t)map_integer_status(integer_status);
}

#define DEFINE_FLOAT_BRIDGE(name_, intrinsic_, arity_)                      \
    uint32_t name_(                                                         \
        StFrame *frame, st_value_t receiver, const st_value_t *arguments,   \
        size_t argument_count, st_value_t *result_out,                      \
        uint32_t *detail_out)                                               \
    {                                                                       \
        return execute_bridge(                                              \
            frame, (intrinsic_), (arity_), receiver, arguments,             \
            argument_count, result_out, detail_out);                        \
    }

DEFINE_FLOAT_BRIDGE(
    st_aot_float_equals_primitive_execute,
    ST_FLOAT_OPERATION_EQUALS, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_less_than_primitive_execute,
    ST_FLOAT_OPERATION_LESS_THAN, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_greater_than_primitive_execute,
    ST_FLOAT_OPERATION_GREATER_THAN, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_less_equals_primitive_execute,
    ST_FLOAT_OPERATION_LESS_EQUALS, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_greater_equals_primitive_execute,
    ST_FLOAT_OPERATION_GREATER_EQUALS, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_add_primitive_execute,
    ST_FLOAT_OPERATION_ADD, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_subtract_primitive_execute,
    ST_FLOAT_OPERATION_SUBTRACT, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_multiply_primitive_execute,
    ST_FLOAT_OPERATION_MULTIPLY, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_divide_primitive_execute,
    ST_FLOAT_OPERATION_DIVIDE, 1u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_negate_primitive_execute,
    ST_FLOAT_OPERATION_NEGATE, 0u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_truncated_primitive_execute,
    ST_FLOAT_OPERATION_TRUNCATED, 0u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_floor_primitive_execute,
    ST_FLOAT_OPERATION_FLOOR, 0u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_ceiling_primitive_execute,
    ST_FLOAT_OPERATION_CEILING, 0u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_rounded_primitive_execute,
    ST_FLOAT_OPERATION_ROUNDED, 0u)
DEFINE_FLOAT_BRIDGE(
    st_aot_float_hash_primitive_execute,
    ST_FLOAT_OPERATION_HASH, 0u)

#undef DEFINE_FLOAT_BRIDGE
