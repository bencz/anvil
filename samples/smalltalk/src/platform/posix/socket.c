#define _GNU_SOURCE 1
#include "socket_reactor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* Readiness comes from posix_reactor (socket_reactor.h).  Where the host
 * lacks the atomic SOCK_NONBLOCK/SOCK_CLOEXEC flags and accept4 (macOS),
 * descriptors are configured with fcntl; where it lacks MSG_NOSIGNAL,
 * SO_NOSIGPIPE keeps a closed peer from raising SIGPIPE. */
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
#define ST_ATOMIC_SOCKET_FLAGS (SOCK_NONBLOCK | SOCK_CLOEXEC)
#else
#define ST_ATOMIC_SOCKET_FLAGS 0
#endif

#if defined(MSG_NOSIGNAL)
#define ST_SEND_FLAGS MSG_NOSIGNAL
#else
#define ST_SEND_FLAGS 0
#endif

static bool configure_descriptor(int descriptor)
{
#if !(defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC))
    int status = fcntl(descriptor, F_GETFL);
    int flags = fcntl(descriptor, F_GETFD);

    if (status < 0 || flags < 0 || fcntl(descriptor, F_SETFL, status | O_NONBLOCK) != 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
        return false;
#endif
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int enabled = 1;

    if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return false;
#endif
    (void)descriptor;
    return true;
}

static st_socket_reactor_t *create_reactor(int *error_out)
{
    st_socket_reactor_t *reactor = calloc(1u, sizeof(*reactor));

    *error_out = 0;

    if (reactor == NULL) {
        *error_out = ENOMEM;
        return NULL;
    }

    if (!posix_reactor.open(reactor, error_out)) {
        free(reactor);
        return NULL;
    }

    return reactor;
}

static uintptr_t adopt(st_socket_reactor_t *reactor, int descriptor, int *error_out)
{
    native_socket_t *socket = calloc(1u, sizeof(*socket));

    if (socket == NULL) {
        close(descriptor);
        *error_out = ENOMEM;
        return ST_SOCKET_INVALID;
    }

    socket->reactor = reactor;
    socket->descriptor = descriptor;
    socket->next = reactor->sockets;
    socket->previous_link = &reactor->sockets;

    if (socket->next != NULL)
        socket->next->previous_link = &socket->next;

    reactor->sockets = socket;
    return (uintptr_t)socket;
}

static uintptr_t listen_loopback(st_socket_reactor_t *reactor, uint16_t port, int *error_out)
{
    int descriptor = socket(AF_INET, SOCK_STREAM | ST_ATOMIC_SOCKET_FLAGS, 0);
    int enabled = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };

    *error_out = 0;

    if (descriptor < 0 || !configure_descriptor(descriptor) || setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0
            || bind(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(descriptor, SOMAXCONN) != 0) {
        *error_out = errno;

        if (descriptor >= 0)
            close(descriptor);

        return ST_SOCKET_INVALID;
    }

    return adopt(reactor, descriptor, error_out);
}

static uintptr_t accept_connection(uintptr_t handle, int *error_out)
{
    native_socket_t *listener = (native_socket_t *)handle;
    int descriptor;

    do {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
        descriptor = accept4(listener->descriptor, NULL, NULL, ST_ATOMIC_SOCKET_FLAGS);
#else
        descriptor = accept(listener->descriptor, NULL, NULL);
#endif
    } while (descriptor < 0 && errno == EINTR);

    if (descriptor >= 0 && !configure_descriptor(descriptor)) {
        *error_out = errno;
        close(descriptor);
        return ST_SOCKET_INVALID;
    }

    *error_out = descriptor < 0 ? errno : 0;
    return descriptor < 0 ? ST_SOCKET_INVALID : adopt(listener->reactor, descriptor, error_out);
}

static bool update_watch(native_socket_t *socket, int *error_out)
{
    return posix_reactor.update(socket, error_out);
}

static bool watch_socket(uintptr_t handle, uint32_t events, void *token, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    void **slot = events == ST_SOCKET_READABLE ? &socket->reader : &socket->writer;

    *error_out = 0;

    if (*slot != NULL && *slot != token) {
        *error_out = EBUSY;
        return false;
    }

    *slot = token;
    if (!update_watch(socket, error_out)) {
        *slot = NULL;
        return false;
    }

    return true;
}

static bool cancel_wait(uintptr_t handle, uint32_t events, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;

    *error_out = 0;

    if (events == ST_SOCKET_READABLE)
        socket->reader = NULL;
    else
        socket->writer = NULL;

    return update_watch(socket, error_out);
}

static bool close_socket(uintptr_t handle, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;

    *error_out = 0;

    if (socket->reader != NULL || socket->writer != NULL) {
        *error_out = EBUSY;
        return false;
    }

    int result = close(socket->descriptor);

    *error_out = result == 0 || errno == EINTR ? 0 : errno;
    *socket->previous_link = socket->next;

    if (socket->next != NULL)
        socket->next->previous_link = socket->previous_link;

    free(socket);
    /* Linux releases the descriptor even when close reports a delayed error.
     * Report ownership as released so callers never retry a recycled fd. */
    return true;
}

static bool destroy_reactor(st_socket_reactor_t *reactor, int *error_out)
{
    *error_out = 0;

    if (reactor->sockets != NULL) {
        *error_out = EBUSY;
        return false;
    }

    posix_reactor.close(reactor);
    free(reactor);
    return true;
}

static uint16_t local_port(uintptr_t handle, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);

    *error_out = 0;

    if (getsockname(socket->descriptor, (struct sockaddr *)&address, &length) != 0) {
        *error_out = errno;
        return 0u;
    }

    return ntohs(address.sin_port);
}

