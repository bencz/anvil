#include "st_control.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct {
    unsigned order[16];
    size_t count;
    StFrame *expected_frame;
} order_log_t;

typedef struct {
    order_log_t *log;
    unsigned tag;
} order_action_t;

static void record_ensure(void *user, StFrame *frame,
                          st_control_thread_t *thread)
{
    order_action_t *action = user;
    (void)thread;
    CHECK(action && action->log && frame == action->log->expected_frame);
    if (action && action->log && action->log->count < 16u)
        action->log->order[action->log->count++] = action->tag;
}

static void test_nlr_and_lifo(void)
{
    int identity;
    st_control_thread_t thread = {0};
    StFrame root_frame = { .thread = &identity };
    StFrame child_frame = { .thread = &identity, .caller = &root_frame };
    st_control_scope_t root, child;
    st_control_ensure_t first, second, third;
    StHomeToken *home = NULL;
    st_home_token_id_t id = {0};
    st_control_leave_result_t leave;
    st_control_pending_info_t pending;
    order_log_t log = {0};
    order_action_t one = { &log, 1u }, two = { &log, 2u };
    order_action_t three = { &log, 3u };
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&root);
    CHECK(st_control_scope_enter(&thread, &root, &root_frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &root, &home)
          == ST_CONTROL_OK);
    CHECK(home && root_frame.home == home && st_home_token_is_active(home));
    CHECK(st_home_token_id(home, &id) == ST_CONTROL_OK && id.thread_id != 0u &&
          id.activation_id != 0u);
    CHECK(st_home_token_retain(home) == ST_CONTROL_OK); /* captured closure */
    st_control_ensure_init(&first);
    st_control_ensure_init(&second);
    log.expected_frame = &root_frame;
    CHECK(st_control_ensure_push(&thread, &root, &first, record_ensure, &one)
          == ST_CONTROL_OK);
    CHECK(st_control_ensure_push(&thread, &root, &first, record_ensure, &one)
          == ST_CONTROL_ERR_INVALID_ENSURE);
    CHECK(st_control_ensure_push(&thread, &root, &second, record_ensure, &two)
          == ST_CONTROL_OK);

    child_frame.home = home;
    st_control_scope_init(&child);
    CHECK(st_control_scope_enter(&thread, &child, &child_frame)
          == ST_CONTROL_OK);
    st_control_ensure_init(&third);
    log.expected_frame = &child_frame;
    CHECK(st_control_ensure_push(&thread, &child, &third, record_ensure, &three)
          == ST_CONTROL_OK);
    CHECK(st_control_non_local_return(&thread, home, UINT64_C(0xabc))
          == ST_CONTROL_OK);
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK &&
          pending.kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN &&
          pending.target.thread_id == id.thread_id &&
          pending.target.activation_id == id.activation_id &&
          pending.value == UINT64_C(0xabc));
    CHECK(st_control_non_local_return(&thread, home, UINT64_C(0xdef))
          == ST_CONTROL_ERR_BUSY);
    CHECK(st_control_scope_leave(&thread, &child, 7u, &leave) == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NLR_PROPAGATE &&
          leave.value == UINT64_C(0xabc));
    CHECK(log.count == 1u && log.order[0] == 3u && child_frame.home == NULL);
    log.expected_frame = &root_frame;
    CHECK(st_control_scope_leave(&thread, &root, 8u, &leave) == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NLR_CAUGHT &&
          leave.value == UINT64_C(0xabc));
    CHECK(log.count == 3u && log.order[1] == 2u && log.order[2] == 1u);
    CHECK(root_frame.home == NULL && !st_home_token_is_active(home));
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK &&
          pending.kind == ST_CONTROL_PENDING_NONE);
    CHECK(st_control_non_local_return(&thread, home, 1u)
          == ST_CONTROL_ERR_BLOCK_RETURNED);
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK &&
          pending.kind == ST_CONTROL_PENDING_NONE);
    CHECK(st_control_live_token_count(&thread) == 1u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_ERR_BUSY);
    st_home_token_release(home);
    CHECK(st_control_live_token_count(&thread) == 0u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    st_control_scope_t *scope;
    StHomeToken *replacement;
    st_control_status_t status;
    unsigned calls;
} replacement_context_t;

