#include "st_control.h"

#include <stdlib.h>
#include <string.h>

enum {
    THREAD_READY = UINT32_C(0x4354524c),
    SCOPE_INITIALIZED = UINT32_C(0x5343494e),
    SCOPE_ACTIVE = UINT32_C(0x53434143),
    SCOPE_LEAVING = UINT32_C(0x53434c56),
    SCOPE_DONE = UINT32_C(0x5343444e),
    ENSURE_INITIALIZED = UINT32_C(0x454e494e),
    ENSURE_ARMED = UINT32_C(0x454e4152),
    ENSURE_RUNNING = UINT32_C(0x454e5255),
    ENSURE_DONE = UINT32_C(0x454e444e),
    HANDLER_INITIALIZED = UINT32_C(0x4844494e),
    HANDLER_ACTIVE = UINT32_C(0x48444143),
    HANDLER_DONE = UINT32_C(0x4844444e),
    HOME_ACTIVE = UINT32_C(0x484f4d41),
    HOME_RETURNED = UINT32_C(0x484f4d52)
};

struct StHomeToken {
    uint32_t abi_version;
    uint32_t reserved;
    _Atomic uint32_t references;
    _Atomic uint32_t state;
    st_home_token_id_t id;
    st_control_thread_t *owner;
    st_control_allocator_t allocator;
};

static _Atomic uint64_t next_thread_id = UINT64_C(1);

static bool handler_chain_is_valid(const st_control_thread_t *thread);
static bool handler_chain_contains(
    const st_control_thread_t *thread,
    const st_control_handler_t *candidate);

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static bool thread_is_ready(const st_control_thread_t *thread)
{
    return thread && thread->_st_abi_version == ST_CONTROL_ABI_VERSION &&
           thread->_st_state == THREAD_READY &&
           thread->_st_thread_id != 0u &&
           thread->_st_frame_thread_identity != NULL &&
           thread->_st_allocator.allocate && thread->_st_allocator.deallocate;
}

static bool token_is_valid(const StHomeToken *token)
{
    uint32_t state;
    if (!token || token->abi_version != ST_CONTROL_ABI_VERSION ||
        token->id.thread_id == 0u || token->id.activation_id == 0u ||
        !thread_is_ready(token->owner) ||
        token->id.thread_id != token->owner->_st_thread_id ||
        !token->allocator.allocate ||
        !token->allocator.deallocate ||
        atomic_load_explicit(&token->references, memory_order_acquire) == 0u)
        return false;
    state = atomic_load_explicit(&token->state, memory_order_acquire);
    return state == HOME_ACTIVE || state == HOME_RETURNED;
}

static bool pending_is_valid(const st_control_thread_t *thread)
{
    if (thread->_st_pending_kind == ST_CONTROL_PENDING_NONE)
        return thread->_st_pending_target == NULL &&
               thread->_st_pending_handler == NULL &&
               thread->_st_pending_value == 0u &&
               thread->_st_pending_exception_class_id == 0u;
    if (thread->_st_pending_kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN)
        return thread->_st_pending_handler == NULL &&
               thread->_st_pending_exception_class_id == 0u &&
               token_is_valid(thread->_st_pending_target) &&
               thread->_st_pending_target->owner == thread;
    return thread->_st_pending_kind == ST_CONTROL_PENDING_EXCEPTION &&
           thread->_st_pending_target == NULL &&
           thread->_st_pending_exception_class_id != 0u &&
           st_value_kind(thread->_st_pending_value) == ST_VALUE_OBJECT &&
           (thread->_st_pending_handler == NULL ||
            handler_chain_contains(
                thread, thread->_st_pending_handler));
}

static bool allocate_thread_id(uint64_t *id_out)
{
    uint64_t candidate = atomic_load_explicit(&next_thread_id,
                                               memory_order_relaxed);
    while (candidate != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &next_thread_id, &candidate, candidate + 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            *id_out = candidate;
            return true;
        }
    }
    return false;
}

static bool increment_live_tokens(st_control_thread_t *thread)
{
    size_t count = atomic_load_explicit(&thread->_st_live_token_count,
                                        memory_order_relaxed);
    while (count != SIZE_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &thread->_st_live_token_count, &count, count + 1u,
                memory_order_release, memory_order_relaxed))
            return true;
    }
    return false;
}

static bool scope_header_valid(const st_control_scope_t *scope)
{
    return scope && scope->_st_abi_version == ST_CONTROL_ABI_VERSION &&
           (scope->_st_state == SCOPE_ACTIVE ||
            scope->_st_state == SCOPE_LEAVING) &&
           (scope->_st_state == SCOPE_LEAVING
                ? scope->_st_leave_value_active
                : !scope->_st_leave_value_active);
}

