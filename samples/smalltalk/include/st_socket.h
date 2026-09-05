#ifndef ANVIL_SMALLTALK_SOCKET_H
#define ANVIL_SMALLTALK_SOCKET_H

#include "st_send_bridge.h"

typedef struct st_socket_context st_socket_context_t;

typedef enum {
    ST_SOCKET_OK = 0,
    ST_SOCKET_INVALID_ARGUMENT,
    ST_SOCKET_INVALID_FRAME,
    ST_SOCKET_INVALID_HANDLE,
    ST_SOCKET_INVALID_BUFFER,
    ST_SOCKET_BUSY,
    ST_SOCKET_OUT_OF_MEMORY,
    ST_SOCKET_TIMEOUT,
    ST_SOCKET_OS_ERROR,
    ST_SOCKET_RUNTIME_ERROR,
    ST_SOCKET_INTERRUPTED
} st_socket_status_t;

/* Local TCP server transport. Listen binds IPv4 loopback; port zero selects
 * an ephemeral port. Handles carry slot generations and never expose native
 * descriptors or pointers. One reader and one writer may operate concurrently.
 * Close rejects an active operation; its owner must complete or time out first.
 * Destruction requires quiescence and closes all remaining handles. */
st_socket_status_t st_socket_context_create(st_aot_thread_t *owner, st_socket_context_t **context_out);
st_socket_status_t st_socket_context_destroy(st_socket_context_t *context);
st_socket_status_t st_socket_listen(StFrame *frame, uint16_t port, uint64_t *handle_out, int *error_out);
st_socket_status_t st_socket_accept(StFrame *frame, uint64_t listener, uint32_t timeout, uint64_t *handle_out, int *error_out);
st_socket_status_t st_socket_close(StFrame *frame, uint64_t handle, int *error_out);
st_socket_status_t st_socket_interrupt(StFrame *frame, uint64_t handle, int *error_out);
st_socket_status_t st_socket_port(StFrame *frame, uint64_t handle, uint16_t *port_out, int *error_out);
st_socket_status_t st_socket_receive(StFrame *frame, uint64_t handle, st_value_t buffer, size_t offset, size_t count,
    uint32_t timeout, size_t *transferred_out, int *error_out);
st_socket_status_t st_socket_send(StFrame *frame, uint64_t handle, st_value_t buffer, size_t offset, size_t count,
    uint32_t timeout, size_t *transferred_out, int *error_out);

#endif