static void replace_pending(void *user, StFrame *frame,
                            st_control_thread_t *thread)
{
    replacement_context_t *context = user;
    (void)frame;
    context->calls++;
    context->status = st_control_non_local_return(
        thread, context->replacement, UINT64_C(0x222));
}

static void test_ensure_replaces_pending(void)
{
    int identity;
    st_control_thread_t thread = {0};
    StFrame root_frame = { .thread = &identity };
    StFrame child_frame = { .thread = &identity, .caller = &root_frame };
    st_control_scope_t root, child;
    st_control_ensure_t ensure_record;
    StHomeToken *root_home = NULL, *child_home = NULL;
    st_control_leave_result_t leave;
    replacement_context_t context = {0};
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&root);
    st_control_scope_init(&child);
    CHECK(st_control_scope_enter(&thread, &root, &root_frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &root, &root_home)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_enter(&thread, &child, &child_frame)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &child, &child_home)
          == ST_CONTROL_OK);
    context.scope = &child;
    context.replacement = child_home;
    st_control_ensure_init(&ensure_record);
    CHECK(st_control_ensure_push(&thread, &child, &ensure_record,
                                 replace_pending, &context) == ST_CONTROL_OK);
    CHECK(st_control_non_local_return(&thread, root_home, UINT64_C(0x111))
          == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &child, 0u, &leave) == ST_CONTROL_OK);
    CHECK(context.calls == 1u && context.status == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NLR_CAUGHT &&
          leave.value == UINT64_C(0x222));
    CHECK(st_control_scope_leave(&thread, &root, UINT64_C(0x333), &leave)
          == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NORMAL &&
          leave.value == UINT64_C(0x333));
    CHECK(st_control_live_token_count(&thread) == 0u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    int *identity;
    st_control_scope_t *outer;
    StFrame nested_frame;
    st_control_scope_t nested_scope;
    st_control_ensure_t nested_ensure;
    st_control_ensure_t illegal_ensure;
    st_control_status_t push_same;
    st_control_status_t leave_same;
    st_control_status_t nested_enter;
    st_control_status_t nested_leave;
    unsigned nested_callbacks;
} nested_context_t;

static void nested_leaf_ensure(void *user, StFrame *frame,
                               st_control_thread_t *thread)
{
    nested_context_t *context = user;
    (void)frame;
    (void)thread;
    context->nested_callbacks++;
}

static void nested_ensure(void *user, StFrame *frame,
                          st_control_thread_t *thread)
{
    nested_context_t *context = user;
    st_control_leave_result_t leave;
    context->nested_frame = (StFrame) {
        .thread = context->identity,
        .caller = frame
    };
    st_control_ensure_init(&context->illegal_ensure);
    context->push_same = st_control_ensure_push(
        thread, context->outer, &context->illegal_ensure,
        nested_leaf_ensure, context);
    context->leave_same = st_control_scope_leave(
        thread, context->outer, 0u, &leave);
    st_control_scope_init(&context->nested_scope);
    context->nested_enter = st_control_scope_enter(
        thread, &context->nested_scope, &context->nested_frame);
    st_control_ensure_init(&context->nested_ensure);
    if (context->nested_enter == ST_CONTROL_OK)
        CHECK(st_control_ensure_push(thread, &context->nested_scope,
              &context->nested_ensure, nested_leaf_ensure, context)
              == ST_CONTROL_OK);
    context->nested_leave = context->nested_enter == ST_CONTROL_OK
        ? st_control_scope_leave(thread, &context->nested_scope, 44u, &leave)
        : context->nested_enter;
    CHECK(context->nested_leave != ST_CONTROL_OK ||
          (leave.kind == ST_CONTROL_LEAVE_NORMAL && leave.value == 44u));
}

