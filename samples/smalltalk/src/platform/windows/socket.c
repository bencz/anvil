#include "../socket.h"

#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum { OP_IDLE, OP_PENDING, OP_COMPLETE } operation_state_t;
typedef struct native_socket native_socket_t;

typedef struct {
    OVERLAPPED overlapped;
    native_socket_t *owner;
    void *token;
    void *buffer;
    size_t capacity;
    DWORD transferred;
    int error;
    operation_state_t state;
} socket_operation_t;

struct native_socket {
    native_socket_t *next;
    native_socket_t **previous_link;
    st_socket_reactor_t *reactor;
    SOCKET handle;
    SOCKET accepted;
    LPFN_ACCEPTEX accept_ex;
    socket_operation_t read;
    socket_operation_t write;
    unsigned char addresses[2u * (sizeof(struct sockaddr_in) + 16u)];
};

struct st_socket_reactor {
    HANDLE port;
    native_socket_t *sockets;
};

static st_socket_reactor_t *create_reactor(int *error_out)
{
    WSADATA data;

    *error_out = WSAStartup(MAKEWORD(2, 2), &data);
    if (*error_out != 0)
        return NULL;

    st_socket_reactor_t *reactor = calloc(1u, sizeof(*reactor));

    if (reactor == NULL) {
        *error_out = WSAENOBUFS;
        WSACleanup();
        return NULL;
    }

    reactor->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0u, 1u);

    if (reactor->port == NULL) {
        *error_out = (int)GetLastError();
        free(reactor);
        WSACleanup();
        return NULL;
    }

    return reactor;
}

static SOCKET create_socket(void)
{
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0u, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

static uintptr_t adopt(st_socket_reactor_t *reactor, SOCKET handle, int *error_out)
{
    native_socket_t *socket = calloc(1u, sizeof(*socket));

    if (socket == NULL) {
        *error_out = WSAENOBUFS;
        closesocket(handle);
        return ST_SOCKET_INVALID;
    }

    if (CreateIoCompletionPort((HANDLE)handle, reactor->port, 0u, 0u) == NULL) {
        *error_out = (int)GetLastError();
        closesocket(handle);
        free(socket);
        return ST_SOCKET_INVALID;
    }

    socket->reactor = reactor;
    socket->handle = handle;
    socket->accepted = INVALID_SOCKET;
    socket->read.owner = socket;
    socket->write.owner = socket;
    socket->next = reactor->sockets;
    socket->previous_link = &reactor->sockets;

    if (socket->next != NULL)
        socket->next->previous_link = &socket->next;

    reactor->sockets = socket;
    return (uintptr_t)socket;
}

static uintptr_t listen_loopback(st_socket_reactor_t *reactor, uint16_t port, int *error_out)
{
    SOCKET handle = create_socket();
    int enabled = 1;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port) };

    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    *error_out = 0;

    /* Ask the TCP provider for its largest supported pending-accept queue.
     * SOMAXCONN alone can select a small provider default under bursts. This
     * queue is independent of the dynamically allocated active connections. */
    if (handle == INVALID_SOCKET || setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&enabled, sizeof(enabled)) != 0
            || bind(handle, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(handle, SOMAXCONN_HINT(SOMAXCONN)) != 0) {
        *error_out = WSAGetLastError();

        if (handle != INVALID_SOCKET)
            closesocket(handle);

        return ST_SOCKET_INVALID;
    }

    return adopt(reactor, handle, error_out);
}

static void reset_operation(socket_operation_t *operation)
{
    native_socket_t *owner = operation->owner;

    memset(operation, 0, sizeof(*operation));
    operation->owner = owner;
}

static uintptr_t accept_connection(uintptr_t handle, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    socket_operation_t *operation = &socket->read;

    *error_out = 0;

    if (operation->state == OP_COMPLETE) {
        SOCKET accepted = socket->accepted;

        socket->accepted = INVALID_SOCKET;
        *error_out = operation->error;
        reset_operation(operation);

        if (*error_out == 0 && setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&socket->handle, sizeof(socket->handle)) != 0)
            *error_out = WSAGetLastError();

        if (*error_out != 0) {
            closesocket(accepted);
            return ST_SOCKET_INVALID;
        }

        return adopt(socket->reactor, accepted, error_out);
    }

    if (operation->state == OP_IDLE) {
        if (socket->accept_ex == NULL) {
            GUID identifier = WSAID_ACCEPTEX;
            DWORD bytes;

            if (WSAIoctl(socket->handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, sizeof(identifier), &socket->accept_ex,
                    sizeof(socket->accept_ex), &bytes, NULL, NULL) != 0) {
                *error_out = WSAGetLastError();
                return ST_SOCKET_INVALID;
            }
        }

        socket->accepted = create_socket();

        if (socket->accepted == INVALID_SOCKET) {
            *error_out = WSAGetLastError();
            return ST_SOCKET_INVALID;
        }

        operation->state = OP_PENDING;
        DWORD transferred;
        BOOL started = socket->accept_ex(socket->handle, socket->accepted, socket->addresses, 0u,
            sizeof(struct sockaddr_in) + 16u, sizeof(struct sockaddr_in) + 16u, &transferred, &operation->overlapped);

        if (!started && WSAGetLastError() != ERROR_IO_PENDING) {
            *error_out = WSAGetLastError();
            closesocket(socket->accepted);
            socket->accepted = INVALID_SOCKET;
            reset_operation(operation);
            return ST_SOCKET_INVALID;
        }
    }

    *error_out = WSAEWOULDBLOCK;
    return ST_SOCKET_INVALID;
}