static int64_t receive_bytes(uintptr_t handle, void *bytes, size_t count, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    ssize_t result;

    do {
        result = recv(socket->descriptor, bytes, count > INT_MAX ? INT_MAX : count, 0);
    } while (result < 0 && errno == EINTR);

    *error_out = result < 0 ? errno : 0;
    return result;
}

static int64_t send_bytes(uintptr_t handle, const void *bytes, size_t count, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    ssize_t result;

    do {
        result = send(socket->descriptor, bytes, count > INT_MAX ? INT_MAX : count, ST_SEND_FLAGS);
    } while (result < 0 && errno == EINTR);

    *error_out = result < 0 ? errno : 0;
    return result;
}

static bool would_block(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

static int wait_events(st_socket_reactor_t *reactor, void **tokens, size_t capacity, int timeout, int *error_out)
{
    posix_ready_t ready[128];
    size_t limit = capacity / 2u;

    *error_out = 0;

    if (limit == 0u) {
        *error_out = EINVAL;
        return -1;
    }

    if (limit > 128u)
        limit = 128u;

    int count = posix_reactor.wait(reactor, ready, limit, timeout, error_out);

    if (count <= 0)
        return count;

    size_t emitted = 0u;

    for (int index = 0; index < count; index++) {
        native_socket_t *socket = ready[index].socket;
        uint32_t flags = ready[index].ready;

        if (socket->reader != NULL && (flags & (POSIX_READY_IN | POSIX_READY_ERROR)) != 0u) {
            tokens[emitted++] = socket->reader;
            socket->reader = NULL;
        }

        if (socket->writer != NULL && (flags & (POSIX_READY_OUT | POSIX_READY_ERROR)) != 0u) {
            tokens[emitted++] = socket->writer;
            socket->writer = NULL;
        }

        if (!update_watch(socket, error_out))
            return -1;
    }

    return (int)emitted;
}

const st_socket_platform_ops_t st_socket_platform = {
    .create = create_reactor,
    .destroy = destroy_reactor,
    .listen = listen_loopback,
    .accept = accept_connection,
    .close = close_socket,
    .port = local_port,
    .receive = receive_bytes,
    .send = send_bytes,
    .would_block = would_block,
    .watch = watch_socket,
    .cancel = cancel_wait,
    .wait = wait_events
};
