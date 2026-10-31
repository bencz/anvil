#ifndef ANVIL_SMALLTALK_POSIX_SOCKET_REACTOR_H
#define ANVIL_SMALLTALK_POSIX_SOCKET_REACTOR_H

#include "../socket.h"

/* State shared by the POSIX socket layer (socket.c) and its readiness
 * backend.  Exactly one backend defines posix_reactor for the host:
 * reactor_epoll.c on Linux, reactor_poll.c on any other POSIX system. */

typedef struct native_socket {
    struct native_socket *next;
    struct native_socket **previous_link;
    st_socket_reactor_t *reactor;
    int descriptor;
    void *reader;
    void *writer;
    bool registered;
} native_socket_t;

struct st_socket_reactor {
    native_socket_t *sockets;
    int descriptor;  /* the backend's kernel object, if it has one */
    void *state;     /* the backend's private state, if any */
};

enum {
    POSIX_READY_IN = 1u,
    POSIX_READY_OUT = 2u,
    POSIX_READY_ERROR = 4u  /* error or hang-up: wakes both waiters */
};

typedef struct {
    native_socket_t *socket;
    uint32_t ready;
} posix_ready_t;

typedef struct {
    bool (*open)(st_socket_reactor_t *reactor, int *error_out);
    void (*close)(st_socket_reactor_t *reactor);
    /* Interest follows socket->reader / socket->writer; with neither the
     * socket leaves the backend.  A delivered readiness disarms the socket
     * until the next update. */
    bool (*update)(native_socket_t *socket, int *error_out);
    /* Up to capacity ready sockets; 0 on timeout or interruption, negative
     * on error. */
    int (*wait)(st_socket_reactor_t *reactor, posix_ready_t *ready, size_t capacity, int timeout, int *error_out);
} posix_reactor_ops_t;

extern const posix_reactor_ops_t posix_reactor;

#endif