static int64_t transfer_bytes(native_socket_t *socket, socket_operation_t *operation, void *bytes, size_t count, bool writing, int *error_out)
{
    *error_out = 0;

    if (operation->state != OP_IDLE && (operation->buffer != bytes || operation->capacity != count)) {
        *error_out = WSAEINVAL;
        return -1;
    }

    if (operation->state == OP_COMPLETE) {
        int64_t result = operation->error == 0 ? operation->transferred : -1;

        *error_out = operation->error;
        reset_operation(operation);
        return result;
    }

    if (operation->state == OP_IDLE) {
        WSABUF buffer = { .len = count > ULONG_MAX ? ULONG_MAX : (ULONG)count, .buf = bytes };
        DWORD flags = 0u;
        DWORD transferred;

        operation->buffer = bytes;
        operation->capacity = count;
        operation->state = OP_PENDING;

        int result = writing
            ? WSASend(socket->handle, &buffer, 1u, &transferred, 0u, &operation->overlapped, NULL)
            : WSARecv(socket->handle, &buffer, 1u, &transferred, &flags, &operation->overlapped, NULL);

        if (result != 0 && WSAGetLastError() != WSA_IO_PENDING) {
            *error_out = WSAGetLastError();
            reset_operation(operation);
            return -1;
        }
    }

    /* Even synchronous success queues an IOCP packet. The OVERLAPPED and
     * borrowed buffer remain live until that packet has been consumed. */
    *error_out = WSAEWOULDBLOCK;
    return -1;
}

static int64_t receive_bytes(uintptr_t handle, void *bytes, size_t count, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;

    return transfer_bytes(socket, &socket->read, bytes, count, false, error_out);
}

static int64_t send_bytes(uintptr_t handle, const void *bytes, size_t count, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;

    return transfer_bytes(socket, &socket->write, (void *)bytes, count, true, error_out);
}

static bool watch_socket(uintptr_t handle, uint32_t events, void *token, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    socket_operation_t *operation = events == ST_SOCKET_READABLE ? &socket->read : &socket->write;

    *error_out = 0;

    if (operation->state != OP_PENDING || (operation->token != NULL && operation->token != token)) {
        *error_out = WSAEINVAL;
        return false;
    }

    operation->token = token;
    return true;
}

static bool cancel_operation(uintptr_t handle, uint32_t events, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    socket_operation_t *operation = events == ST_SOCKET_READABLE ? &socket->read : &socket->write;

    *error_out = 0;

    if (operation->state == OP_PENDING) {
        if (!CancelIoEx((HANDLE)socket->handle, &operation->overlapped) && GetLastError() != ERROR_NOT_FOUND) {
            *error_out = (int)GetLastError();
            return false;
        }

        *error_out = WSAEWOULDBLOCK;
        return false;
    }

    if (events == ST_SOCKET_READABLE && socket->accepted != INVALID_SOCKET) {
        closesocket(socket->accepted);
        socket->accepted = INVALID_SOCKET;
    }

    reset_operation(operation);
    return true;
}

static bool close_socket(uintptr_t handle, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;

    *error_out = 0;

    if (socket->read.state != OP_IDLE || socket->write.state != OP_IDLE) {
        *error_out = WSAEINPROGRESS;
        return false;
    }

    if (closesocket(socket->handle) != 0) {
        *error_out = WSAGetLastError();
        return false;
    }

    *socket->previous_link = socket->next;

    if (socket->next != NULL)
        socket->next->previous_link = socket->previous_link;

    free(socket);
    return true;
}

static bool destroy_reactor(st_socket_reactor_t *reactor, int *error_out)
{
    *error_out = 0;

    if (reactor->sockets != NULL) {
        *error_out = WSAEINPROGRESS;
        return false;
    }

    CloseHandle(reactor->port);
    free(reactor);
    WSACleanup();
    return true;
}

static uint16_t local_port(uintptr_t handle, int *error_out)
{
    native_socket_t *socket = (native_socket_t *)handle;
    struct sockaddr_in address;
    int length = sizeof(address);

    *error_out = 0;

    if (getsockname(socket->handle, (struct sockaddr *)&address, &length) != 0) {
        *error_out = WSAGetLastError();
        return 0u;
    }

    return ntohs(address.sin_port);
}

static bool would_block(int error)
{
    return error == WSAEWOULDBLOCK;
}

static int wait_events(st_socket_reactor_t *reactor, void **tokens, size_t capacity, int timeout, int *error_out)
{
    OVERLAPPED_ENTRY entries[128];
    ULONG limit = capacity > 128u ? 128u : (ULONG)capacity;
    ULONG count = 0u;

    *error_out = 0;

    if (!GetQueuedCompletionStatusEx(reactor->port, entries, limit, &count, timeout < 0 ? INFINITE : (DWORD)timeout, FALSE)) {
        DWORD error = GetLastError();

        *error_out = error == WAIT_TIMEOUT ? 0 : (int)error;
        return error == WAIT_TIMEOUT ? 0 : -1;
    }

    size_t emitted = 0u;

    for (ULONG index = 0u; index < count; index++) {
        socket_operation_t *operation = (socket_operation_t *)entries[index].lpOverlapped;
        DWORD flags = 0u;

        if (!WSAGetOverlappedResult(operation->owner->handle, &operation->overlapped, &operation->transferred, FALSE, &flags))
            operation->error = WSAGetLastError();

        operation->state = OP_COMPLETE;

        if (operation->token != NULL) {
            tokens[emitted++] = operation->token;
            operation->token = NULL;
        }
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
    .cancel = cancel_operation,
    .wait = wait_events
};
