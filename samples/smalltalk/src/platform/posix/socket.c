#define _GNU_SOURCE 1
#include "../socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

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
    int descriptor;
    native_socket_t *sockets;
};

static st_socket_reactor_t *create_reactor(int *error_out)
{
    st_socket_reactor_t *reactor = calloc(1u, sizeof(*reactor));

    *error_out = 0;

    if (reactor == NULL) {
        *error_out = ENOMEM;
        return NULL;
    }

    reactor->descriptor = epoll_create1(EPOLL_CLOEXEC);

    if (reactor->descriptor < 0) {
        *error_out = errno;
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
    int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int enabled = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };

    *error_out = 0;

    if (descriptor < 0 || setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0
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
        descriptor = accept4(listener->descriptor, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);

    *error_out = descriptor < 0 ? errno : 0;
    return descriptor < 0 ? ST_SOCKET_INVALID : adopt(listener->reactor, descriptor, error_out);
}

static bool update_watch(native_socket_t *socket, int *error_out)
{
    uint32_t events = EPOLLONESHOT;

    if (socket->reader != NULL)
        events |= EPOLLIN;

    if (socket->writer != NULL)
        events |= EPOLLOUT;

    if (socket->reader == NULL && socket->writer == NULL) {
        if (socket->registered)
            epoll_ctl(socket->reactor->descriptor, EPOLL_CTL_DEL, socket->descriptor, NULL);

        socket->registered = false;
        return true;
    }

    struct epoll_event event = { .events = events, .data = { .ptr = socket } };
    int operation = socket->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

    if (epoll_ctl(socket->reactor->descriptor, operation, socket->descriptor, &event) != 0) {
        *error_out = errno;
        return false;
    }

    socket->registered = true;
    return true;
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

    close(reactor->descriptor);
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
        result = send(socket->descriptor, bytes, count > INT_MAX ? INT_MAX : count, MSG_NOSIGNAL);
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
    struct epoll_event events[128];
    size_t limit = capacity / 2u;

    *error_out = 0;

    if (limit == 0u) {
        *error_out = EINVAL;
        return -1;
    }

    if (limit > 128u)
        limit = 128u;

    int count = epoll_wait(reactor->descriptor, events, (int)limit, timeout);

    if (count < 0) {
        *error_out = errno == EINTR ? 0 : errno;
        return errno == EINTR ? 0 : -1;
    }

    size_t emitted = 0u;

    for (int index = 0; index < count; index++) {
        native_socket_t *socket = events[index].data.ptr;
        uint32_t flags = events[index].events;

        if (socket->reader != NULL && (flags & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0u) {
            tokens[emitted++] = socket->reader;
            socket->reader = NULL;
        }

        if (socket->writer != NULL && (flags & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0u) {
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
