#include "st_reflection_primitive_bridge.h"
#include "st_reflection_primitives.h"
#include "st_send_bridge.h"

uint32_t st_aot_behavior_lookup_selector_primitive_execute(
    StFrame *frame, st_value_t receiver, const st_value_t *arguments,
    size_t argument_count, st_value_t *result_out, uint32_t *detail_out)
{
    st_reflection_primitive_status_t status;
    st_aot_thread_t *thread;

    if (result_out != NULL) {
        *result_out = ST_VALUE_INVALID;
    }
    if (detail_out != NULL) {
        *detail_out = 0u;
    }
    if (frame == NULL || result_out == NULL || detail_out == NULL
            || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK) {
        return (uint32_t)ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    thread = frame->thread;
    if (thread->reflection == NULL) {
        return (uint32_t)ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }

    if (argument_count != 1u) {
        *detail_out = (uint32_t)argument_count;
        return (uint32_t)ST_REFLECTION_PRIMITIVE_ERR_WRONG_ARITY;
    }
    if (arguments == NULL) {
        return (uint32_t)ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }

    status = st_reflection_lookup_selector(
        thread->reflection, receiver, arguments[0], result_out);
    return (uint32_t)status;
}
