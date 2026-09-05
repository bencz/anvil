#include "st_socket_primitives.h"

static const st_primitive_spec_t specifications[] = {
    {
        .name = "SocketInterruptPrimitive",
        .name_length = sizeof("SocketInterruptPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_interrupt",
        .runtime_symbol_length = sizeof("st_aot_socket_interrupt") - 1u
    },
    {
        .name = "SocketListenPrimitive",
        .name_length = sizeof("SocketListenPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_listen",
        .runtime_symbol_length = sizeof("st_aot_socket_listen") - 1u
    },
    {
        .name = "SocketAcceptPrimitive",
        .name_length = sizeof("SocketAcceptPrimitive") - 1u,
        .method_arity = 2u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_accept",
        .runtime_symbol_length = sizeof("st_aot_socket_accept") - 1u
    },
    {
        .name = "SocketClosePrimitive",
        .name_length = sizeof("SocketClosePrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_close",
        .runtime_symbol_length = sizeof("st_aot_socket_close") - 1u
    },
    {
        .name = "SocketPortPrimitive",
        .name_length = sizeof("SocketPortPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_port",
        .runtime_symbol_length = sizeof("st_aot_socket_port") - 1u
    },
    {
        .name = "SocketReceivePrimitive",
        .name_length = sizeof("SocketReceivePrimitive") - 1u,
        .method_arity = 5u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_receive",
        .runtime_symbol_length = sizeof("st_aot_socket_receive") - 1u
    },
    {
        .name = "SocketSendPrimitive",
        .name_length = sizeof("SocketSendPrimitive") - 1u,
        .method_arity = 5u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_socket_send",
        .runtime_symbol_length = sizeof("st_aot_socket_send") - 1u
    },
};

const st_primitive_spec_t *st_socket_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(specifications) / sizeof(specifications[0]);

    return specifications;
}