static bool scope_chain_is_valid(const st_control_thread_t *thread)
{
    const st_control_scope_t *slow = thread->_st_top_scope;
    const st_control_scope_t *fast = thread->_st_top_scope;
    const st_control_scope_t *scope = thread->_st_top_scope;
    StFrame *expected = NULL;
    while (fast && fast->_st_previous) {
        slow = slow->_st_previous;
        fast = fast->_st_previous->_st_previous;
        if (slow == fast) return false;
    }
    while (scope) {
        if (!scope_header_valid(scope) || scope->_st_thread != thread ||
            !scope->_st_frame ||
            scope->_st_frame->thread != thread->_st_frame_thread_identity ||
            (expected && expected->caller != scope->_st_frame))
            return false;
        expected = scope->_st_frame;
        scope = scope->_st_previous;
    }
    return expected == NULL || expected->caller == NULL;
}

static bool scope_chain_contains_home(const st_control_thread_t *thread,
                                      const StHomeToken *target)
{
    const st_control_scope_t *scope;
    if (!scope_chain_is_valid(thread)) return false;
    for (scope = thread->_st_top_scope; scope; scope = scope->_st_previous)
        if (scope->_st_activation_home == target) return true;
    return false;
}

static bool scope_chain_contains_scope(
    const st_control_thread_t *thread,
    const st_control_scope_t *candidate)
{
    const st_control_scope_t *scope;
    for (scope = thread->_st_top_scope; scope; scope = scope->_st_previous)
        if (scope == candidate) return true;
    return false;
}

static bool handler_chain_is_valid(const st_control_thread_t *thread)
{
    const st_control_handler_t *slow = thread->_st_handler_top;
    const st_control_handler_t *fast = thread->_st_handler_top;
    const st_control_handler_t *handler;
    while (fast != NULL && fast->_st_previous != NULL) {
        slow = slow->_st_previous;
        fast = fast->_st_previous->_st_previous;
        if (slow == fast) return false;
    }
    for (handler = thread->_st_handler_top; handler != NULL;
         handler = handler->_st_previous) {
        if (handler->_st_abi_version != ST_CONTROL_ABI_VERSION ||
            handler->_st_state != HANDLER_ACTIVE ||
            handler->_st_thread != thread || handler->_st_scope == NULL ||
            !scope_chain_contains_scope(thread, handler->_st_scope) ||
            handler->_st_caught_class_id == 0u ||
            handler->_st_reserved != 0u ||
            ((handler->_st_roots == NULL) !=
             (handler->_st_root_count == 0u)))
            return false;
    }
    return true;
}

static bool handler_chain_contains(
    const st_control_thread_t *thread,
    const st_control_handler_t *candidate)
{
    const st_control_handler_t *handler;
    if (!handler_chain_is_valid(thread)) return false;
    for (handler = thread->_st_handler_top; handler != NULL;
         handler = handler->_st_previous)
        if (handler == candidate) return true;
    return false;
}

st_control_status_t st_control_thread_init(
    st_control_thread_t *thread, void *thread_identity,
    st_control_allocator_t allocator)
{
    uint64_t thread_id;
    if (!thread || !thread_identity || thread->_st_state != 0u ||
        ((allocator.allocate == NULL) != (allocator.deallocate == NULL)))
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    if (!allocator.allocate) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
    }
    if (!allocate_thread_id(&thread_id)) return ST_CONTROL_ERR_ID_EXHAUSTED;
    memset(thread, 0, sizeof(*thread));
    thread->_st_abi_version = ST_CONTROL_ABI_VERSION;
    thread->_st_state = THREAD_READY;
    thread->_st_thread_id = thread_id;
    thread->_st_next_activation_id = 1u;
    thread->_st_frame_thread_identity = thread_identity;
    thread->_st_allocator = allocator;
    atomic_init(&thread->_st_live_token_count, 0u);
    return ST_CONTROL_OK;
}

st_control_status_t st_control_thread_destroy(st_control_thread_t *thread)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (thread->_st_top_scope || thread->_st_pending_kind !=
            ST_CONTROL_PENDING_NONE || thread->_st_pending_target ||
        thread->_st_callback_depth != 0u ||
        thread->_st_running_ensure != NULL ||
        thread->_st_handler_top != NULL ||
        atomic_load_explicit(&thread->_st_live_token_count,
                             memory_order_acquire) != 0u)
        return ST_CONTROL_ERR_BUSY;
    memset(thread, 0, sizeof(*thread));
    return ST_CONTROL_OK;
}

