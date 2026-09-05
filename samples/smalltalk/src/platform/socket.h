#ifndef ANVIL_SMALLTALK_PLATFORM_SOCKET_H
#define ANVIL_SMALLTALK_PLATFORM_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST_SOCKET_INVALID UINTPTR_MAX

enum { ST_SOCKET_READABLE = 1u, ST_SOCKET_WRITABLE = 2u };

typedef struct st_socket_reactor st_socket_reactor_t;

/* Every socket is nonblocking and noninheritable. Counts are bytes, zero from
 * receive is orderly EOF, negative is an OS error. Pending operations borrow
 * buffers until completion, including after cancellation. watch attaches a
 * stable waiter token; wait returns only completed/ready tokens, never scans
 * idle connections. One reader and one writer may wait on each socket.
 * close returns true when ownership was released, even if error_out reports
 * a delayed close error; false leaves ownership with the caller. */
typedef struct {
    st_socket_reactor_t *(*create)(int *error_out);
    bool (*destroy)(st_socket_reactor_t *reactor, int *error_out);
    uintptr_t (*listen)(st_socket_reactor_t *reactor, uint16_t port, int *error_out);
    uintptr_t (*accept)(uintptr_t listener, int *error_out);
    bool (*close)(uintptr_t socket, int *error_out);
    uint16_t (*port)(uintptr_t socket, int *error_out);
    int64_t (*receive)(uintptr_t socket, void *bytes, size_t count, int *error_out);
    int64_t (*send)(uintptr_t socket, const void *bytes, size_t count, int *error_out);
    bool (*would_block)(int error);
    bool (*watch)(uintptr_t socket, uint32_t events, void *token, int *error_out);
    bool (*cancel)(uintptr_t socket, uint32_t events, int *error_out);
    int (*wait)(st_socket_reactor_t *reactor, void **tokens, size_t capacity, int timeout, int *error_out);
} st_socket_platform_ops_t;

extern const st_socket_platform_ops_t st_socket_platform;

#endif
