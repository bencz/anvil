/* Linux readiness backend: one-shot epoll registrations. */
#include "socket_reactor.h"

#if defined(__linux__)

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

static bool open_reactor(st_socket_reactor_t *reactor, int *error_out)
{
    reactor->descriptor = epoll_create1(EPOLL_CLOEXEC);
    *error_out = reactor->descriptor < 0 ? errno : 0;
    return reactor->descriptor >= 0;
}

static void close_reactor(st_socket_reactor_t *reactor)
{
    close(reactor->descriptor);
}

static bool update_socket(native_socket_t *socket, int *error_out)
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

static int wait_ready(st_socket_reactor_t *reactor, posix_ready_t *ready, size_t capacity, int timeout, int *error_out)
{
    struct epoll_event events[128];
    size_t limit = capacity < 128u ? capacity : 128u;
    int count = epoll_wait(reactor->descriptor, events, (int)limit, timeout);

    *error_out = 0;

    if (count < 0) {
        *error_out = errno == EINTR ? 0 : errno;
        return errno == EINTR ? 0 : -1;
    }

    for (int index = 0; index < count; index++) {
        uint32_t flags = events[index].events;

        ready[index].socket = events[index].data.ptr;
        ready[index].ready = ((flags & EPOLLIN) != 0u ? POSIX_READY_IN : 0u) | ((flags & EPOLLOUT) != 0u ? POSIX_READY_OUT : 0u) |
                             ((flags & (EPOLLERR | EPOLLHUP)) != 0u ? POSIX_READY_ERROR : 0u);
    }

    return count;
}

const posix_reactor_ops_t posix_reactor = {
    .open = open_reactor,
    .close = close_reactor,
    .update = update_socket,
    .wait = wait_ready
};

#else

typedef int posix_reactor_epoll_unused_t;

#endif
