#include "st_socket_primitives.h"

static bool integer_argument(st_value_t argument, int64_t minimum, int64_t maximum, int64_t *value_out)
{
    return st_value_to_small_integer(argument, value_out) && *value_out >= minimum && *value_out <= maximum;
}

uint32_t st_aot_socket_interrupt(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    int64_t handle;
    int error;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle))
        return ST_SOCKET_INVALID_ARGUMENT;

    st_socket_status_t status = st_socket_interrupt(frame, (uint64_t)handle, &error);

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(-(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_listen(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t port;
    uint64_t handle;

    if (!integer_argument(arguments[0], 0, UINT16_MAX, &port))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_listen(frame, (uint16_t)port, &handle, &error);
    value = (int64_t)handle;

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_accept(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 2u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t handle;
    int64_t timeout;
    uint64_t accepted;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle) || !integer_argument(arguments[1], 0, UINT32_MAX, &timeout))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_accept(frame, (uint64_t)handle, (uint32_t)timeout, &accepted, &error);
    value = (int64_t)accepted;

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_close(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t handle;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_close(frame, (uint64_t)handle, &error);

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_port(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 1u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t handle;
    uint16_t port;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_port(frame, (uint64_t)handle, &port, &error);
    value = port;

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_receive(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 5u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t handle;
    int64_t start;
    int64_t count;
    int64_t timeout;
    size_t transferred;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle)
            || !integer_argument(arguments[2], 1, ST_SMALL_INTEGER_MAX, &start)
            || !integer_argument(arguments[3], 0, ST_SMALL_INTEGER_MAX, &count)
            || !integer_argument(arguments[4], 0, UINT32_MAX, &timeout))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_receive(frame, (uint64_t)handle, arguments[1], (size_t)(start - 1), (size_t)count,
        (uint32_t)timeout, &transferred, &error);
    value = (int64_t)transferred;

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}

uint32_t st_aot_socket_send(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out)
{
    if (result_out == NULL || detail_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    *detail_out = 0u;

    if (argument_count != 5u || arguments == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK || frame->receiver != receiver)
        return ST_SOCKET_INVALID_FRAME;

    st_socket_status_t status;
    int error = 0;
    int64_t value = 0;
    int64_t handle;
    int64_t start;
    int64_t count;
    int64_t timeout;
    size_t transferred;

    if (!integer_argument(arguments[0], 1, ST_SMALL_INTEGER_MAX, &handle)
            || !integer_argument(arguments[2], 1, ST_SMALL_INTEGER_MAX, &start)
            || !integer_argument(arguments[3], 0, ST_SMALL_INTEGER_MAX, &count)
            || !integer_argument(arguments[4], 0, UINT32_MAX, &timeout))
        return ST_SOCKET_INVALID_ARGUMENT;

    status = st_socket_send(frame, (uint64_t)handle, arguments[1], (size_t)(start - 1), (size_t)count,
        (uint32_t)timeout, &transferred, &error);
    value = (int64_t)transferred;

    *detail_out = (uint32_t)error;
    st_value_from_small_integer(status == ST_SOCKET_OK ? value : -(int64_t)status, result_out);
    return 0u;
}