static void test_nested_callback_and_reentrancy(void)
{
    int identity;
    st_control_thread_t thread = {0};
    StFrame frame = { .thread = &identity };
    st_control_scope_t scope;
    st_control_ensure_t ensure_record;
    st_control_leave_result_t leave;
    nested_context_t context = { .identity = &identity, .outer = &scope };
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    st_control_ensure_init(&ensure_record);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_ensure_push(&thread, &scope, &ensure_record,
                                 nested_ensure, &context) == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &scope, 55u, &leave) == ST_CONTROL_OK);
    CHECK(context.push_same == ST_CONTROL_ERR_REENTRANT &&
          context.leave_same == ST_CONTROL_ERR_REENTRANT);
    CHECK(context.nested_enter == ST_CONTROL_OK &&
          context.nested_leave == ST_CONTROL_OK &&
          context.nested_callbacks == 1u);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NORMAL && leave.value == 55u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    int *identity;
    st_control_scope_t *outer;
    StFrame nested_frame;
    st_control_scope_t nested_scope;
    st_control_status_t enter_status;
    unsigned calls;
} leak_context_t;

static void leave_nested_open(void *user, StFrame *frame,
                              st_control_thread_t *thread)
{
    leak_context_t *context = user;
    context->calls++;
    context->nested_frame = (StFrame) {
        .thread = context->identity,
        .caller = frame
    };
    st_control_scope_init(&context->nested_scope);
    context->enter_status = st_control_scope_enter(
        thread, &context->nested_scope, &context->nested_frame);
}

static void test_unbalanced_callback_recovery(void)
{
    int identity;
    st_control_thread_t thread = {0};
    StFrame frame = { .thread = &identity };
    st_control_scope_t scope;
    st_control_ensure_t ensure_record;
    st_control_leave_result_t leave;
    leak_context_t context = { .identity = &identity, .outer = &scope };
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    st_control_ensure_init(&ensure_record);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_ensure_push(&thread, &scope, &ensure_record,
              leave_nested_open, &context) == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &scope, 1u, &leave)
          == ST_CONTROL_ERR_CALLBACK_STATE);
    CHECK(context.calls == 1u && context.enter_status == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &context.nested_scope, 2u, &leave)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &scope, 3u, &leave) == ST_CONTROL_OK);
    CHECK(context.calls == 1u && leave.kind == ST_CONTROL_LEAVE_NORMAL &&
          leave.value == 3u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    bool fail;
    size_t allocations;
    size_t frees;
} allocator_state_t;

static void *test_allocate(void *user, size_t size)
{
    allocator_state_t *state = user;
    void *memory;
    if (state->fail) return NULL;
    memory = malloc(size);
    if (memory) state->allocations++;
    return memory;
}

static void test_deallocate(void *user, void *pointer)
{
    allocator_state_t *state = user;
    if (pointer) state->frees++;
    free(pointer);
}

