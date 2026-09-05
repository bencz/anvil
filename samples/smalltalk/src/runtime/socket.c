#include "st_socket.h"
#include "st_image_runtime.h"
#include "fiber_io.h"
#include "../platform/fiber.h"

#include <stdlib.h>

typedef struct {
    uintptr_t native;
    uint32_t generation;
    uint32_t next_free;
    bool reading;
    bool writing;
    bool listener;
} socket_entry_t;

struct st_socket_context {
    st_aot_thread_t *owner;
    socket_entry_t **entries;
    size_t count;
    size_t capacity;
    uint32_t free_head;
    uintptr_t owner_id;
};

static st_socket_status_t context_for_frame(StFrame *frame, st_socket_context_t **context_out)
{
    if (st_fiber_io_validate_frame(frame) != ST_FIBER_OK)
        return ST_SOCKET_INVALID_FRAME;

    st_aot_thread_t *thread = frame->thread;
    st_socket_context_t *context = thread->sockets;

    if (context == NULL || thread->fibers != context->owner->fibers || thread->image != context->owner->image)
        return ST_SOCKET_INVALID_FRAME;

    *context_out = context;
    return ST_SOCKET_OK;
}

static socket_entry_t *find_entry(st_socket_context_t *context, uint64_t handle)
{
    uint32_t index = (uint32_t)handle;
    uint32_t generation = (uint32_t)(handle >> 32u);

    if (handle > (uint64_t)ST_SMALL_INTEGER_MAX || index == 0u || index > context->count)
        return NULL;

    socket_entry_t *entry = context->entries[index - 1u];

    return entry->native != ST_SOCKET_INVALID && entry->generation == generation ? entry : NULL;
}

static st_socket_status_t publish(st_socket_context_t *context, uintptr_t native, bool listener, uint64_t *handle_out)
{
    socket_entry_t *entry;
    uint32_t index;

    if (context->free_head != 0u) {
        index = context->free_head;
        entry = context->entries[index - 1u];
        context->free_head = entry->next_free;
    } else {
        if (context->count == UINT32_MAX)
            return ST_SOCKET_OUT_OF_MEMORY;

        if (context->count == context->capacity) {
            size_t capacity = context->capacity == 0u ? 32u : context->capacity * 2u;

            if (capacity < context->capacity || capacity > SIZE_MAX / sizeof(*context->entries))
                return ST_SOCKET_OUT_OF_MEMORY;

            socket_entry_t **entries = realloc(context->entries, capacity * sizeof(*entries));

            if (entries == NULL)
                return ST_SOCKET_OUT_OF_MEMORY;

            context->entries = entries;
            context->capacity = capacity;
        }

        entry = calloc(1u, sizeof(*entry));

        if (entry == NULL)
            return ST_SOCKET_OUT_OF_MEMORY;

        entry->generation = 1u;
        index = (uint32_t)++context->count;
        context->entries[index - 1u] = entry;
    }

    entry->native = native;
    entry->listener = listener;
    entry->next_free = 0u;
    *handle_out = ((uint64_t)entry->generation << 32u) | index;
    return ST_SOCKET_OK;
}

