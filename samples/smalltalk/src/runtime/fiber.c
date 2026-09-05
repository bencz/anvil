#include "st_fiber.h"

#include "st_control_roots.h"
#include "st_dnu.h"
#include "st_image_runtime.h"
#include "../platform/fiber.h"
#include "fiber_io.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_SLEEPING,
    FIBER_JOINING,
    FIBER_IO,
    FIBER_FINISHED
} fiber_state_t;

typedef struct fiber_record {
    struct fiber_record *next;
    struct fiber_record **previous_link;
    struct fiber_record *ready_next;
    struct fiber_record *waiting_for;
    struct fiber_record *join_waiters;
    struct fiber_record *join_next;
    st_fiber_context_t *context;
    st_native_fiber_t *native;
    st_aot_thread_t thread_storage;
    st_aot_thread_t *thread;
    st_control_thread_t control;
    st_dnu_context_t dnu;
    st_image_root_provider_t provider;
    st_value_t *roots;
    size_t root_count;
    size_t root_capacity;
    uint64_t wake_at;
    size_t timer_index;
    uint64_t id;
    uint32_t exception_class;
    fiber_state_t state;
    st_fiber_status_t status;
    bool consumed;
    bool detached;
    size_t join_references;
    bool io_ready;
    bool io_interrupted;
    uintptr_t io_socket;
} fiber_record_t;

struct st_fiber_context {
    fiber_record_t main;
    fiber_record_t *records;
    fiber_record_t *current;
    fiber_record_t *ready_head;
    fiber_record_t *ready_tail;
    uintptr_t owner_id;
    size_t stack_size;
    uint64_t next_id;
    fiber_record_t **timers;
    size_t timer_count;
    size_t timer_capacity;
    size_t io_count;
    st_socket_reactor_t *reactor;
    bool shutting_down;
};

static st_image_runtime_status_t fiber_roots(void *owner, const st_value_t **roots_out, size_t *count_out)
{
    const fiber_record_t *fiber = owner;

    *roots_out = fiber->root_count == 0u ? NULL : fiber->roots;
    *count_out = fiber->root_count;
    return ST_IMAGE_RUNTIME_OK;
}

static bool append_root(void *user, const st_value_t *slot)
{
    fiber_record_t *fiber = user;

    if (fiber->root_count == fiber->root_capacity) {
        size_t capacity = fiber->root_capacity == 0u ? 32u : fiber->root_capacity * 2u;

        if (capacity < fiber->root_capacity || capacity > SIZE_MAX / sizeof(*fiber->roots))
            return false;

        st_value_t *roots = realloc(fiber->roots, capacity * sizeof(*roots));

        if (roots == NULL)
            return false;

        fiber->roots = roots;
        fiber->root_capacity = capacity;
    }

    fiber->roots[fiber->root_count++] = *slot;
    return true;
}

static bool initialize_roots(fiber_record_t *fiber, st_value_t block)
{
    st_value_t nil = st_value_nil();

    if (!append_root(fiber, &block) || !append_root(fiber, &nil))
        return false;

    fiber->provider = (st_image_root_provider_t) {
        .abi_version = ST_IMAGE_ROOT_PROVIDER_ABI_VERSION,
        .owner = fiber,
        .roots = fiber_roots
    };

    return st_image_runtime_root_provider_attach(fiber->thread->image, &fiber->provider);
}

