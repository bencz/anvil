#include "st_stream_primitive_bridge.h"

#include <limits.h>
#include <stdlib.h>

uint32_t st_aot_stream_write_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_aot_thread_t *thread;
    st_stream_primitive_status_t status;
    int os_error = 0;
    if (result_out != NULL) *result_out = (st_value_t)ST_VALUE_INVALID;
    if (detail_out != NULL) *detail_out = 0u;
    if (frame == NULL || result_out == NULL || detail_out == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return (uint32_t)ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    thread = frame->thread;
    if (thread->streams == NULL)
        return (uint32_t)ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT;
    status = st_stream_write_primitive_execute(
        thread->streams, receiver, arguments, argument_count,
        result_out, &os_error);
    if (status == ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED) {
        *detail_out = os_error > 0 ? (uint32_t)os_error : UINT32_C(1);
    } else if (os_error != 0) {
        *result_out = (st_value_t)ST_VALUE_INVALID;
        return (uint32_t)ST_STREAM_PRIMITIVE_ERR_WRITE_CONTRACT;
    }
    return (uint32_t)status;
}

_Noreturn st_value_t st_aot_stream_primitive_contract_violation(
    uint32_t status, uint32_t detail, const StFrame *frame)
{
    (void)status;
    (void)detail;
    (void)frame;
    abort();
}