static void test_faults_ids_and_malformed(void)
{
    int identity, foreign_identity;
    allocator_state_t allocator_state = { .fail = true };
    st_control_allocator_t allocator = {
        test_allocate, test_deallocate, &allocator_state
    };
    st_control_thread_t thread = {0}, foreign = {0};
    StFrame frame = { .thread = &identity };
    StFrame bad_frame = { .thread = &foreign_identity };
    st_control_scope_t scope, bad_scope;
    StHomeToken *home = NULL, *foreign_home = NULL;
    st_control_leave_result_t leave;
    uint64_t before_id;
    CHECK(st_control_thread_init(&thread, &identity,
          (st_control_allocator_t){ test_allocate, NULL, &allocator_state })
          == ST_CONTROL_ERR_INVALID_ARGUMENT);
    CHECK(st_control_thread_init(&thread, &identity, allocator) == ST_CONTROL_OK);
    thread._st_pending_kind = UINT32_C(99);
    {
        st_control_pending_info_t malformed_pending;
        CHECK(st_control_pending_get(&thread, &malformed_pending)
              == ST_CONTROL_ERR_INVALID_THREAD);
    }
    thread._st_pending_kind = ST_CONTROL_PENDING_NONE;
    bad_frame.thread = &identity;
    bad_frame.caller = &bad_frame;
    st_control_scope_init(&bad_scope);
    CHECK(st_control_scope_enter(&thread, &bad_scope, &bad_frame)
          == ST_CONTROL_ERR_INVALID_FRAME);
    bad_frame.thread = &foreign_identity;
    bad_frame.caller = NULL;
    st_control_scope_init(&bad_scope);
    CHECK(st_control_scope_enter(&thread, &bad_scope, &bad_frame)
          == ST_CONTROL_ERR_INVALID_FRAME);
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    scope._st_previous = &scope;
    CHECK(st_control_scope_leave(&thread, &scope, 0u, &leave)
          == ST_CONTROL_ERR_INVALID_SCOPE);
    scope._st_previous = NULL;
    before_id = thread._st_next_activation_id;
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_ERR_OUT_OF_MEMORY);
    CHECK(home == NULL && frame.home == NULL &&
          thread._st_next_activation_id == before_id &&
          st_control_live_token_count(&thread) == 0u);
    allocator_state.fail = false;
    thread._st_next_activation_id = UINT64_MAX;
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_ERR_ID_EXHAUSTED);
    CHECK(home == NULL && frame.home == NULL &&
          st_control_live_token_count(&thread) == 0u);
    thread._st_next_activation_id = UINT64_MAX - 1u;
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_OK);
    CHECK(thread._st_next_activation_id == UINT64_MAX);
    CHECK(st_control_thread_init(&foreign, &foreign_identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    {
        StFrame foreign_frame = { .thread = &foreign_identity };
        st_control_scope_t foreign_scope;
        st_control_scope_init(&foreign_scope);
        CHECK(st_control_scope_enter(&foreign, &foreign_scope, &foreign_frame)
              == ST_CONTROL_OK);
        CHECK(st_control_scope_establish_home(&foreign, &foreign_scope,
                                              &foreign_home) == ST_CONTROL_OK);
        CHECK(st_control_non_local_return(&thread, foreign_home, 1u)
              == ST_CONTROL_ERR_INVALID_TOKEN);
        CHECK(st_control_scope_leave(&foreign, &foreign_scope, 0u, &leave)
              == ST_CONTROL_OK);
    }
    CHECK(st_control_thread_destroy(&foreign) == ST_CONTROL_OK);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_ERR_BUSY);
    CHECK(st_control_scope_leave(&thread, &scope, 9u, &leave) == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NORMAL && leave.value == 9u);
    CHECK(allocator_state.allocations == allocator_state.frees);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    StHomeToken *token;
    st_home_token_id_t expected;
    unsigned iterations;
    _Atomic unsigned errors;
} retain_worker_t;

static void *retain_worker(void *argument)
{
    retain_worker_t *worker = argument;
    unsigned index;
    for (index = 0u; index < worker->iterations; index++) {
        st_home_token_id_t id;
        if (st_home_token_retain(worker->token) != ST_CONTROL_OK ||
            st_home_token_id(worker->token, &id) != ST_CONTROL_OK ||
            id.thread_id != worker->expected.thread_id ||
            id.activation_id != worker->expected.activation_id ||
            !st_home_token_is_active(worker->token))
            atomic_fetch_add_explicit(&worker->errors, 1u,
                                      memory_order_relaxed);
        else
            st_home_token_release(worker->token);
    }
    return NULL;
}

static void test_concurrent_token_ownership(void)
{
    enum { WORKERS = 4, ITERATIONS = 50000 };
    int identity;
    st_control_thread_t thread = {0};
    StFrame frame = { .thread = &identity };
    st_control_scope_t scope;
    StHomeToken *home = NULL;
    st_home_token_id_t id;
    st_control_leave_result_t leave;
    retain_worker_t worker;
    pthread_t threads[WORKERS];
    size_t index;
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_OK);
    CHECK(st_home_token_id(home, &id) == ST_CONTROL_OK);
    worker = (retain_worker_t) { home, id, ITERATIONS, ATOMIC_VAR_INIT(0u) };
    for (index = 0u; index < WORKERS; index++)
        CHECK(pthread_create(&threads[index], NULL, retain_worker, &worker) == 0);
    for (index = 0u; index < WORKERS; index++)
        CHECK(pthread_join(threads[index], NULL) == 0);
    CHECK(atomic_load_explicit(&worker.errors, memory_order_relaxed) == 0u);
    CHECK(st_control_live_token_count(&thread) == 1u);
    CHECK(st_control_scope_leave(&thread, &scope, 0u, &leave) == ST_CONTROL_OK);
    CHECK(st_control_live_token_count(&thread) == 0u);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

