#include "st_string_primitive_bridge.h"

#include "st_send_bridge.h"
#include "st_string_primitives.h"

static void clear_outputs(st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out != NULL) {
        *result_out = ST_VALUE_INVALID;
    }
    if (detail_out != NULL) {
        *detail_out = 0u;
    }
}

static uint32_t execute_bridge(
    StFrame *frame, st_string_operation_t operation,
    st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_aot_thread_t *thread;

    clear_outputs(result_out, detail_out);
    if (frame == NULL || result_out == NULL || detail_out == NULL
            || argument_count != 1u || arguments == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK) {
        return (uint32_t)ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    thread = frame->thread;
    if (thread->strings == NULL) {
        return (uint32_t)ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    return (uint32_t)st_string_primitive_execute(
        thread->strings, operation, receiver, arguments, argument_count,
        result_out);
}

uint32_t st_aot_string_compare_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    return execute_bridge(
        frame, ST_STRING_OPERATION_COMPARE, receiver, arguments,
        argument_count, result_out, detail_out);
}

uint32_t st_aot_string_concat_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    return execute_bridge(
        frame, ST_STRING_OPERATION_CONCAT, receiver, arguments,
        argument_count, result_out, detail_out);
}