st_control_status_t st_control_exception_configure(
    st_control_thread_t *thread, st_control_class_object_fn class_object,
    void *user)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (class_object == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (thread->_st_top_scope != NULL || thread->_st_callback_depth != 0u ||
        thread->_st_handler_top != NULL ||
        thread->_st_pending_kind != ST_CONTROL_PENDING_NONE)
        return ST_CONTROL_ERR_BUSY;
    if (thread->_st_class_object != NULL)
        return ST_CONTROL_ERR_BUSY;
    thread->_st_class_object = class_object;
    thread->_st_class_object_user = user;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_exception_class_resolve(
    st_control_thread_t *thread, st_value_t class_object,
    uint32_t *class_id_out)
{
    uint32_t class_id = 0u;
    if (class_id_out == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *class_id_out = 0u;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (thread->_st_class_object == NULL ||
        st_value_kind(class_object) != ST_VALUE_OBJECT ||
        !thread->_st_class_object(
            thread->_st_class_object_user, class_object, &class_id) ||
        class_id == 0u)
        return ST_CONTROL_ERR_INVALID_EXCEPTION;
    *class_id_out = class_id;
    return ST_CONTROL_OK;
}

void st_control_scope_init(st_control_scope_t *scope)
{
    if (!scope) return;
    memset(scope, 0, sizeof(*scope));
    scope->_st_abi_version = ST_CONTROL_ABI_VERSION;
    scope->_st_state = SCOPE_INITIALIZED;
}

st_control_status_t st_control_scope_enter(
    st_control_thread_t *thread, st_control_scope_t *scope, StFrame *frame)
{
    StHomeToken *captured_home;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!scope || scope->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        scope->_st_state != SCOPE_INITIALIZED)
        return ST_CONTROL_ERR_INVALID_SCOPE;
    if (!frame || frame->thread != thread->_st_frame_thread_identity ||
        (thread->_st_top_scope
             ? frame->caller != thread->_st_top_scope->_st_frame
             : frame->caller != NULL) ||
        !scope_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_FRAME;
    if (thread->_st_pending_kind != ST_CONTROL_PENDING_NONE &&
        thread->_st_callback_depth == 0u)
        return ST_CONTROL_ERR_BUSY;
    captured_home = frame->home;
    if (captured_home) {
        st_control_status_t status;
        if (!token_is_valid(captured_home) ||
            captured_home->owner != thread)
            return ST_CONTROL_ERR_INVALID_TOKEN;
        status = st_home_token_retain(captured_home);
        if (status != ST_CONTROL_OK) return status;
    }
    scope->_st_thread = thread;
    scope->_st_frame = frame;
    scope->_st_previous = thread->_st_top_scope;
    scope->_st_ensure_top = NULL;
    scope->_st_frame_home_ref = captured_home;
    scope->_st_activation_home = NULL;
    scope->_st_leave_value = 0u;
    scope->_st_leave_value_active = false;
    scope->_st_state = SCOPE_ACTIVE;
    thread->_st_top_scope = scope;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_scope_establish_home(
    st_control_thread_t *thread, st_control_scope_t *scope,
    StHomeToken **home_out)
{
    StHomeToken *token;
    uint64_t activation_id;
    if (home_out) *home_out = NULL;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!home_out || !scope || scope->_st_thread != thread ||
        scope->_st_state != SCOPE_ACTIVE ||
        thread->_st_top_scope != scope || !scope->_st_frame ||
        scope->_st_frame_home_ref || scope->_st_activation_home ||
        scope->_st_frame->home || !scope_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_SCOPE;
    activation_id = thread->_st_next_activation_id;
    if (activation_id == UINT64_MAX) return ST_CONTROL_ERR_ID_EXHAUSTED;
    token = thread->_st_allocator.allocate(thread->_st_allocator.user,
                                           sizeof(*token));
    if (!token) return ST_CONTROL_ERR_OUT_OF_MEMORY;
    if (!increment_live_tokens(thread)) {
        thread->_st_allocator.deallocate(thread->_st_allocator.user, token);
        return ST_CONTROL_ERR_REFCOUNT_OVERFLOW;
    }
    memset(token, 0, sizeof(*token));
    token->abi_version = ST_CONTROL_ABI_VERSION;
    atomic_init(&token->references, 1u);
    atomic_init(&token->state, HOME_ACTIVE);
    token->id.thread_id = thread->_st_thread_id;
    token->id.activation_id = activation_id;
    token->owner = thread;
    token->allocator = thread->_st_allocator;
    thread->_st_next_activation_id = activation_id + 1u;
    scope->_st_frame_home_ref = token;
    scope->_st_activation_home = token;
    scope->_st_frame->home = token;
    *home_out = token;
    return ST_CONTROL_OK;
}

void st_control_ensure_init(st_control_ensure_t *ensure_record)
{
    if (!ensure_record) return;
    memset(ensure_record, 0, sizeof(*ensure_record));
    ensure_record->_st_abi_version = ST_CONTROL_ABI_VERSION;
    ensure_record->_st_state = ENSURE_INITIALIZED;
}

st_control_status_t st_control_ensure_push(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record, st_control_ensure_fn callback,
    void *user)
{
    return st_control_ensure_push_with_roots(
        thread, scope, ensure_record, callback, user, NULL, 0u);
}

st_control_status_t st_control_ensure_push_with_roots(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record, st_control_ensure_fn callback,
    void *user, const st_value_t *roots, size_t root_count)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!scope || scope->_st_thread != thread ||
        scope->_st_state != SCOPE_ACTIVE || thread->_st_top_scope != scope ||
        !scope_chain_is_valid(thread))
        return scope && scope->_st_state == SCOPE_LEAVING
            ? ST_CONTROL_ERR_REENTRANT : ST_CONTROL_ERR_INVALID_SCOPE;
    if (!ensure_record || ((roots == NULL) != (root_count == 0u)) ||
        ensure_record->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        ensure_record->_st_state != ENSURE_INITIALIZED || !callback)
        return ST_CONTROL_ERR_INVALID_ENSURE;
    ensure_record->_st_scope = scope;
    ensure_record->_st_previous = scope->_st_ensure_top;
    ensure_record->_st_callback = callback;
    ensure_record->_st_user = user;
    ensure_record->_st_roots = roots;
    ensure_record->_st_root_count = root_count;
    ensure_record->_st_running_previous = NULL;
    ensure_record->_st_state = ENSURE_ARMED;
    scope->_st_ensure_top = ensure_record;
    return ST_CONTROL_OK;
}

static void ensure_mark_done(st_control_ensure_t *ensure_record)
{
    ensure_record->_st_scope = NULL;
    ensure_record->_st_previous = NULL;
    ensure_record->_st_callback = NULL;
    ensure_record->_st_user = NULL;
    ensure_record->_st_roots = NULL;
    ensure_record->_st_root_count = 0u;
    ensure_record->_st_running_previous = NULL;
    ensure_record->_st_state = ENSURE_DONE;
}

static st_control_status_t ensure_run_top(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record)
{
    st_control_ensure_fn callback;
    void *user;
    if (ensure_record == NULL || scope->_st_ensure_top != ensure_record ||
        ensure_record->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        ensure_record->_st_state != ENSURE_ARMED ||
        ensure_record->_st_scope != scope ||
        ensure_record->_st_callback == NULL ||
        ((ensure_record->_st_roots == NULL) !=
         (ensure_record->_st_root_count == 0u)))
        return ST_CONTROL_ERR_INVALID_ENSURE;
    if (thread->_st_callback_depth == UINT32_MAX)
        return ST_CONTROL_ERR_REENTRANT;

    scope->_st_ensure_top = ensure_record->_st_previous;
    callback = ensure_record->_st_callback;
    user = ensure_record->_st_user;
    ensure_record->_st_state = ENSURE_RUNNING;
    ensure_record->_st_running_previous = thread->_st_running_ensure;
    thread->_st_running_ensure = ensure_record;
    thread->_st_callback_depth++;
    callback(user, scope->_st_frame, thread);
    thread->_st_callback_depth--;
    if (thread->_st_running_ensure != ensure_record)
        return ST_CONTROL_ERR_CALLBACK_STATE;
    thread->_st_running_ensure = ensure_record->_st_running_previous;
    ensure_mark_done(ensure_record);
    if (thread->_st_top_scope != scope || !scope_chain_is_valid(thread) ||
        !handler_chain_is_valid(thread) || !pending_is_valid(thread))
        return ST_CONTROL_ERR_CALLBACK_STATE;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_ensure_run(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_ensure_t *ensure_record)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (scope == NULL || scope->_st_thread != thread ||
        scope->_st_state != SCOPE_ACTIVE || thread->_st_top_scope != scope ||
        !scope_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_SCOPE;
    return ensure_run_top(thread, scope, ensure_record);
}

void st_control_handler_init(st_control_handler_t *handler)
{
    if (handler == NULL) return;
    memset(handler, 0, sizeof(*handler));
    handler->_st_abi_version = ST_CONTROL_ABI_VERSION;
    handler->_st_state = HANDLER_INITIALIZED;
}

static void handler_mark_done(st_control_handler_t *handler)
{
    handler->_st_thread = NULL;
    handler->_st_scope = NULL;
    handler->_st_previous = NULL;
    handler->_st_caught_class_id = 0u;
    handler->_st_reserved = 0u;
    handler->_st_roots = NULL;
    handler->_st_root_count = 0u;
    handler->_st_state = HANDLER_DONE;
}

st_control_status_t st_control_handler_push(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_control_handler_t *handler, uint32_t caught_class_id,
    const st_value_t *roots, size_t root_count)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (scope == NULL || scope->_st_thread != thread ||
        scope->_st_state != SCOPE_ACTIVE || thread->_st_top_scope != scope ||
        !scope_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_SCOPE;
    if (handler == NULL ||
        handler->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        handler->_st_state != HANDLER_INITIALIZED ||
        caught_class_id == 0u ||
        ((roots == NULL) != (root_count == 0u)))
        return ST_CONTROL_ERR_INVALID_HANDLER;
    if (thread->_st_pending_kind != ST_CONTROL_PENDING_NONE &&
        thread->_st_callback_depth == 0u)
        return ST_CONTROL_ERR_BUSY;
    handler->_st_thread = thread;
    handler->_st_scope = scope;
    handler->_st_previous = thread->_st_handler_top;
    handler->_st_caught_class_id = caught_class_id;
    handler->_st_reserved = 0u;
    handler->_st_roots = roots;
    handler->_st_root_count = root_count;
    handler->_st_state = HANDLER_ACTIVE;
    thread->_st_handler_top = handler;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_handler_pop(
    st_control_thread_t *thread, st_control_handler_t *handler)
{
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (handler == NULL || handler->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        handler->_st_state != HANDLER_ACTIVE ||
        handler->_st_thread != thread || thread->_st_handler_top != handler)
        return ST_CONTROL_ERR_INVALID_HANDLER;
    if (thread->_st_pending_kind == ST_CONTROL_PENDING_EXCEPTION &&
        thread->_st_pending_handler == handler)
        return ST_CONTROL_ERR_BUSY;
    thread->_st_handler_top = handler->_st_previous;
    handler_mark_done(handler);
    return ST_CONTROL_OK;
}

st_control_status_t st_control_handler_is_target(
    const st_control_thread_t *thread,
    const st_control_handler_t *handler, bool *is_target_out)
{
    if (is_target_out == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *is_target_out = false;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (handler == NULL || !handler_chain_contains(thread, handler))
        return ST_CONTROL_ERR_INVALID_HANDLER;
    *is_target_out = thread->_st_pending_kind == ST_CONTROL_PENDING_EXCEPTION &&
        thread->_st_pending_handler == handler;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_handler_consume_exception(
    st_control_thread_t *thread, st_control_handler_t *handler,
    st_value_t *exception_out)
{
    if (exception_out == NULL) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *exception_out = (st_value_t)ST_VALUE_INVALID;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (handler == NULL || thread->_st_handler_top != handler ||
        thread->_st_pending_kind != ST_CONTROL_PENDING_EXCEPTION ||
        thread->_st_pending_handler != handler)
        return ST_CONTROL_ERR_INVALID_HANDLER;
    *exception_out = thread->_st_pending_value;
    thread->_st_pending_handler = NULL;
    thread->_st_pending_value = 0u;
    thread->_st_pending_kind = ST_CONTROL_PENDING_NONE;
    thread->_st_pending_exception_class_id = 0u;
    thread->_st_handler_top = handler->_st_previous;
    handler_mark_done(handler);
    return ST_CONTROL_OK;
}

st_control_status_t st_control_exception_signal(
    st_control_thread_t *thread, st_value_t exception,
    uint32_t exception_class_id, st_control_exception_match_fn match,
    void *match_user)
{
    st_control_handler_t *handler;
    StHomeToken *old_target;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (st_value_kind(exception) != ST_VALUE_OBJECT ||
        exception_class_id == 0u || match == NULL)
        return ST_CONTROL_ERR_INVALID_EXCEPTION;
    if (thread->_st_pending_kind != ST_CONTROL_PENDING_NONE &&
        thread->_st_callback_depth == 0u)
        return ST_CONTROL_ERR_BUSY;
    handler = thread->_st_handler_top;
    while (handler != NULL &&
           !match(match_user, exception_class_id,
                  handler->_st_caught_class_id))
        handler = handler->_st_previous;

    old_target = thread->_st_pending_target;
    thread->_st_pending_target = NULL;
    thread->_st_pending_handler = handler;
    thread->_st_pending_value = exception;
    thread->_st_pending_kind = ST_CONTROL_PENDING_EXCEPTION;
    thread->_st_pending_exception_class_id = exception_class_id;
    if (old_target != NULL) st_home_token_release(old_target);
    return ST_CONTROL_OK;
}

st_control_status_t st_home_token_retain(StHomeToken *token)
{
    uint32_t references;
    if (!token_is_valid(token)) return ST_CONTROL_ERR_INVALID_TOKEN;
    references = atomic_load_explicit(&token->references,
                                      memory_order_relaxed);
    for (;;) {
        if (references == 0u) return ST_CONTROL_ERR_INVALID_TOKEN;
        if (references == UINT32_MAX)
            return ST_CONTROL_ERR_REFCOUNT_OVERFLOW;
        if (atomic_compare_exchange_weak_explicit(
                &token->references, &references, references + 1u,
                memory_order_acquire, memory_order_relaxed))
            return ST_CONTROL_OK;
    }
}

void st_home_token_release(StHomeToken *token)
{
    uint32_t previous;
    st_control_allocator_t allocator;
    st_control_thread_t *owner;
    if (!token || token->abi_version != ST_CONTROL_ABI_VERSION) return;
    previous = atomic_load_explicit(&token->references, memory_order_relaxed);
    for (;;) {
        if (previous == 0u) return;
        if (atomic_compare_exchange_weak_explicit(
                &token->references, &previous, previous - 1u,
                memory_order_acq_rel, memory_order_relaxed))
            break;
    }
    if (previous != 1u) return;
    allocator = token->allocator;
    owner = token->owner;
    token->abi_version = 0u;
    atomic_fetch_sub_explicit(&owner->_st_live_token_count, 1u,
                              memory_order_acq_rel);
    allocator.deallocate(allocator.user, token);
}

bool st_home_token_is_active(const StHomeToken *token)
{
    return token_is_valid(token) &&
           atomic_load_explicit(&token->state, memory_order_acquire) ==
               HOME_ACTIVE;
}

st_control_status_t st_home_token_id(
    const StHomeToken *token, st_home_token_id_t *id_out)
{
    if (!id_out) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *id_out = (st_home_token_id_t){0};
    if (!token_is_valid(token)) return ST_CONTROL_ERR_INVALID_TOKEN;
    *id_out = token->id;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_non_local_return(
    st_control_thread_t *thread, StHomeToken *target, st_value_t value)
{
    st_control_status_t status;
    StHomeToken *old_target;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!token_is_valid(target) || target->owner != thread)
        return ST_CONTROL_ERR_INVALID_TOKEN;
    if (!st_home_token_is_active(target))
        return ST_CONTROL_ERR_BLOCK_RETURNED;
    if (!scope_chain_contains_home(thread, target))
        return ST_CONTROL_ERR_TARGET_NOT_ACTIVE;
    if (thread->_st_pending_kind != ST_CONTROL_PENDING_NONE &&
        thread->_st_callback_depth == 0u)
        return ST_CONTROL_ERR_BUSY;
    status = st_home_token_retain(target);
    if (status != ST_CONTROL_OK) return status;
    old_target = thread->_st_pending_target;
    thread->_st_pending_target = target;
    thread->_st_pending_handler = NULL;
    thread->_st_pending_value = value;
    thread->_st_pending_kind = ST_CONTROL_PENDING_NON_LOCAL_RETURN;
    thread->_st_pending_exception_class_id = 0u;
    if (old_target) st_home_token_release(old_target);
    return ST_CONTROL_OK;
}

st_control_status_t st_control_pending_get(
    const st_control_thread_t *thread, st_control_pending_info_t *info_out)
{
    if (!info_out) return ST_CONTROL_ERR_INVALID_ARGUMENT;
    *info_out = (st_control_pending_info_t){0};
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    info_out->kind = (st_control_pending_kind_t)thread->_st_pending_kind;
    if (thread->_st_pending_kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN) {
        if (!token_is_valid(thread->_st_pending_target))
            return ST_CONTROL_ERR_INVALID_TOKEN;
        info_out->target = thread->_st_pending_target->id;
        info_out->value = thread->_st_pending_value;
    } else if (thread->_st_pending_kind == ST_CONTROL_PENDING_EXCEPTION) {
        info_out->value = thread->_st_pending_value;
        info_out->exception_class_id =
            thread->_st_pending_exception_class_id;
        info_out->has_handler = thread->_st_pending_handler != NULL;
    }
    return ST_CONTROL_OK;
}

st_control_status_t st_control_pending_clear(st_control_thread_t *thread)
{
    StHomeToken *target;
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (thread->_st_top_scope || thread->_st_callback_depth != 0u)
        return ST_CONTROL_ERR_BUSY;
    target = thread->_st_pending_target;
    thread->_st_pending_target = NULL;
    thread->_st_pending_handler = NULL;
    thread->_st_pending_value = 0u;
    thread->_st_pending_kind = ST_CONTROL_PENDING_NONE;
    thread->_st_pending_exception_class_id = 0u;
    if (target) st_home_token_release(target);
    return ST_CONTROL_OK;
}

static bool ensure_roots_valid(const st_control_ensure_t *ensure_record)
{
    return ensure_record != NULL && ensure_record->_st_callback != NULL &&
        ((ensure_record->_st_roots == NULL) ==
         (ensure_record->_st_root_count == 0u));
}

static bool scope_contains(const st_control_thread_t *thread,
                           const st_control_scope_t *candidate)
{
    const st_control_scope_t *scope;
    for (scope = thread->_st_top_scope; scope; scope = scope->_st_previous)
        if (scope == candidate) return true;
    return false;
}

static bool armed_ensure_chain_valid(const st_control_scope_t *scope)
{
    const st_control_ensure_t *slow = scope->_st_ensure_top;
    const st_control_ensure_t *fast = scope->_st_ensure_top;
    const st_control_ensure_t *record;
    while (fast && fast->_st_previous) {
        slow = slow->_st_previous;
        fast = fast->_st_previous->_st_previous;
        if (slow == fast) return false;
    }
    for (record = scope->_st_ensure_top; record;
         record = record->_st_previous) {
        if (record->_st_abi_version != ST_CONTROL_ABI_VERSION ||
            record->_st_state != ENSURE_ARMED ||
            record->_st_scope != scope ||
            record->_st_running_previous != NULL ||
            !ensure_roots_valid(record))
            return false;
    }
    return true;
}

static bool running_ensure_chain_valid(const st_control_thread_t *thread)
{
    const st_control_ensure_t *slow = thread->_st_running_ensure;
    const st_control_ensure_t *fast = thread->_st_running_ensure;
    const st_control_ensure_t *running;
    size_t count = 0u;
    while (fast && fast->_st_running_previous) {
        slow = slow->_st_running_previous;
        fast = fast->_st_running_previous->_st_running_previous;
        if (slow == fast) return false;
    }
    for (running = thread->_st_running_ensure; running;
         running = running->_st_running_previous) {
        const st_control_scope_t *scope;
        if (running->_st_abi_version != ST_CONTROL_ABI_VERSION ||
            running->_st_state != ENSURE_RUNNING ||
            !scope_contains(thread, running->_st_scope) ||
            !ensure_roots_valid(running) || count == UINT32_MAX)
            return false;
        for (scope = thread->_st_top_scope; scope;
             scope = scope->_st_previous) {
            const st_control_ensure_t *armed;
            for (armed = scope->_st_ensure_top; armed;
                 armed = armed->_st_previous)
                if (armed == running) return false;
        }
        count++;
    }
    return count == thread->_st_callback_depth;
}

static bool control_frame_chain_valid(const st_control_thread_t *thread,
                                      const StFrame *top_frame)
{
    const StFrame *slow = top_frame;
    const StFrame *fast = top_frame;
    const StFrame *frame;
    const st_control_scope_t *scope = thread->_st_top_scope;
    while (fast && fast->caller) {
        slow = slow->caller;
        fast = fast->caller->caller;
        if (slow == fast) return false;
    }
    for (frame = top_frame; frame; frame = frame->caller) {
        bool controlled;
        if (frame->thread != thread->_st_frame_thread_identity ||
            !st_method_descriptor_is_valid(frame->method))
            return false;
        controlled = (frame->method->flags & ST_METHOD_CAN_UNWIND) != 0u;
        if (controlled) {
            if (scope == NULL || scope->_st_frame != frame ||
                frame->home != scope->_st_frame_home_ref ||
                (scope->_st_activation_home != NULL &&
                 scope->_st_activation_home != scope->_st_frame_home_ref))
                return false;
            scope = scope->_st_previous;
        } else if (frame->home != NULL) {
            return false;
        }
    }
    return scope == NULL;
}

static bool visit_root_vector(const st_value_t *roots, size_t root_count,
                              st_control_root_visitor_fn visitor, void *user,
                              size_t *visited)
{
    size_t index;
    for (index = 0u; index < root_count; index++) {
        if (*visited == SIZE_MAX || !visitor(user, &roots[index]))
            return false;
        (*visited)++;
    }
    return true;
}

st_control_status_t st_control_visit_roots(
    const st_control_thread_t *thread, const StFrame *top_frame,
    st_control_root_visitor_fn visitor, void *user, size_t *visited_out)
{
    const st_control_scope_t *scope;
    const st_control_ensure_t *running;
    size_t visited = 0u;
    if (visited_out) *visited_out = 0u;
    if (visitor == NULL || visited_out == NULL)
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    if (!thread_is_ready(thread) || !pending_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_THREAD;
    if (!scope_chain_is_valid(thread) ||
        !control_frame_chain_valid(thread, top_frame))
        return ST_CONTROL_ERR_INVALID_FRAME;
    if (!handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_HANDLER;
    for (scope = thread->_st_top_scope; scope; scope = scope->_st_previous)
        if (!armed_ensure_chain_valid(scope))
            return ST_CONTROL_ERR_INVALID_ENSURE;
    if (!running_ensure_chain_valid(thread))
        return ST_CONTROL_ERR_INVALID_ENSURE;
    if (thread->_st_pending_kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN) {
        if (!scope_chain_contains_home(thread, thread->_st_pending_target))
            return ST_CONTROL_ERR_INVALID_TOKEN;
    }
    if (thread->_st_pending_kind != ST_CONTROL_PENDING_NONE) {
        if (!visit_root_vector(&thread->_st_pending_value, 1u, visitor, user,
                               &visited))
            return ST_CONTROL_ERR_VISITOR_ABORTED;
    }
    for (scope = thread->_st_top_scope; scope; scope = scope->_st_previous) {
        const st_control_ensure_t *armed;
        if (scope->_st_leave_value_active &&
            !visit_root_vector(&scope->_st_leave_value, 1u, visitor, user,
                               &visited))
            return ST_CONTROL_ERR_VISITOR_ABORTED;
        for (armed = scope->_st_ensure_top; armed;
             armed = armed->_st_previous)
            if (!visit_root_vector(armed->_st_roots, armed->_st_root_count,
                                   visitor, user, &visited))
                return ST_CONTROL_ERR_VISITOR_ABORTED;
    }
    for (running = thread->_st_running_ensure; running;
         running = running->_st_running_previous)
        if (!visit_root_vector(running->_st_roots, running->_st_root_count,
                               visitor, user, &visited))
            return ST_CONTROL_ERR_VISITOR_ABORTED;
    for (const st_control_handler_t *handler = thread->_st_handler_top;
         handler != NULL; handler = handler->_st_previous)
        if (!visit_root_vector(handler->_st_roots, handler->_st_root_count,
                               visitor, user, &visited))
            return ST_CONTROL_ERR_VISITOR_ABORTED;
    *visited_out = visited;
    return ST_CONTROL_OK;
}

st_control_status_t st_control_scope_leave(
    st_control_thread_t *thread, st_control_scope_t *scope,
    st_value_t normal_value, st_control_leave_result_t *result_out)
{
    StHomeToken *frame_home;
    StHomeToken *activation_home;
    bool catches_pending;
    if (result_out) *result_out = (st_control_leave_result_t){0};
    if (!thread_is_ready(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!pending_is_valid(thread)) return ST_CONTROL_ERR_INVALID_THREAD;
    if (!result_out || !scope || scope->_st_thread != thread)
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    if (scope->_st_state == SCOPE_LEAVING)
        return ST_CONTROL_ERR_REENTRANT;
    if (scope->_st_state != SCOPE_ACTIVE || thread->_st_top_scope != scope ||
        !scope_chain_is_valid(thread) || !handler_chain_is_valid(thread))
        return ST_CONTROL_ERR_INVALID_SCOPE;
    for (st_control_handler_t *handler = thread->_st_handler_top;
         handler != NULL; handler = handler->_st_previous)
        if (handler->_st_scope == scope)
            return ST_CONTROL_ERR_INVALID_HANDLER;
    scope->_st_state = SCOPE_LEAVING;
    scope->_st_leave_value = normal_value;
    scope->_st_leave_value_active = true;
    while (scope->_st_ensure_top) {
        st_control_ensure_t *ensure_record = scope->_st_ensure_top;
        st_control_status_t status = ensure_run_top(
            thread, scope, ensure_record);
        if (status != ST_CONTROL_OK) {
            scope->_st_leave_value = 0u;
            scope->_st_leave_value_active = false;
            scope->_st_state = SCOPE_ACTIVE;
            return status;
        }
    }
    frame_home = scope->_st_frame_home_ref;
    activation_home = scope->_st_activation_home;
    catches_pending = activation_home &&
        thread->_st_pending_kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN &&
        thread->_st_pending_target == activation_home;
    if (activation_home) {
        uint32_t expected = HOME_ACTIVE;
        if (!atomic_compare_exchange_strong_explicit(
                &activation_home->state, &expected, HOME_RETURNED,
                memory_order_acq_rel, memory_order_acquire)) {
            scope->_st_leave_value = 0u;
            scope->_st_leave_value_active = false;
            scope->_st_state = SCOPE_ACTIVE;
            return ST_CONTROL_ERR_INVALID_TOKEN;
        }
    }
    thread->_st_top_scope = scope->_st_previous;
    scope->_st_frame->home = NULL;
    if (catches_pending) {
        StHomeToken *pending = thread->_st_pending_target;
        result_out->kind = ST_CONTROL_LEAVE_NLR_CAUGHT;
        result_out->value = thread->_st_pending_value;
        thread->_st_pending_target = NULL;
        thread->_st_pending_handler = NULL;
        thread->_st_pending_value = 0u;
        thread->_st_pending_kind = ST_CONTROL_PENDING_NONE;
        thread->_st_pending_exception_class_id = 0u;
        st_home_token_release(pending);
    } else if (thread->_st_pending_kind ==
               ST_CONTROL_PENDING_NON_LOCAL_RETURN) {
        result_out->kind = ST_CONTROL_LEAVE_NLR_PROPAGATE;
        result_out->value = thread->_st_pending_value;
    } else if (thread->_st_pending_kind == ST_CONTROL_PENDING_EXCEPTION) {
        result_out->kind = ST_CONTROL_LEAVE_EXCEPTION_PROPAGATE;
        result_out->value = thread->_st_pending_value;
    } else {
        result_out->kind = ST_CONTROL_LEAVE_NORMAL;
        result_out->value = normal_value;
    }
    scope->_st_thread = NULL;
    scope->_st_frame = NULL;
    scope->_st_previous = NULL;
    scope->_st_frame_home_ref = NULL;
    scope->_st_activation_home = NULL;
    scope->_st_leave_value = 0u;
    scope->_st_leave_value_active = false;
    scope->_st_state = SCOPE_DONE;
    if (frame_home) st_home_token_release(frame_home);
    return ST_CONTROL_OK;
}

size_t st_control_live_token_count(const st_control_thread_t *thread)
{
    return thread_is_ready(thread)
        ? atomic_load_explicit(&thread->_st_live_token_count,
                               memory_order_acquire)
        : 0u;
}

const char *st_control_status_string(st_control_status_t status)
{
    switch (status) {
    case ST_CONTROL_OK: return "ok";
    case ST_CONTROL_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_CONTROL_ERR_INVALID_THREAD: return "invalid control thread";
    case ST_CONTROL_ERR_INVALID_FRAME: return "invalid frame chain";
    case ST_CONTROL_ERR_INVALID_SCOPE: return "invalid control scope";
    case ST_CONTROL_ERR_INVALID_ENSURE: return "invalid ensure record";
    case ST_CONTROL_ERR_INVALID_HANDLER: return "invalid exception handler";
    case ST_CONTROL_ERR_INVALID_EXCEPTION:
        return "invalid exception object or class";
    case ST_CONTROL_ERR_INVALID_TOKEN: return "invalid home token";
    case ST_CONTROL_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_CONTROL_ERR_ID_EXHAUSTED: return "home identity exhausted";
    case ST_CONTROL_ERR_REFCOUNT_OVERFLOW: return "token reference overflow";
    case ST_CONTROL_ERR_BUSY: return "control state is busy";
    case ST_CONTROL_ERR_BLOCK_RETURNED: return "block home has returned";
    case ST_CONTROL_ERR_TARGET_NOT_ACTIVE:
        return "home is not in the active dynamic chain";
    case ST_CONTROL_ERR_REENTRANT: return "reentrant scope mutation";
    case ST_CONTROL_ERR_CALLBACK_STATE:
        return "ensure callback left an unbalanced nested scope";
    case ST_CONTROL_ERR_VISITOR_ABORTED: return "root visitor aborted";
    }
    return "unknown control status";
}
