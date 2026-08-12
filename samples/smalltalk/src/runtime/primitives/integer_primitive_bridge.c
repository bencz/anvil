#include "st_integer_primitives.h"
#include "st_send_bridge.h"

#include <limits.h>

static st_integer_primitive_status_t bridge_context(
    StFrame *frame, size_t expected_arity, const st_value_t *arguments,
    size_t argument_count, st_numeric_context_t **context_out)
{
    st_aot_thread_t *thread;

    *context_out = NULL;
    if (frame == NULL || argument_count != expected_arity
            || (argument_count == 0u) != (arguments == NULL)
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    thread = frame->thread;
    if (thread->numeric == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_STATE;
    *context_out = thread->numeric;
    return ST_INTEGER_PRIMITIVE_OK;
}

static void bridge_outputs(st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out != NULL) *result_out = ST_VALUE_INVALID;
    if (detail_out != NULL) *detail_out = 0u;
}

uint32_t st_aot_large_integer_binary_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_numeric_context_t *context;
    st_integer_primitive_status_t status;
    int64_t operation;

    bridge_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = bridge_context(
        frame, 2u, arguments, argument_count, &context);
    if (status != ST_INTEGER_PRIMITIVE_OK) return (uint32_t)status;
    if (!st_value_to_small_integer(arguments[0], &operation)
            || operation < INT32_MIN || operation > INT32_MAX)
        return ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION;
    status = st_integer_binary(
        context, receiver, (st_integer_binary_operation_t)operation,
        arguments[1], result_out);
    if (status == ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION)
        *detail_out = (uint32_t)(int32_t)operation;
    return (uint32_t)status;
}

uint32_t st_aot_large_integer_compare_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_numeric_context_t *context;
    st_integer_primitive_status_t status;

    bridge_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = bridge_context(
        frame, 1u, arguments, argument_count, &context);
    if (status != ST_INTEGER_PRIMITIVE_OK) return (uint32_t)status;
    return (uint32_t)st_integer_compare(
        context, receiver, arguments[0], result_out);
}

uint32_t st_aot_large_integer_shift_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_numeric_context_t *context;
    st_integer_primitive_status_t status;

    bridge_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = bridge_context(
        frame, 1u, arguments, argument_count, &context);
    if (status != ST_INTEGER_PRIMITIVE_OK) return (uint32_t)status;
    return (uint32_t)st_integer_shift(
        context, receiver, arguments[0], result_out);
}

static uint32_t bridge_as_float(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out,
    bool require_small)
{
    st_numeric_context_t *context;
    st_integer_primitive_status_t status;
    int64_t ignored;

    bridge_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = bridge_context(
        frame, 0u, arguments, argument_count, &context);
    if (status != ST_INTEGER_PRIMITIVE_OK) return (uint32_t)status;
    if (require_small && !st_value_to_small_integer(receiver, &ignored))
        return ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH;
    if (!require_small && st_value_to_small_integer(receiver, &ignored))
        return ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH;
    return (uint32_t)st_integer_as_float(context, receiver, result_out);
}

uint32_t st_aot_large_integer_as_float_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    return bridge_as_float(
        frame, receiver, arguments, argument_count,
        result_out, detail_out, false);
}

uint32_t st_aot_small_integer_as_float_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    return bridge_as_float(
        frame, receiver, arguments, argument_count,
        result_out, detail_out, true);
}

uint32_t st_aot_integer_hash_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_numeric_context_t *context;
    st_integer_primitive_status_t status;

    bridge_outputs(result_out, detail_out);
    if (result_out == NULL || detail_out == NULL)
        return ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = bridge_context(
        frame, 0u, arguments, argument_count, &context);
    if (status != ST_INTEGER_PRIMITIVE_OK) return (uint32_t)status;
    return (uint32_t)st_integer_hash(context, receiver, result_out);
}
