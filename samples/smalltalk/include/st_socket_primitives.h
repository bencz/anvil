#ifndef ANVIL_SMALLTALK_SOCKET_PRIMITIVES_H
#define ANVIL_SMALLTALK_SOCKET_PRIMITIVES_H

#include "st_socket.h"
#include "st_primitive.h"

const st_primitive_spec_t *st_socket_primitive_specs(size_t *count_out);

uint32_t st_aot_socket_interrupt(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_listen(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_accept(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_close(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_port(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_receive(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

uint32_t st_aot_socket_send(StFrame *frame, st_value_t receiver, const st_value_t *arguments, size_t argument_count,
    st_value_t *result_out, uint32_t *detail_out);

#endif
