#include "st_control_bridge.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        failures++;                                                          \
    }                                                                        \
} while (0)

typedef struct {
    StClassDescriptor classes[7];
    StShapeDescriptor shapes[7];
    const StClassDescriptor *class_pointers[7];
    const StShapeDescriptor *shape_pointers[7];
    st_runtime_descriptors_t descriptors;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    st_control_thread_t control;
    StMethodDescriptor method;
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT];
} fixture_t;

static bool fixture_init(fixture_t *fixture,
                         st_control_allocator_t allocator)
{
    static const char *const names[7] = {
        "Object", "Nil", "False", "True", "SmallInteger", "Character",
        "Class"
    };
    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t index = 0u; index < 7u; index++) {
        fixture->classes[index] = (StClassDescriptor) {
            index + 1u, (index == 0u || index == 6u) ? 0u : 1u,
            7u, index + 1u, index == 6u ? ST_CLASS_METACLASS : 0u,
            names[index], strlen(names[index]), NULL, 0u
        };
        fixture->shapes[index] = (StShapeDescriptor) {
            index + 1u, index + 1u, 8u, 24u, 0u, ST_INDEXED_NONE,
            NULL, 0u
        };
        fixture->class_pointers[index] = &fixture->classes[index];
        fixture->shape_pointers[index] = &fixture->shapes[index];
    }
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->class_pointers, 7u,
        fixture->shape_pointers, 7u
    };
    fixture->method = (StMethodDescriptor) {
        ST_METHOD_ABI_VERSION, 1u, 1u, 0u, 0u, 0u, 1u,
        NULL, 0u, 0u, 0u, NULL, 0u, NULL, 0u
    };
    fixture->immediate_ids[ST_AOT_IMMEDIATE_NIL] = 2u;
    fixture->immediate_ids[ST_AOT_IMMEDIATE_FALSE] = 3u;
    fixture->immediate_ids[ST_AOT_IMMEDIATE_TRUE] = 4u;
    fixture->immediate_ids[ST_AOT_IMMEDIATE_SMALL_INTEGER] = 5u;
    fixture->immediate_ids[ST_AOT_IMMEDIATE_CHARACTER] = 6u;
    return st_runtime_descriptors_validate(&fixture->descriptors)
            == ST_RUNTIME_OK
        && st_lookup_context_init(&fixture->lookup, &fixture->descriptors,
                                  (st_lookup_allocator_t){0})
            == ST_LOOKUP_FOUND
        && st_control_thread_init(&fixture->control, &fixture->thread,
                                  allocator) == ST_CONTROL_OK
        && st_aot_thread_init(&fixture->thread, &fixture->lookup,
                              fixture->immediate_ids, NULL,
                              &fixture->control, NULL,
                              NULL, NULL, NULL, NULL);
}