static void test_thread_reuse_has_no_token_aba(void)
{
    int identity;
    st_control_thread_t thread = {0};
    StFrame frame = { .thread = &identity };
    st_control_scope_t scope;
    StHomeToken *home;
    st_home_token_id_t first, second;
    st_control_leave_result_t leave;
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_OK);
    CHECK(st_home_token_id(home, &first) == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&thread, &scope, 0u, &leave) == ST_CONTROL_OK);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);

    frame = (StFrame) { .thread = &identity };
    CHECK(st_control_thread_init(&thread, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&thread, &scope, &home)
          == ST_CONTROL_OK);
    CHECK(st_home_token_id(home, &second) == ST_CONTROL_OK);
    CHECK(first.thread_id != second.thread_id &&
          first.activation_id == second.activation_id);
    CHECK(st_control_scope_leave(&thread, &scope, 0u, &leave) == ST_CONTROL_OK);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

typedef struct {
    st_value_t class_objects[6];
    uint32_t superclass[6];
} exception_classes_t;

static bool resolve_class_object(
    void *user, st_value_t class_object, uint32_t *class_id_out)
{
    exception_classes_t *classes = user;
    for (uint32_t class_id = 1u; class_id < 6u; class_id++) {
        if (classes->class_objects[class_id] == class_object) {
            *class_id_out = class_id;
            return true;
        }
    }
    return false;
}

static bool exception_matches(
    void *user, uint32_t exception_class_id, uint32_t caught_class_id)
{
    exception_classes_t *classes = user;
    uint32_t cursor = exception_class_id;
    for (size_t hops = 0u; cursor != 0u && hops < 6u; hops++) {
        if (cursor == caught_class_id) return true;
        cursor = classes->superclass[cursor];
    }
    return false;
}

typedef struct {
    exception_classes_t *classes;
    st_value_t replacement;
    uint32_t replacement_class_id;
    st_control_status_t status;
    unsigned calls;
} exception_replacement_t;

static void replace_with_exception(
    void *user, StFrame *frame, st_control_thread_t *thread)
{
    exception_replacement_t *replacement = user;
    (void)frame;
    replacement->calls++;
    replacement->status = st_control_exception_signal(
        thread, replacement->replacement,
        replacement->replacement_class_id,
        exception_matches, replacement->classes);
}

