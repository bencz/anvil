#include "st_control_bridge.h"

#include <stdlib.h>

static st_control_status_t frame_control(
    StFrame *frame, st_control_thread_t **control_out)
{
    st_aot_thread_t *thread;
    st_control_pending_info_t pending;
    st_aot_send_status_t frame_status;

    if (control_out == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *control_out = NULL;
    if (frame == NULL) return ST_CONTROL_ERR_INVALID_FRAME;
    frame_status = st_aot_frame_validate(frame, 0u);
    if (frame_status == ST_AOT_SEND_ERR_INVALID_THREAD)
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (frame_status != ST_AOT_SEND_OK)
        return ST_CONTROL_ERR_INVALID_FRAME;
    thread = frame->thread;
    if (thread->control == NULL)
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (thread->control->_st_frame_thread_identity != thread)
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (st_control_pending_get(thread->control, &pending) != ST_CONTROL_OK)
        return ST_CONTROL_ERR_INVALID_THREAD;
    *control_out = thread->control;
    return ST_CONTROL_OK;
}

st_control_status_t st_aot_control_scope_enter(
    StFrame *frame, st_control_scope_t *scope, uint32_t establish_home)
{
    st_control_thread_t *control;
    st_control_status_t status = frame_control(frame, &control);
    if (status != ST_CONTROL_OK) return status;
    if (scope == NULL || establish_home > 1u)
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    st_control_scope_init(scope);
    status = st_control_scope_enter(control, scope, frame);
    if (status != ST_CONTROL_OK) return status;
    if (establish_home) {
        StHomeToken *home = NULL;
        status = st_control_scope_establish_home(control, scope, &home);
        if (status != ST_CONTROL_OK) {
            st_control_leave_result_t ignored;
            st_control_status_t rollback = st_control_scope_leave(
                control, scope, st_value_nil(), &ignored);
            if (rollback != ST_CONTROL_OK) abort();
            return status;
        }
        if (home == NULL || frame->home != home) abort();
    }
    return ST_CONTROL_OK;
}

st_control_status_t st_aot_control_scope_leave(
    StFrame *frame, st_control_scope_t *scope, st_value_t normal_value,
    st_value_t *value_out)
{
    st_control_thread_t *control;
    st_control_leave_result_t result;
    st_control_status_t status;
    if (value_out == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *value_out = st_value_nil();
    status = frame_control(frame, &control);
    if (status != ST_CONTROL_OK) return status;
    status = st_control_scope_leave(control, scope, normal_value, &result);
    if (status == ST_CONTROL_OK) *value_out = result.value;
    return status;
}

st_control_status_t st_aot_control_pending(
    StFrame *frame, uint32_t *pending_out, st_value_t *value_out)
{
    st_control_thread_t *control;
    st_control_pending_info_t pending;
    st_control_status_t status;
    if (pending_out == NULL || value_out == NULL)
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *pending_out = 0u;
    *value_out = st_value_nil();
    status = frame_control(frame, &control);
    if (status != ST_CONTROL_OK) return status;
    status = st_control_pending_get(control, &pending);
    if (status == ST_CONTROL_OK) {
        *pending_out = pending.kind != ST_CONTROL_PENDING_NONE;
        *value_out = pending.value;
    }
    return status;
}

st_control_status_t st_aot_control_non_local_return(
    StFrame *frame, StHomeToken *target, st_value_t value)
{
    st_control_thread_t *control;
    st_control_status_t status = frame_control(frame, &control);
    if (status != ST_CONTROL_OK) return status;
    return st_control_non_local_return(control, target, value);
}

_Noreturn st_value_t st_aot_control_contract_violation(
    st_control_status_t status, StFrame *frame)
{
    (void)frame;
    (void)status;
    abort();
}