static void fixture_destroy(fixture_t *fixture)
{
    st_aot_thread_destroy(&fixture->thread);
    CHECK(st_control_thread_destroy(&fixture->control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&fixture->lookup);
}

typedef struct {
    unsigned *log;
    size_t *count;
    unsigned marker;
} log_ensure_t;

static void log_ensure(void *user, StFrame *frame,
                       st_control_thread_t *thread)
{
    log_ensure_t *record = user;
    CHECK(frame != NULL && thread != NULL);
    record->log[(*record->count)++] = record->marker;
}

typedef struct {
    StHomeToken *target;
    st_value_t value;
    st_control_status_t status;
    unsigned calls;
} override_ensure_t;

static void override_ensure(void *user, StFrame *frame,
                            st_control_thread_t *thread)
{
    override_ensure_t *override = user;
    (void)frame;
    override->calls++;
    override->status = st_control_non_local_return(
        thread, override->target, override->value);
}

static void test_nested_nlr_and_ensure(void)
{
    fixture_t fixture;
    st_control_scope_t scopes[3];
    st_control_ensure_t ensures[6];
    StFrame frames[3];
    unsigned log[6] = {0};
    size_t log_count = 0u;
    log_ensure_t records[6];
    st_value_t value = 0u;
    CHECK(fixture_init(&fixture, (st_control_allocator_t){0}));
    memset(frames, 0, sizeof(frames));
    for (size_t index = 0u; index < 3u; index++) {
        frames[index].thread = &fixture.thread;
        frames[index].method = &fixture.method;
        frames[index].receiver = st_value_true();
        frames[index].caller = index == 0u ? NULL : &frames[index - 1u];
        CHECK(st_aot_control_scope_enter(
                  &frames[index], &scopes[index], index == 0u ? 1u : 0u)
              == ST_CONTROL_OK);
        for (size_t item = 0u; item < 2u; item++) {
            size_t slot = index * 2u + item;
            records[slot] = (log_ensure_t) {
                log, &log_count, (unsigned)slot + 1u
            };
            st_control_ensure_init(&ensures[slot]);
            CHECK(st_control_ensure_push(
                      &fixture.control, &scopes[index], &ensures[slot],
                      log_ensure, &records[slot]) == ST_CONTROL_OK);
        }
    }
    CHECK(frames[0].home != NULL);
    CHECK(st_aot_control_non_local_return(
              &frames[2], frames[0].home, UINT64_C(0x123)) == ST_CONTROL_OK);
    CHECK(st_aot_control_scope_leave(
              &frames[2], &scopes[2], st_value_nil(), &value)
          == ST_CONTROL_OK && value == UINT64_C(0x123));
    CHECK(st_aot_control_scope_leave(
              &frames[1], &scopes[1], st_value_false(), &value)
          == ST_CONTROL_OK && value == UINT64_C(0x123));
    CHECK(st_aot_control_scope_leave(
              &frames[0], &scopes[0], st_value_true(), &value)
          == ST_CONTROL_OK && value == UINT64_C(0x123));
    static const unsigned expected[6] = {6u, 5u, 4u, 3u, 2u, 1u};
    CHECK(log_count == 6u && memcmp(log, expected, sizeof(expected)) == 0);
    CHECK(frames[0].home == NULL && st_control_live_token_count(
              &fixture.control) == 0u);
    fixture_destroy(&fixture);
}

static void test_normal_return_override_and_returned_home(void)
{
    fixture_t fixture;
    StFrame outer = {0}, inner = {0};
    st_control_scope_t outer_scope, inner_scope;
    st_control_ensure_t ensure;
    override_ensure_t override;
    StHomeToken *retained;
    st_value_t value = 0u;
    CHECK(fixture_init(&fixture, (st_control_allocator_t){0}));
    outer.thread = &fixture.thread;
    outer.method = &fixture.method;
    outer.receiver = st_value_true();
    CHECK(st_aot_control_scope_enter(&outer, &outer_scope, 1u)
          == ST_CONTROL_OK);
    retained = outer.home;
    CHECK(st_home_token_retain(retained) == ST_CONTROL_OK);
    inner.thread = &fixture.thread;
    inner.method = &fixture.method;
    inner.caller = &outer;
    inner.receiver = st_value_false();
    CHECK(st_aot_control_scope_enter(&inner, &inner_scope, 0u)
          == ST_CONTROL_OK);
    override = (override_ensure_t) {
        retained, UINT64_C(0xbbb), ST_CONTROL_ERR_INVALID_ARGUMENT, 0u
    };
    st_control_ensure_init(&ensure);
    CHECK(st_control_ensure_push(&fixture.control, &inner_scope, &ensure,
                                 override_ensure, &override)
          == ST_CONTROL_OK);
    CHECK(st_aot_control_non_local_return(
              &inner, retained, UINT64_C(0xaaa)) == ST_CONTROL_OK);
    CHECK(st_aot_control_scope_leave(
              &inner, &inner_scope, st_value_nil(), &value)
          == ST_CONTROL_OK && value == UINT64_C(0xbbb));
    CHECK(override.calls == 1u && override.status == ST_CONTROL_OK);
    CHECK(st_aot_control_scope_leave(
              &outer, &outer_scope, st_value_true(), &value)
          == ST_CONTROL_OK && value == UINT64_C(0xbbb));
    CHECK(st_aot_control_non_local_return(
              &outer, retained, UINT64_C(0xccc))
          == ST_CONTROL_ERR_BLOCK_RETURNED);
    st_home_token_release(retained);

    st_control_scope_t normal_scope;
    CHECK(st_aot_control_scope_enter(&outer, &normal_scope, 0u)
          == ST_CONTROL_OK);
    CHECK(st_aot_control_scope_leave(
              &outer, &normal_scope, UINT64_C(0xddd), &value)
          == ST_CONTROL_OK && value == UINT64_C(0xddd));
    fixture_destroy(&fixture);
}

typedef struct { bool fail; } failing_allocator_t;

static void *failing_allocate(void *user, size_t size)
{
    failing_allocator_t *allocator = user;
    return allocator->fail ? NULL : malloc(size);
}

static void failing_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void test_invalid_sidecars_and_transactional_oom(void)
{
    fixture_t fixture;
    st_control_scope_t scope;
    StFrame frame = {0};
    CHECK(fixture_init(&fixture, (st_control_allocator_t){0}));
    frame.thread = &fixture.thread;
    frame.method = &fixture.method;
    frame.receiver = st_value_nil();
    st_control_thread_t *saved = fixture.thread.control;
    fixture.thread.control = NULL;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u)
          == ST_CONTROL_ERR_INVALID_THREAD);
    st_control_thread_t corrupt = {0};
    fixture.thread.control = &corrupt;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u)
          == ST_CONTROL_ERR_INVALID_THREAD);
    fixture.thread.control = saved;
    fixture_destroy(&fixture);

    failing_allocator_t allocator = { true };
    CHECK(fixture_init(&fixture, (st_control_allocator_t) {
        failing_allocate, failing_deallocate, &allocator
    }));
    frame = (StFrame) {
        .thread = &fixture.thread, .method = &fixture.method,
        .receiver = st_value_nil()
    };
    CHECK(st_aot_control_scope_enter(&frame, &scope, 1u)
          == ST_CONTROL_ERR_OUT_OF_MEMORY);
    CHECK(frame.home == NULL && fixture.control._st_top_scope == NULL
          && st_control_live_token_count(&fixture.control) == 0u);
    fixture_destroy(&fixture);
}

static void test_fatal_contract(void)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)st_aot_control_contract_violation(
            ST_CONTROL_ERR_INVALID_THREAD, NULL);
        _exit(91);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}

int main(void)
{
    test_nested_nlr_and_ensure();
    test_normal_return_override_and_returned_home();
    test_invalid_sidecars_and_transactional_oom();
    test_fatal_contract();
    if (failures != 0u) {
        fprintf(stderr, "%u control bridge regression(s) failed\n", failures);
        return 1;
    }
    puts("Smalltalk AOT control bridge: PASS");
    return 0;
}
