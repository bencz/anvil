#include "st_fiber_primitives.h"

uint32_t st_aot_fiber_spawn(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    uint64_t id;

    status = st_fiber_spawn(frame, arguments[0], &id);
    if (status == ST_FIBER_OK)
        st_value_from_small_integer((int64_t)id, result_out);

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_yield(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 0u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    status = st_fiber_yield(frame);
    if (status == ST_FIBER_OK)
        *result_out = receiver;

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_sleep(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    int64_t duration;

    if (!st_value_to_small_integer(arguments[0], &duration) || duration < 0 || duration > UINT32_MAX)
        return ST_FIBER_INVALID_ARGUMENT;

    status = st_fiber_sleep(frame, (uint32_t)duration);
    if (status == ST_FIBER_OK)
        *result_out = receiver;

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_join(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    int64_t id;

    if (!st_value_to_small_integer(arguments[0], &id) || id <= 0)
        return ST_FIBER_INVALID_ARGUMENT;

    status = st_fiber_join(frame, (uint64_t)id, result_out);
    if (status == ST_FIBER_EXCEPTION)
        status = ST_FIBER_OK;

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_run(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 0u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    status = st_fiber_run(frame);
    if (status == ST_FIBER_EXCEPTION)
        status = ST_FIBER_OK;

    if (status == ST_FIBER_OK)
        *result_out = receiver;

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_detach(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    int64_t id;

    if (!st_value_to_small_integer(arguments[0], &id) || id <= 0)
        return ST_FIBER_INVALID_ARGUMENT;

    status = st_fiber_detach(frame, (uint64_t)id);
    if (status == ST_FIBER_OK)
        *result_out = receiver;

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

uint32_t st_aot_fiber_collect(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 0u || (arguments == NULL) != (argument_count == 0u))
        return ST_FIBER_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_FIBER_INVALID_FRAME;

    st_fiber_status_t status;
    size_t reclaimed;

    status = st_fiber_collect(frame, &reclaimed);
    if (status == ST_FIBER_EXCEPTION)
        status = ST_FIBER_OK;

    if (status == ST_FIBER_OK)
        st_value_from_small_integer((int64_t)reclaimed, result_out);

    *detail_out = (uint32_t)status;
    return (uint32_t)status;
}

