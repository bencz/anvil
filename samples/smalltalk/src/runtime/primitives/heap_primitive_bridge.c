#include "st_heap_primitive_bridge.h"

#include <stdlib.h>

st_heap_primitive_status_t st_aot_heap_primitive_execute(
    StFrame *frame, uint32_t intrinsic_id, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out)
{
    if (result_out != NULL) *result_out = 0u;
    if (result_out == NULL || frame == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    st_aot_thread_t *thread = frame->thread;
    if (thread->heap_primitives == NULL)
        return ST_HEAP_PRIMITIVE_ERR_INVALID_ARGUMENT;
    return st_heap_primitive_execute(
        thread->heap_primitives, intrinsic_id, receiver, arguments,
        argument_count, result_out);
}

_Noreturn st_value_t st_aot_heap_primitive_contract_violation(
    uint32_t intrinsic_id, st_heap_primitive_status_t status,
    const StFrame *frame)
{
    (void)intrinsic_id;
    (void)status;
    (void)frame;
    abort();
}