st_socket_status_t st_socket_context_create(st_aot_thread_t *owner, st_socket_context_t **context_out)
{
    if (context_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *context_out = NULL;

    if (owner == NULL || !owner->initialized || owner->fibers == NULL || owner->sockets != NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    st_socket_context_t *context = calloc(1u, sizeof(*context));

    if (context == NULL)
        return ST_SOCKET_OUT_OF_MEMORY;

    context->owner = owner;
    context->owner_id = st_fiber_platform.thread_id();
    owner->sockets = context;
    *context_out = context;
    return ST_SOCKET_OK;
}

st_socket_status_t st_socket_listen(StFrame *frame, uint16_t port, uint64_t *handle_out, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (handle_out == NULL || error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *handle_out = 0u;
    *error_out = 0;

    if (status != ST_SOCKET_OK)
        return status;

    st_socket_reactor_t *reactor;

    if (st_fiber_io_prepare(frame, &reactor) != ST_FIBER_OK)
        return ST_SOCKET_RUNTIME_ERROR;

    uintptr_t native = st_socket_platform.listen(reactor, port, error_out);

    if (native == ST_SOCKET_INVALID) {
        status = ST_SOCKET_OS_ERROR;
    } else {
        status = publish(context, native, true, handle_out);

        if (status != ST_SOCKET_OK) {
            int close_error;

            st_socket_platform.close(native, &close_error);
        }
    }

    st_fiber_io_finish(frame);
    return status;
}

static st_socket_status_t suspend_operation(StFrame *frame, uintptr_t native, uint32_t events, uint64_t deadline, int *error_out)
{
    st_fiber_status_t status = st_fiber_io_wait(frame, native, events, deadline, error_out);

    if (status == ST_FIBER_OK)
        return ST_SOCKET_OK;

    /* Cancellation is an asynchronous request on IOCP. Keep the activation
     * and its managed buffer alive until the completion packet is consumed. */
    int original_error = *error_out;

    for (;;) {
        int cancel_error;

        if (st_socket_platform.cancel(native, events, &cancel_error))
            break;

        if (!st_socket_platform.would_block(cancel_error))
            abort();

        if (st_fiber_io_wait(frame, native, events, UINT64_MAX, &cancel_error) != ST_FIBER_OK)
            abort();
    }

    *error_out = original_error;

    if (status == ST_FIBER_INTERRUPTED)
        return ST_SOCKET_INTERRUPTED;

    return status == ST_FIBER_TIMEOUT ? ST_SOCKET_TIMEOUT : ST_SOCKET_RUNTIME_ERROR;
}

st_socket_status_t st_socket_accept(StFrame *frame, uint64_t listener, uint32_t timeout, uint64_t *handle_out, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (handle_out == NULL || error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *handle_out = 0u;
    *error_out = 0;

    if (status != ST_SOCKET_OK)
        return status;

    socket_entry_t *entry = find_entry(context, listener);

    if (entry == NULL || !entry->listener)
        return ST_SOCKET_INVALID_HANDLE;

    if (entry->reading)
        return ST_SOCKET_BUSY;

    st_socket_reactor_t *reactor;

    if (st_fiber_io_prepare(frame, &reactor) != ST_FIBER_OK)
        return ST_SOCKET_RUNTIME_ERROR;

    uint64_t deadline = st_fiber_milliseconds() + timeout;

    entry->reading = true;

    for (;;) {
        uintptr_t native = st_socket_platform.accept(entry->native, error_out);

        if (native != ST_SOCKET_INVALID) {
            status = publish(context, native, false, handle_out);

            if (status != ST_SOCKET_OK) {
                int close_error;

                st_socket_platform.close(native, &close_error);
            }

            break;
        }

        if (!st_socket_platform.would_block(*error_out)) {
            status = ST_SOCKET_OS_ERROR;
            break;
        }

        status = suspend_operation(frame, entry->native, ST_SOCKET_READABLE, deadline, error_out);

        if (status != ST_SOCKET_OK)
            break;
    }

    entry->reading = false;
    st_fiber_io_finish(frame);
    return status;
}

static st_socket_status_t transfer(StFrame *frame, uint64_t handle, st_value_t buffer, size_t offset, size_t count,
    uint32_t timeout, bool writing, size_t *transferred_out, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (transferred_out == NULL || error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *transferred_out = 0u;
    *error_out = 0;

    if (status != ST_SOCKET_OK)
        return status;

    socket_entry_t *entry = find_entry(context, handle);

    if (entry == NULL || entry->listener)
        return ST_SOCKET_INVALID_HANDLE;

    bool *busy = writing ? &entry->writing : &entry->reading;

    if (*busy)
        return ST_SOCKET_BUSY;

    st_object_view_t view;

    if (st_heap_object_view(context->owner->image->heap, buffer, &view) != ST_HEAP_OK
            || view.shape_descriptor->indexed_format != ST_INDEXED_UINT8 || view.shape_descriptor->fixed_word_count != 0u
            || offset > view.indexed_length || count > view.indexed_length - offset
            || (!writing && (st_object_header_flags(st_object_header_load(&view.object->header)) & ST_HEADER_IMMUTABLE) != 0u))
        return ST_SOCKET_INVALID_BUFFER;

    if (count == 0u)
        return ST_SOCKET_OK;

    st_socket_reactor_t *reactor;

    if (st_fiber_io_prepare(frame, &reactor) != ST_FIBER_OK)
        return ST_SOCKET_RUNTIME_ERROR;

    unsigned char *bytes = (unsigned char *)view.indexed_elements + offset;
    uint64_t deadline = st_fiber_milliseconds() + timeout;

    *busy = true;

    for (;;) {
        int64_t transferred = writing
            ? st_socket_platform.send(entry->native, bytes, count, error_out)
            : st_socket_platform.receive(entry->native, bytes, count, error_out);

        if (transferred >= 0) {
            *transferred_out = (size_t)transferred;
            status = ST_SOCKET_OK;
            break;
        }

        if (!st_socket_platform.would_block(*error_out)) {
            status = ST_SOCKET_OS_ERROR;
            break;
        }

        status = suspend_operation(frame, entry->native, writing ? ST_SOCKET_WRITABLE : ST_SOCKET_READABLE, deadline, error_out);

        if (status != ST_SOCKET_OK)
            break;
    }

    *busy = false;
    st_fiber_io_finish(frame);
    return status;
}

st_socket_status_t st_socket_receive(StFrame *frame, uint64_t handle, st_value_t buffer, size_t offset, size_t count,
    uint32_t timeout, size_t *transferred_out, int *error_out)
{
    return transfer(frame, handle, buffer, offset, count, timeout, false, transferred_out, error_out);
}

st_socket_status_t st_socket_send(StFrame *frame, uint64_t handle, st_value_t buffer, size_t offset, size_t count,
    uint32_t timeout, size_t *transferred_out, int *error_out)
{
    return transfer(frame, handle, buffer, offset, count, timeout, true, transferred_out, error_out);
}

st_socket_status_t st_socket_port(StFrame *frame, uint64_t handle, uint16_t *port_out, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (port_out == NULL || error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *port_out = 0u;
    *error_out = 0;

    if (status != ST_SOCKET_OK)
        return status;

    socket_entry_t *entry = find_entry(context, handle);

    if (entry == NULL)
        return ST_SOCKET_INVALID_HANDLE;

    *port_out = st_socket_platform.port(entry->native, error_out);
    return *error_out == 0 ? ST_SOCKET_OK : ST_SOCKET_OS_ERROR;
}

st_socket_status_t st_socket_close(StFrame *frame, uint64_t handle, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *error_out = 0;
    if (status != ST_SOCKET_OK)
        return status;

    socket_entry_t *entry = find_entry(context, handle);

    if (entry == NULL)
        return ST_SOCKET_INVALID_HANDLE;

    if (entry->reading || entry->writing)
        return ST_SOCKET_BUSY;

    if (!st_socket_platform.close(entry->native, error_out))
        return ST_SOCKET_OS_ERROR;

    entry->native = ST_SOCKET_INVALID;

    if (entry->generation < UINT32_C(0x0fffffff)) {
        entry->generation++;
        entry->next_free = context->free_head;
        context->free_head = (uint32_t)handle;
    }

    return *error_out == 0 ? ST_SOCKET_OK : ST_SOCKET_OS_ERROR;
}

st_socket_status_t st_socket_interrupt(StFrame *frame, uint64_t handle, int *error_out)
{
    st_socket_context_t *context;
    st_socket_status_t status = context_for_frame(frame, &context);

    if (error_out == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    *error_out = 0;

    if (status != ST_SOCKET_OK)
        return status;

    socket_entry_t *entry = find_entry(context, handle);

    if (entry == NULL)
        return ST_SOCKET_INVALID_HANDLE;

    st_fiber_io_interrupt(frame, entry->native);
    return ST_SOCKET_OK;
}

st_socket_status_t st_socket_context_destroy(st_socket_context_t *context)
{
    if (context == NULL)
        return ST_SOCKET_INVALID_ARGUMENT;

    if (context->owner_id != st_fiber_platform.thread_id())
        return ST_SOCKET_INVALID_FRAME;

    for (size_t index = 0u; index < context->count; index++) {
        if (context->entries[index]->reading || context->entries[index]->writing)
            return ST_SOCKET_BUSY;
    }

    for (size_t index = 0u; index < context->count; index++) {
        socket_entry_t *entry = context->entries[index];
        int error;

        if (entry->native != ST_SOCKET_INVALID && !st_socket_platform.close(entry->native, &error))
            return ST_SOCKET_OS_ERROR;

        entry->native = ST_SOCKET_INVALID;
    }

    for (size_t index = 0u; index < context->count; index++)
        free(context->entries[index]);

    context->owner->sockets = NULL;
    free(context->entries);
    free(context);
    return ST_SOCKET_OK;
}