static st_fiber_status_t snapshot(fiber_record_t *fiber, StFrame *top)
{
    const StFrame *slow = top;
    const StFrame *fast = top;
    st_fiber_status_t status = ST_FIBER_INVALID_FRAME;

    fiber->root_count = 2u;

    while (fast != NULL && fast->caller != NULL) {
        slow = slow->caller;
        fast = fast->caller->caller;

        if (slow == fast)
            return ST_FIBER_INVALID_FRAME;
    }

    for (StFrame *frame = top; frame != NULL; frame = frame->caller) {
        if (frame->thread != fiber->thread || st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
            goto failed;

        if (!append_root(fiber, &frame->receiver)) {
            status = ST_FIBER_OUT_OF_MEMORY;
            goto failed;
        }

        for (size_t index = 0u; index < frame->argc; index++) {
            if (!append_root(fiber, &frame->argv[index])) {
                status = ST_FIBER_OUT_OF_MEMORY;
                goto failed;
            }
        }

        const st_root_map_t *map = NULL;

        for (size_t index = 0u; index < frame->method->root_map_count; index++) {
            if (frame->method->root_maps[index].safepoint_id == frame->safepoint_id) {
                map = &frame->method->root_maps[index];
                break;
            }
        }

        /* AOT methods may enter with safepoint zero before publishing a map.
         * They may not suspend there: the compiler must publish live roots. */
        if (map == NULL)
            goto failed;

        for (size_t index = 0u; index < map->root_count; index++) {
            if ((map->live_root_bitmap[index / 64u] & (UINT64_C(1) << (index % 64u))) == 0u)
                continue;

            if (!append_root(fiber, &frame->roots[index])) {
                status = ST_FIBER_OUT_OF_MEMORY;
                goto failed;
            }
        }
    }

    size_t visited = 0u;
    st_control_status_t control = st_aot_control_visit_roots(top, append_root, fiber, &visited);

    if (control == ST_CONTROL_OK)
        return ST_FIBER_OK;

    status = control == ST_CONTROL_ERR_VISITOR_ABORTED ? ST_FIBER_OUT_OF_MEMORY : ST_FIBER_INVALID_FRAME;

failed:
    fiber->root_count = 2u;
    return status;
}

static st_fiber_status_t current_fiber(StFrame *frame, fiber_record_t **fiber_out)
{
    if (st_aot_frame_validate(frame, 0u) != ST_AOT_SEND_OK)
        return ST_FIBER_INVALID_FRAME;

    st_aot_thread_t *thread = frame->thread;
    st_fiber_context_t *context = thread->fibers;

    if (context == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    if (context->owner_id != st_fiber_platform.thread_id())
        return ST_FIBER_WRONG_THREAD;

    if (context->shutting_down)
        return ST_FIBER_BUSY;

    if (context->current->thread != thread)
        return ST_FIBER_INVALID_FRAME;

    *fiber_out = context->current;
    return ST_FIBER_OK;
}

static void enqueue(st_fiber_context_t *context, fiber_record_t *fiber)
{
    fiber->state = FIBER_READY;
    fiber->ready_next = NULL;

    if (context->ready_tail != NULL)
        context->ready_tail->ready_next = fiber;
    else
        context->ready_head = fiber;

    context->ready_tail = fiber;
}

static bool reserve_timer(st_fiber_context_t *context)
{
    if (context->timer_count < context->timer_capacity)
        return true;

    size_t capacity = context->timer_capacity == 0u ? 32u : context->timer_capacity * 2u;

    if (capacity < context->timer_capacity || capacity > SIZE_MAX / sizeof(*context->timers))
        return false;

    fiber_record_t **timers = realloc(context->timers, capacity * sizeof(*timers));

    if (timers == NULL)
        return false;

    context->timers = timers;
    context->timer_capacity = capacity;
    return true;
}

static void timer_swap(st_fiber_context_t *context, size_t first, size_t second)
{
    fiber_record_t *temporary = context->timers[first];

    context->timers[first] = context->timers[second];
    context->timers[second] = temporary;
    context->timers[first]->timer_index = first;
    context->timers[second]->timer_index = second;
}

static void timer_up(st_fiber_context_t *context, size_t index)
{
    while (index != 0u) {
        size_t parent = (index - 1u) / 2u;

        if (context->timers[parent]->wake_at <= context->timers[index]->wake_at)
            break;

        timer_swap(context, parent, index);
        index = parent;
    }
}

static void timer_add(st_fiber_context_t *context, fiber_record_t *fiber, uint64_t deadline)
{
    fiber->wake_at = deadline;
    fiber->timer_index = context->timer_count;
    context->timers[context->timer_count++] = fiber;
    timer_up(context, fiber->timer_index);
}

static void timer_remove(st_fiber_context_t *context, fiber_record_t *fiber)
{
    size_t index = fiber->timer_index;

    if (index == SIZE_MAX)
        return;

    fiber->timer_index = SIZE_MAX;
    context->timer_count--;

    if (index == context->timer_count)
        return;

    context->timers[index] = context->timers[context->timer_count];
    context->timers[index]->timer_index = index;

    if (index != 0u && context->timers[index]->wake_at < context->timers[(index - 1u) / 2u]->wake_at) {
        timer_up(context, index);
        return;
    }

    for (;;) {
        size_t left = index * 2u + 1u;

        if (left >= context->timer_count)
            break;

        size_t smallest = left;
        size_t right = left + 1u;

        if (right < context->timer_count && context->timers[right]->wake_at < context->timers[left]->wake_at)
            smallest = right;

        if (context->timers[index]->wake_at <= context->timers[smallest]->wake_at)
            break;

        timer_swap(context, index, smallest);
        index = smallest;
    }
}

static void wake_fiber(st_fiber_context_t *context, fiber_record_t *fiber)
{
    timer_remove(context, fiber);

    if (fiber->state == FIBER_IO)
        context->io_count--;

    if (fiber == &context->main)
        fiber->state = FIBER_RUNNING;
    else
        enqueue(context, fiber);
}

static bool dispatch_one(st_fiber_context_t *context, uint64_t wait_until)
{
    bool main_was_waiting = context->main.state != FIBER_RUNNING;

    for (;;) {
        uint64_t now = st_fiber_platform.milliseconds();

        while (context->timer_count != 0u && context->timers[0]->wake_at <= now)
            wake_fiber(context, context->timers[0]);

        uint64_t nearest = wait_until;

        if (context->timer_count != 0u && context->timers[0]->wake_at < nearest)
            nearest = context->timers[0]->wake_at;

        int timeout = -1;

        if (context->ready_head != NULL || now >= nearest)
            timeout = 0;
        else if (nearest != UINT64_MAX)
            timeout = nearest - now > INT_MAX ? INT_MAX : (int)(nearest - now);

        if (context->io_count != 0u) {
            void *tokens[256];
            int error;
            int count = st_socket_platform.wait(context->reactor, tokens, 256u, timeout, &error);

            if (count < 0)
                abort();

            for (int index = 0; index < count; index++) {
                fiber_record_t *fiber = tokens[index];

                /* A completion may arrive after its deadline woke the fiber.
                 * Its native result still must be drained before buffer release. */
                fiber->io_ready = true;

                if (fiber->state == FIBER_IO)
                    wake_fiber(context, fiber);
            }
        } else if (context->ready_head == NULL) {
            if (nearest == UINT64_MAX || now >= nearest)
                return false;

            st_fiber_platform.sleep((uint32_t)timeout);
        }

        if (context->ready_head != NULL)
            break;

        if ((main_was_waiting && context->main.state == FIBER_RUNNING) || st_fiber_platform.milliseconds() >= wait_until)
            return false;
    }

    fiber_record_t *fiber = context->ready_head;

    context->ready_head = fiber->ready_next;
    if (context->ready_head == NULL)
        context->ready_tail = NULL;

    context->current = fiber;
    fiber->root_count = 2u;
    fiber->state = FIBER_RUNNING;
    st_fiber_platform.transfer(context->main.native, fiber->native);
    context->current = &context->main;

    if (fiber->state == FIBER_FINISHED) {
        st_fiber_platform.destroy(fiber->native);
        fiber->native = NULL;
    }

    return true;
}

static void enter_block(void *argument)
{
    fiber_record_t *fiber = argument;
    const st_root_map_t map = { .safepoint_id = 1u };
    const StMethodDescriptor descriptor = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = 1u,
        .flags = ST_METHOD_CAN_UNWIND,
        .root_maps = &map,
        .root_map_count = 1u
    };
    StFrame root = {
        .thread = fiber->thread,
        .method = &descriptor,
        .receiver = fiber->roots[0],
        .safepoint_id = 1u
    };
    st_control_scope_t scope = {0};
    st_control_leave_result_t leave;
    st_value_t result = st_value_nil();

    st_control_scope_init(&scope);

    if (st_control_scope_enter(&fiber->control, &scope, &root) != ST_CONTROL_OK)
        abort();

    st_aot_closure_status_t call = st_aot_closure_invoke(&root, fiber->roots[0], NULL, 0u, &result);

    fiber->status = call == ST_AOT_CLOSURE_OK ? ST_FIBER_OK : ST_FIBER_RUNTIME_ERROR;

    if (st_control_scope_leave(&fiber->control, &scope, result, &leave) != ST_CONTROL_OK)
        abort();

    st_control_pending_info_t pending;

    if (st_control_pending_get(&fiber->control, &pending) != ST_CONTROL_OK)
        abort();

    if (pending.kind == ST_CONTROL_PENDING_EXCEPTION) {
        fiber->status = ST_FIBER_EXCEPTION;
        fiber->exception_class = pending.exception_class_id;
        result = pending.value;
    } else if (pending.kind != ST_CONTROL_PENDING_NONE) {
        fiber->status = ST_FIBER_RUNTIME_ERROR;
    }

    if (st_control_pending_clear(&fiber->control) != ST_CONTROL_OK)
        abort();

    fiber->roots[0] = st_value_nil();
    if (fiber->detached && fiber->status != ST_FIBER_OK)
        fiber->consumed = false;

    fiber->roots[1] = fiber->consumed ? st_value_nil() : result;
    fiber->root_count = 2u;
    fiber->state = FIBER_FINISHED;

    while (fiber->join_waiters != NULL) {
        fiber_record_t *waiter = fiber->join_waiters;

        fiber->join_waiters = waiter->join_next;
        waiter->join_next = NULL;
        wake_fiber(fiber->context, waiter);
    }

    st_fiber_platform.transfer(fiber->native, fiber->context->main.native);
    abort();
}

static bool initialize_thread(fiber_record_t *fiber, st_aot_thread_t *source)
{
    fiber->thread = &fiber->thread_storage;

    if (st_control_thread_init(&fiber->control, fiber->thread, source->control->_st_allocator) != ST_CONTROL_OK)
        return false;

    if (source->control->_st_class_object != NULL
            && st_control_exception_configure(&fiber->control, source->control->_st_class_object, source->control->_st_class_object_user) != ST_CONTROL_OK)
        return false;

    if (!st_aot_thread_init(fiber->thread, source->lookup, source->immediate_class_ids, source->heap_primitives, &fiber->control,
            source->closures, source->object_class, source->object_class_user, NULL, NULL))
        return false;

    fiber->thread->image = source->image;
    fiber->thread->streams = source->streams;
    fiber->thread->strings = source->strings;
    fiber->thread->numeric = source->numeric;
    fiber->thread->reflection = source->reflection;
    fiber->thread->fibers = fiber->context;
    fiber->thread->sockets = source->sockets;

    if (source->failure == st_dnu_send_failure) {
        if (st_dnu_context_fork(&fiber->dnu, source->failure_user) != ST_DNU_OK
                || st_dnu_context_attach(&fiber->dnu, fiber->thread) != ST_DNU_OK)
            return false;
    } else {
        fiber->thread->failure = source->failure;
        fiber->thread->failure_user = source->failure_user;
    }

    return true;
}

static void dispose_record(fiber_record_t *fiber)
{
    if (fiber->dnu.attached_thread != NULL)
        st_dnu_context_detach(&fiber->dnu, fiber->thread);

    if (fiber->dnu.initialized)
        st_dnu_context_destroy(&fiber->dnu);

    if (fiber->provider.owner != NULL)
        st_image_runtime_root_provider_detach(fiber->thread->image, &fiber->provider);

    if (fiber->control._st_thread_id != 0u && st_control_thread_destroy(&fiber->control) != ST_CONTROL_OK)
        abort();

    st_aot_thread_destroy(fiber->thread);
    st_fiber_platform.destroy(fiber->native);
    free(fiber->roots);
    free(fiber);
}

st_fiber_status_t st_fiber_context_create(st_aot_thread_t *owner, size_t stack_size, st_fiber_context_t **context_out)
{
    if (context_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *context_out = NULL;

    if (owner == NULL || !owner->initialized || owner->abi_version != ST_AOT_THREAD_ABI_VERSION || owner->fibers != NULL
            || owner->image == NULL || owner->control == NULL || owner->closures == NULL || stack_size < 65536u)
        return ST_FIBER_INVALID_ARGUMENT;

    st_fiber_context_t *context = calloc(1u, sizeof(*context));

    if (context == NULL)
        return ST_FIBER_OUT_OF_MEMORY;

    context->owner_id = st_fiber_platform.thread_id();
    context->stack_size = stack_size;
    context->next_id = 1u;
    context->main.context = context;
    context->main.thread = owner;
    context->main.state = FIBER_RUNNING;
    context->main.timer_index = SIZE_MAX;
    context->main.io_socket = ST_SOCKET_INVALID;
    context->current = &context->main;
    context->main.native = st_fiber_platform.capture();

    if (context->main.native == NULL || !initialize_roots(&context->main, st_value_nil())) {
        if (context->main.native != NULL)
            st_fiber_platform.release(context->main.native);

        free(context->main.roots);
        free(context);
        return ST_FIBER_OUT_OF_MEMORY;
    }

    owner->fibers = context;
    *context_out = context;
    return ST_FIBER_OK;
}

st_fiber_status_t st_fiber_spawn(StFrame *frame, st_value_t block, uint64_t *id_out)
{
    fiber_record_t *parent;
    st_fiber_status_t status = current_fiber(frame, &parent);

    if (id_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *id_out = 0u;
    if (status != ST_FIBER_OK)
        return status;

    st_aot_closure_target_t target;

    if (st_aot_closure_resolve(frame, block, 0u, &target) != ST_AOT_CLOSURE_OK || target.home != NULL)
        return ST_FIBER_INVALID_BLOCK;

    st_fiber_context_t *context = parent->context;

    if (context->next_id > (uint64_t)ST_SMALL_INTEGER_MAX)
        return ST_FIBER_BUSY;

    fiber_record_t *fiber = calloc(1u, sizeof(*fiber));

    if (fiber == NULL)
        return ST_FIBER_OUT_OF_MEMORY;

    fiber->context = context;
    fiber->timer_index = SIZE_MAX;
    fiber->io_socket = ST_SOCKET_INVALID;

    if (!initialize_thread(fiber, parent->thread) || !initialize_roots(fiber, block)) {
        dispose_record(fiber);
        return ST_FIBER_OUT_OF_MEMORY;
    }

    fiber->native = st_fiber_platform.create(context->stack_size, enter_block, fiber);

    if (fiber->native == NULL) {
        dispose_record(fiber);
        return ST_FIBER_OUT_OF_MEMORY;
    }

    fiber->id = context->next_id++;
    fiber->next = context->records;
    fiber->previous_link = &context->records;

    if (fiber->next != NULL)
        fiber->next->previous_link = &fiber->next;

    context->records = fiber;
    enqueue(context, fiber);
    *id_out = fiber->id;
    return ST_FIBER_OK;
}

st_fiber_status_t st_fiber_sleep(StFrame *frame, uint32_t milliseconds)
{
    fiber_record_t *fiber;
    st_fiber_status_t status = current_fiber(frame, &fiber);

    if (status != ST_FIBER_OK)
        return status;

    status = snapshot(fiber, frame);
    if (status != ST_FIBER_OK)
        return status;

    st_fiber_context_t *context = fiber->context;
    uint64_t now = st_fiber_platform.milliseconds();

    if (milliseconds != 0u && !reserve_timer(context)) {
        fiber->root_count = 2u;
        return ST_FIBER_OUT_OF_MEMORY;
    }

    fiber->wake_at = now > UINT64_MAX - milliseconds ? UINT64_MAX : now + milliseconds;

    if (fiber == &context->main) {
        do {
            if (!dispatch_one(context, fiber->wake_at)) {
                now = st_fiber_platform.milliseconds();
                if (fiber->wake_at > now)
                    st_fiber_platform.sleep((uint32_t)(fiber->wake_at - now));

                break;
            }
        } while (st_fiber_platform.milliseconds() < fiber->wake_at);
    } else {
        if (milliseconds == 0u)
            enqueue(context, fiber);
        else {
            fiber->state = FIBER_SLEEPING;
            timer_add(context, fiber, fiber->wake_at);
        }

        st_fiber_platform.transfer(fiber->native, context->main.native);
    }

    fiber->root_count = 2u;
    return ST_FIBER_OK;
}

st_fiber_status_t st_fiber_yield(StFrame *frame)
{
    return st_fiber_sleep(frame, 0u);
}

static bool exception_matches(void *user, uint32_t raised, uint32_t caught)
{
    const st_runtime_descriptors_t *descriptors = user;

    for (size_t hops = 0u; raised != 0u && hops < descriptors->class_count; hops++) {
        if (raised == caught)
            return true;

        const StClassDescriptor *descriptor = st_runtime_class(descriptors, raised);

        if (descriptor == NULL)
            return false;

        raised = descriptor->superclass_id;
    }

    return false;
}

static st_fiber_status_t report_detached_failure(fiber_record_t *current)
{
    for (fiber_record_t *fiber = current->context->records; fiber != NULL; fiber = fiber->next) {
        if (!fiber->detached || fiber->state != FIBER_FINISHED || fiber->consumed || fiber->status == ST_FIBER_OK)
            continue;

        if (fiber->status == ST_FIBER_EXCEPTION
                && st_control_exception_signal(current->thread->control, fiber->roots[1], fiber->exception_class,
                    exception_matches, (void *)current->thread->lookup->descriptors) != ST_CONTROL_OK)
            return ST_FIBER_RUNTIME_ERROR;

        fiber->consumed = true;
        fiber->roots[1] = st_value_nil();
        return fiber->status;
    }

    return ST_FIBER_OK;
}

st_fiber_status_t st_fiber_join(StFrame *frame, uint64_t id, st_value_t *result_out)
{
    fiber_record_t *fiber;
    st_fiber_status_t status = current_fiber(frame, &fiber);

    if (result_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *result_out = ST_VALUE_INVALID;
    if (status != ST_FIBER_OK)
        return status;

    st_fiber_context_t *context = fiber->context;
    fiber_record_t *target = context->records;

    while (target != NULL && target->id != id)
        target = target->next;

    if (target == NULL || target->consumed || target->detached)
        return ST_FIBER_NOT_FOUND;

    for (fiber_record_t *waiting = target; waiting != NULL; waiting = waiting->waiting_for) {
        if (waiting == fiber)
            return ST_FIBER_DEADLOCK;
    }

    status = snapshot(fiber, frame);
    if (status != ST_FIBER_OK)
        return status;

    fiber->waiting_for = target;
    target->join_references++;

    if (target->state != FIBER_FINISHED) {
        fiber->join_next = target->join_waiters;
        target->join_waiters = fiber;
    }

    while (target->state != FIBER_FINISHED) {
        fiber->state = FIBER_JOINING;

        if (fiber == &context->main) {
            if (!dispatch_one(context, UINT64_MAX)) {
                if (target->state == FIBER_FINISHED)
                    break;

                fiber_record_t **waiter = &target->join_waiters;

                while (*waiter != NULL && *waiter != fiber)
                    waiter = &(*waiter)->join_next;

                if (*waiter == fiber)
                    *waiter = fiber->join_next;

                fiber->join_next = NULL;
                fiber->waiting_for = NULL;
                target->join_references--;
                fiber->state = FIBER_RUNNING;
                fiber->root_count = 2u;
                return ST_FIBER_DEADLOCK;
            }
        } else {
            st_fiber_platform.transfer(fiber->native, context->main.native);
        }
    }

    fiber->waiting_for = NULL;
    target->join_references--;
    fiber->state = FIBER_RUNNING;
    fiber->root_count = 2u;

    if (target->consumed)
        return ST_FIBER_NOT_FOUND;

    status = target->status;
    *result_out = target->roots[1];

    if (status == ST_FIBER_EXCEPTION
            && st_control_exception_signal(fiber->thread->control, *result_out, target->exception_class,
                exception_matches, (void *)fiber->thread->lookup->descriptors) != ST_CONTROL_OK)
        return ST_FIBER_RUNTIME_ERROR;

    target->consumed = true;
    target->roots[1] = st_value_nil();
    return status;
}

st_fiber_status_t st_fiber_run(StFrame *frame)
{
    fiber_record_t *fiber;
    st_fiber_status_t status = current_fiber(frame, &fiber);

    if (status != ST_FIBER_OK)
        return status;

    st_fiber_context_t *context = fiber->context;

    if (fiber != &context->main)
        return ST_FIBER_DEADLOCK;

    status = snapshot(fiber, frame);
    if (status != ST_FIBER_OK)
        return status;

    while (dispatch_one(context, UINT64_MAX)) {
        /* dispatch_one blocks only when every runnable fiber is waiting. */
    }

    fiber->root_count = 2u;

    status = report_detached_failure(fiber);

    if (status != ST_FIBER_OK)
        return status;

    for (fiber_record_t *record = context->records; record != NULL; record = record->next) {
        if (record->state != FIBER_FINISHED)
            return ST_FIBER_DEADLOCK;

        if (record->status != ST_FIBER_OK && !record->consumed)
            status = record->status;
    }

    return status;
}

st_fiber_status_t st_fiber_detach(StFrame *frame, uint64_t id)
{
    fiber_record_t *current;
    st_fiber_status_t status = current_fiber(frame, &current);

    if (status != ST_FIBER_OK)
        return status;

    for (fiber_record_t *fiber = current->context->records; fiber != NULL; fiber = fiber->next) {
        if (fiber->id != id)
            continue;

        if (fiber->consumed || fiber->detached)
            return ST_FIBER_NOT_FOUND;

        if (fiber->join_references != 0u)
            return ST_FIBER_BUSY;

        fiber->detached = true;
        fiber->consumed = fiber->state != FIBER_FINISHED || fiber->status == ST_FIBER_OK;

        if (fiber->consumed)
            fiber->roots[1] = st_value_nil();
        return ST_FIBER_OK;
    }

    return ST_FIBER_NOT_FOUND;
}

st_fiber_status_t st_fiber_collect(StFrame *frame, size_t *reclaimed_out)
{
    fiber_record_t *current;
    st_fiber_status_t status = current_fiber(frame, &current);

    if (status != ST_FIBER_OK)
        return status;

    if (reclaimed_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    *reclaimed_out = 0u;
    status = report_detached_failure(current);

    if (status != ST_FIBER_OK)
        return status;

    st_heap_collection_stats_t stats;

    if (st_image_runtime_collect(current->thread->image, frame, &stats) != ST_IMAGE_RUNTIME_OK)
        return ST_FIBER_RUNTIME_ERROR;

    fiber_record_t *fiber = current->context->records;

    while (fiber != NULL) {
        fiber_record_t *next = fiber->next;

        if (fiber->state == FIBER_FINISHED && fiber->consumed && fiber->join_references == 0u
                && atomic_load(&fiber->control._st_live_token_count) == 0u) {
            *fiber->previous_link = fiber->next;

            if (fiber->next != NULL)
                fiber->next->previous_link = fiber->previous_link;

            dispose_record(fiber);
        }

        fiber = next;
    }

    *reclaimed_out = stats.reclaimed_objects;
    return ST_FIBER_OK;
}

uint64_t st_fiber_milliseconds(void)
{
    return st_fiber_platform.milliseconds();
}

st_fiber_status_t st_fiber_io_validate_frame(StFrame *frame)
{
    fiber_record_t *fiber;

    return current_fiber(frame, &fiber);
}

st_fiber_status_t st_fiber_io_prepare(StFrame *frame, st_socket_reactor_t **reactor_out)
{
    fiber_record_t *fiber;
    st_fiber_status_t status = current_fiber(frame, &fiber);

    if (status != ST_FIBER_OK)
        return status;

    if (reactor_out == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    status = snapshot(fiber, frame);

    if (status != ST_FIBER_OK)
        return status;

    st_fiber_context_t *context = fiber->context;

    if (context->reactor == NULL) {
        int error;

        context->reactor = st_socket_platform.create(&error);
        if (context->reactor == NULL) {
            fiber->root_count = 2u;
            return ST_FIBER_RUNTIME_ERROR;
        }
    }

    /* Reserve before an OS operation can borrow managed memory. Subsequent
     * waits in the same primitive therefore need no timer allocation. */
    if (!reserve_timer(context)) {
        fiber->root_count = 2u;
        return ST_FIBER_OUT_OF_MEMORY;
    }

    *reactor_out = context->reactor;
    return ST_FIBER_OK;
}

st_fiber_status_t st_fiber_io_wait(StFrame *frame, uintptr_t socket, uint32_t events, uint64_t deadline, int *error_out)
{
    fiber_record_t *fiber;
    st_fiber_status_t status = current_fiber(frame, &fiber);

    if (status != ST_FIBER_OK)
        return status;

    st_fiber_context_t *context = fiber->context;

    *error_out = 0;

    if (deadline != UINT64_MAX && st_fiber_milliseconds() >= deadline)
        return ST_FIBER_TIMEOUT;

    status = snapshot(fiber, frame);

    if (status != ST_FIBER_OK)
        return status;

    if (!reserve_timer(context))
        return ST_FIBER_OUT_OF_MEMORY;

    if (!st_socket_platform.watch(socket, events, fiber, error_out))
        return ST_FIBER_RUNTIME_ERROR;

    fiber->state = FIBER_IO;
    fiber->io_ready = false;
    fiber->io_interrupted = false;
    fiber->io_socket = socket;
    context->io_count++;

    if (deadline != UINT64_MAX)
        timer_add(context, fiber, deadline);

    if (fiber == &context->main) {
        while (fiber->state == FIBER_IO)
            dispatch_one(context, UINT64_MAX);
    } else {
        st_fiber_platform.transfer(fiber->native, context->main.native);
    }

    fiber->io_socket = ST_SOCKET_INVALID;

    if (fiber->io_interrupted)
        return ST_FIBER_INTERRUPTED;

    return fiber->io_ready ? ST_FIBER_OK : ST_FIBER_TIMEOUT;
}

void st_fiber_io_interrupt(StFrame *frame, uintptr_t socket)
{
    fiber_record_t *current;

    if (current_fiber(frame, &current) != ST_FIBER_OK)
        abort();

    st_fiber_context_t *context = current->context;
    fiber_record_t *fiber = &context->main;

    while (fiber != NULL) {
        if (fiber->io_socket == socket) {
            fiber->io_interrupted = true;

            if (fiber->state == FIBER_IO)
                wake_fiber(context, fiber);
        }

        fiber = fiber == &context->main ? context->records : fiber->next;
    }
}

void st_fiber_io_finish(StFrame *frame)
{
    fiber_record_t *fiber;

    if (current_fiber(frame, &fiber) != ST_FIBER_OK)
        abort();

    fiber->root_count = 2u;
}

st_fiber_status_t st_fiber_context_destroy(st_fiber_context_t *context)
{
    if (context == NULL)
        return ST_FIBER_INVALID_ARGUMENT;

    if (context->owner_id != st_fiber_platform.thread_id())
        return ST_FIBER_WRONG_THREAD;

    if (context->current != &context->main || context->main.root_count != 2u || context->main.thread->sockets != NULL)
        return ST_FIBER_BUSY;

    for (fiber_record_t *fiber = context->records; fiber != NULL; fiber = fiber->next) {
        if (fiber->state != FIBER_FINISHED)
            return ST_FIBER_BUSY;
    }

    /* Quiescent shutdown releases unconsumed results before the final precise
     * collection, which in turn releases homes retained by dead closures. */
    context->shutting_down = true;

    for (fiber_record_t *fiber = context->records; fiber != NULL; fiber = fiber->next)
        fiber->root_count = 0u;

    st_heap_collection_stats_t stats;

    if (st_image_runtime_collect(context->main.thread->image, NULL, &stats) != ST_IMAGE_RUNTIME_OK)
        return ST_FIBER_RUNTIME_ERROR;

    for (fiber_record_t *fiber = context->records; fiber != NULL; fiber = fiber->next) {
        if (atomic_load(&fiber->control._st_live_token_count) != 0u)
            return ST_FIBER_BUSY;
    }

    while (context->records != NULL) {
        fiber_record_t *fiber = context->records;

        context->records = fiber->next;
        dispose_record(fiber);
    }

    if (context->reactor != NULL) {
        int error;

        if (!st_socket_platform.destroy(context->reactor, &error))
            return ST_FIBER_BUSY;

        context->reactor = NULL;
    }

    if (!st_fiber_platform.release(context->main.native))
        return ST_FIBER_RUNTIME_ERROR;

    st_image_runtime_root_provider_detach(context->main.thread->image, &context->main.provider);
    context->main.thread->fibers = NULL;
    free(context->main.roots);
    free(context->timers);
    free(context);
    return ST_FIBER_OK;
}
