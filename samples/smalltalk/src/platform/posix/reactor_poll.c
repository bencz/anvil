/* Portable POSIX readiness backend (macOS, the BSDs, others): poll(2) over
 * the sockets that currently have a waiter.  Idle sockets are not polled. */
#include "socket_reactor.h"

#if !defined(__linux__)

#include <errno.h>
#include <poll.h>
#include <stdlib.h>

typedef struct {
    struct pollfd *descriptors;
    native_socket_t **sockets;
    size_t capacity;
} poll_state_t;

static bool open_reactor(st_socket_reactor_t *reactor, int *error_out)
{
    reactor->descriptor = -1;
    reactor->state = calloc(1u, sizeof(poll_state_t));
    *error_out = reactor->state == NULL ? ENOMEM : 0;
    return reactor->state != NULL;
}

static void close_reactor(st_socket_reactor_t *reactor)
{
    poll_state_t *state = reactor->state;

    free(state->descriptors);
    free(state->sockets);
    free(state);
}

static bool update_socket(native_socket_t *socket, int *error_out)
{
    /* Interest is read from reader/writer at every wait. */
    *error_out = 0;
    socket->registered = socket->reader != NULL || socket->writer != NULL;
    return true;
}

static bool reserve(poll_state_t *state, size_t count, int *error_out)
{
    if (count <= state->capacity)
        return true;

    size_t capacity = state->capacity != 0u ? state->capacity * 2u : 16u;

    while (capacity < count)
        capacity *= 2u;

    struct pollfd *descriptors = realloc(state->descriptors, capacity * sizeof(*descriptors));

    if (descriptors == NULL) {
        *error_out = ENOMEM;
        return false;
    }

    state->descriptors = descriptors;

    native_socket_t **sockets = realloc(state->sockets, capacity * sizeof(*sockets));

    if (sockets == NULL) {
        *error_out = ENOMEM;
        return false;
    }

    state->sockets = sockets;
    state->capacity = capacity;
    return true;
}

static int wait_ready(st_socket_reactor_t *reactor, posix_ready_t *ready, size_t capacity, int timeout, int *error_out)
{
    poll_state_t *state = reactor->state;
    size_t watched = 0u;

    *error_out = 0;

    for (native_socket_t *socket = reactor->sockets; socket != NULL; socket = socket->next) {
        if (socket->reader != NULL || socket->writer != NULL)
            watched++;
    }

    if (!reserve(state, watched, error_out))
        return -1;

    size_t count = 0u;

    for (native_socket_t *socket = reactor->sockets; socket != NULL; socket = socket->next) {
        if (socket->reader == NULL && socket->writer == NULL)
            continue;

        state->descriptors[count].fd = socket->descriptor;
        state->descriptors[count].events = (short)((socket->reader != NULL ? POLLIN : 0) | (socket->writer != NULL ? POLLOUT : 0));
        state->descriptors[count].revents = 0;
        state->sockets[count++] = socket;
    }

    int result = poll(state->descriptors, (nfds_t)count, timeout);

    if (result < 0) {
        *error_out = errno == EINTR ? 0 : errno;
        return errno == EINTR ? 0 : -1;
    }

    /* Level-triggered: sockets beyond capacity are reported next time. */
    size_t emitted = 0u;

    for (size_t index = 0u; index < count && emitted < capacity; index++) {
        short flags = state->descriptors[index].revents;

        if (flags == 0)
            continue;

        ready[emitted].socket = state->sockets[index];
        ready[emitted].ready = ((flags & POLLIN) != 0 ? POSIX_READY_IN : 0u) | ((flags & POLLOUT) != 0 ? POSIX_READY_OUT : 0u) |
                               ((flags & (POLLERR | POLLHUP | POLLNVAL)) != 0 ? POSIX_READY_ERROR : 0u);
        emitted++;
    }

    return (int)emitted;
}

const posix_reactor_ops_t posix_reactor = {
    .open = open_reactor,
    .close = close_reactor,
    .update = update_socket,
    .wait = wait_ready
};

#else

typedef int posix_reactor_poll_unused_t;

#endif