static void test_exception_handlers_and_ensure(void)
{
    int identity;
    exception_classes_t classes = {
        .class_objects = {
            0u, UINT64_C(0x1000), UINT64_C(0x2000),
            UINT64_C(0x3000), UINT64_C(0x4000), UINT64_C(0x5000)
        },
        .superclass = { 0u, 0u, 1u, 2u, 2u, 1u }
    };
    st_control_thread_t thread = {0};
    StFrame frame = { .thread = &identity };
    st_control_scope_t scope;
    st_control_handler_t outer, inner, malformed;
    st_control_ensure_t ensure_record;
    st_control_pending_info_t pending;
    st_control_leave_result_t leave;
    st_value_t outer_roots[2] = {
        UINT64_C(0x6000), classes.class_objects[2]
    };
    st_value_t inner_roots[2] = {
        UINT64_C(0x7000), classes.class_objects[3]
    };
    st_value_t caught = (st_value_t)ST_VALUE_INVALID;
    bool targeted = false;

    CHECK(st_control_thread_init(
              &thread, &identity, (st_control_allocator_t){0})
          == ST_CONTROL_OK);
    CHECK(st_control_exception_configure(
              &thread, resolve_class_object, &classes) == ST_CONTROL_OK);
    CHECK(st_control_exception_configure(
              &thread, resolve_class_object, &classes) == ST_CONTROL_ERR_BUSY);
    uint32_t resolved = 0u;
    CHECK(st_control_exception_class_resolve(
              &thread, classes.class_objects[3], &resolved) == ST_CONTROL_OK
          && resolved == 3u);
    CHECK(st_control_exception_class_resolve(
              &thread, UINT64_C(0x9000), &resolved)
          == ST_CONTROL_ERR_INVALID_EXCEPTION);

    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&thread, &scope, &frame) == ST_CONTROL_OK);
    st_control_handler_init(&outer);
    st_control_handler_init(&inner);
    CHECK(st_control_handler_push(
              &thread, &scope, &outer, 2u, outer_roots, 2u)
          == ST_CONTROL_OK);
    CHECK(st_control_handler_push(
              &thread, &scope, &inner, 3u, inner_roots, 2u)
          == ST_CONTROL_OK);

    st_value_t error_instance = UINT64_C(0x8000);
    CHECK(st_control_exception_signal(
              &thread, error_instance, 4u, exception_matches, &classes)
          == ST_CONTROL_OK);
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && pending.value == error_instance
          && pending.exception_class_id == 4u
          && pending.has_handler);
    CHECK(st_control_handler_is_target(&thread, &inner, &targeted)
          == ST_CONTROL_OK && !targeted);
    CHECK(st_control_handler_is_target(&thread, &outer, &targeted)
          == ST_CONTROL_OK && targeted);
    CHECK(st_control_handler_pop(&thread, &inner) == ST_CONTROL_OK);
    CHECK(st_control_handler_consume_exception(
              &thread, &outer, &caught) == ST_CONTROL_OK
          && caught == error_instance);
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_NONE);

    /* A pending exception is replaced only while an ensure callback is
     * active; the ensure itself runs once and is no longer reusable. */
    st_control_ensure_init(&ensure_record);
    exception_replacement_t replacement = {
        &classes, UINT64_C(0xa000), 3u, ST_CONTROL_ERR_BUSY, 0u
    };
    CHECK(st_control_ensure_push(
              &thread, &scope, &ensure_record,
              replace_with_exception, &replacement) == ST_CONTROL_OK);
    CHECK(st_control_exception_signal(
              &thread, UINT64_C(0xb000), 4u,
              exception_matches, &classes) == ST_CONTROL_OK);
    CHECK(st_control_exception_signal(
              &thread, UINT64_C(0xc000), 3u,
              exception_matches, &classes) == ST_CONTROL_ERR_BUSY);
    CHECK(st_control_ensure_run(&thread, &scope, &ensure_record)
          == ST_CONTROL_OK);
    CHECK(replacement.calls == 1u && replacement.status == ST_CONTROL_OK);
    CHECK(st_control_ensure_run(&thread, &scope, &ensure_record)
          == ST_CONTROL_ERR_INVALID_ENSURE);
    CHECK(st_control_pending_get(&thread, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && pending.value == replacement.replacement
          && pending.exception_class_id == 3u
          && !pending.has_handler);

    st_control_handler_init(&malformed);
    malformed._st_state = UINT32_C(0xdeadbeef);
    CHECK(st_control_handler_push(
              &thread, &scope, &malformed, 2u, NULL, 0u)
          == ST_CONTROL_ERR_INVALID_HANDLER);
    CHECK(st_control_scope_leave(&thread, &scope, st_value_nil(), &leave)
          == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_EXCEPTION_PROPAGATE
          && leave.value == replacement.replacement);
    CHECK(st_control_pending_clear(&thread) == ST_CONTROL_OK);
    CHECK(st_control_thread_destroy(&thread) == ST_CONTROL_OK);
}

int main(void)
{
    test_nlr_and_lifo();
    test_ensure_replaces_pending();
    test_nested_callback_and_reentrancy();
    test_unbalanced_callback_recovery();
    test_faults_ids_and_malformed();
    test_concurrent_token_ownership();
    test_thread_reuse_has_no_token_aba();
    test_exception_handlers_and_ensure();
    CHECK(strcmp(st_control_status_string(ST_CONTROL_ERR_BLOCK_RETURNED),
                 "block home has returned") == 0);
    if (failures) {
        fprintf(stderr, "smalltalk AOT control: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk AOT control: PASS");
    return EXIT_SUCCESS;
}
