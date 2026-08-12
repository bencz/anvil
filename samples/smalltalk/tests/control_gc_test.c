#include "st_control_roots.h"
#include "st_heap.h"
#include "st_lookup.h"
#include "st_send_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                        \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

enum { CLASS_COUNT = 7 };

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live_blocks;
} heap_allocator_t;

static void *heap_allocate(void *user, size_t alignment, size_t size)
{
    heap_allocator_t *allocator = user;
    void *block;
    if (allocator->calls++ == allocator->fail_at) return NULL;
    block = aligned_alloc(alignment, size);
    if (block) allocator->live_blocks++;
    return block;
}

static void heap_deallocate(void *user, void *pointer, size_t alignment,
                            size_t size)
{
    heap_allocator_t *allocator = user;
    (void)alignment;
    (void)size;
    CHECK(pointer != NULL && allocator->live_blocks != 0u);
    if (pointer != NULL && allocator->live_blocks != 0u)
        allocator->live_blocks--;
    free(pointer);
}

typedef struct {
    StClassDescriptor classes_storage[CLASS_COUNT];
    StShapeDescriptor shapes_storage[CLASS_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[CLASS_COUNT];
    st_runtime_descriptors_t descriptors;
    uint64_t root_bits;
    st_root_map_t root_map;
    st_unwind_region_t nlr_region;
    StMethodDescriptor unwind_method;
} fixture_t;

static void fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Nil", "False", "True", "SmallInteger", "Character",
        "Class"
    };
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    for (index = 0u; index < CLASS_COUNT; index++) {
        fixture->classes_storage[index] = (StClassDescriptor) {
            .class_id = (uint32_t)index + 1u,
            .superclass_id = index == 0u || index == CLASS_COUNT - 1u
                ? 0u : 1u,
            .metaclass_id = CLASS_COUNT,
            .default_shape_id = (uint32_t)index + 1u,
            .flags = index == CLASS_COUNT - 1u ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shapes_storage[index] = (StShapeDescriptor) {
            .shape_id = (uint32_t)index + 1u,
            .class_id = (uint32_t)index + 1u,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->classes_storage[index];
        fixture->shapes[index] = &fixture->shapes_storage[index];
    }
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, CLASS_COUNT, fixture->shapes, CLASS_COUNT
    };
    fixture->root_bits = UINT64_C(1);
    fixture->root_map = (st_root_map_t) {
        .safepoint_id = 1u,
        .root_count = 1u,
        .bitmap_word_count = 1u,
        .live_root_bitmap = &fixture->root_bits
    };
    fixture->nlr_region = (st_unwind_region_t) {
        .start_pc_offset = 0u,
        .end_pc_offset = 1u,
        .landing_pad_pc_offset = 0u,
        .kind = ST_UNWIND_NON_LOCAL_RETURN
    };
    fixture->unwind_method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = 1u,
        .frame_root_capacity = 1u,
        .flags = ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN,
        .code_size = 1u,
        .root_maps = &fixture->root_map,
        .root_map_count = 1u,
        .unwind_regions = &fixture->nlr_region,
        .unwind_region_count = 1u
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
    CHECK(st_method_descriptor_is_valid(&fixture->unwind_method));
}

static st_value_t object_new(st_heap_t *heap)
{
    st_value_t value = 0u;
    CHECK(st_heap_allocate(heap, 1u, 1u, 0u, 0u, 0u, &value) == ST_HEAP_OK);
    return value;
}

typedef struct {
    st_heap_t *heap;
    StFrame *top_frame;
    StHomeToken *replacement_home;
    st_value_t *ensure_roots;
    st_value_t old_pending;
    st_value_t normal_value;
    st_value_t garbage;
    st_heap_status_t first_gc;
    st_heap_status_t second_gc;
    st_control_status_t replacement_status;
    unsigned calls;
} ensure_gc_context_t;

static void ensure_collect_and_replace(void *user, StFrame *active_frame,
                                       st_control_thread_t *control)
{
    ensure_gc_context_t *context = user;
    st_heap_collection_stats_t stats;
    CHECK(active_frame == context->top_frame);
    context->calls++;
    context->first_gc = st_heap_collect(context->heap, context->top_frame,
                                        NULL, 0u, &stats);
    CHECK(context->first_gc == ST_HEAP_OK);
    CHECK(st_heap_contains(context->heap, context->old_pending));
    CHECK(st_heap_contains(context->heap, context->normal_value));
    CHECK(st_heap_contains(context->heap, context->ensure_roots[0]));
    CHECK(st_heap_contains(context->heap, context->ensure_roots[1]));
    CHECK(!st_heap_contains(context->heap, context->garbage));
    context->replacement_status = st_control_non_local_return(
        control, context->replacement_home, context->ensure_roots[1]);
    context->ensure_roots[1] = st_value_nil();
    context->second_gc = st_heap_collect(context->heap, context->top_frame,
                                         NULL, 0u, &stats);
    CHECK(context->second_gc == ST_HEAP_OK);
    CHECK(!st_heap_contains(context->heap, context->old_pending));
    CHECK(st_heap_contains(context->heap, context->normal_value));
    CHECK(st_heap_contains(context->heap, context->ensure_roots[0]));
}

typedef struct {
    st_heap_t *heap;
    StFrame *frame;
    st_heap_status_t status;
} invalid_gc_context_t;

static void ensure_invalid_root_gc(void *user, StFrame *active_frame,
                                   st_control_thread_t *control)
{
    invalid_gc_context_t *context = user;
    st_heap_collection_stats_t stats;
    (void)control;
    CHECK(active_frame == context->frame);
    context->status = st_heap_collect(context->heap, context->frame,
                                      NULL, 0u, &stats);
}

static void test_gc_pending_and_ensure_roots(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_lookup_context_t lookup = {0};
    st_control_thread_t control = {0};
    st_aot_thread_t aot = {0};
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {2, 3, 4, 5, 6};
    st_value_t root_roots[1] = {0};
    st_value_t child_roots[1] = {0};
    StFrame root_frame, child_frame, invalid_frame;
    st_control_scope_t root_scope, child_scope, invalid_scope;
    st_control_ensure_t ensure_record, invalid_ensure;
    StHomeToken *root_home = NULL, *child_home = NULL;
    st_control_leave_result_t leave;
    st_value_t ensure_roots[2];
    ensure_gc_context_t ensure_context;
    invalid_gc_context_t invalid_context;
    st_value_t invalid_root;
    st_value_t invalid_survivor;
    st_heap_collection_stats_t stats;
    st_control_status_t visit_status;
    size_t visited = 0u;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_lookup_context_init(&lookup, &fixture.descriptors,
                                 (st_lookup_allocator_t){0})
          == ST_LOOKUP_FOUND);
    CHECK(st_control_thread_init(&control, &aot,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    CHECK(st_aot_thread_init(&aot, &lookup, immediate_ids, NULL, &control,
                             NULL, NULL, NULL, NULL, NULL));
    root_roots[0] = st_value_nil();
    root_frame = (StFrame) {
        .thread = &aot, .method = &fixture.unwind_method,
        .receiver = st_value_nil(), .roots = root_roots, .root_count = 1u,
        .safepoint_id = 1u
    };
    child_roots[0] = st_value_nil();
    child_frame = (StFrame) {
        .thread = &aot, .caller = &root_frame,
        .method = &fixture.unwind_method, .receiver = st_value_nil(),
        .roots = child_roots, .root_count = 1u, .safepoint_id = 1u
    };
    st_control_scope_init(&root_scope);
    st_control_scope_init(&child_scope);
    CHECK(st_control_scope_enter(&control, &root_scope, &root_frame)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&control, &root_scope, &root_home)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_enter(&control, &child_scope, &child_frame)
          == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&control, &child_scope, &child_home)
          == ST_CONTROL_OK);
    memset(&ensure_context, 0, sizeof(ensure_context));
    ensure_context.heap = &heap;
    ensure_context.top_frame = &child_frame;
    ensure_context.replacement_home = child_home;
    ensure_context.old_pending = object_new(&heap);
    ensure_context.normal_value = object_new(&heap);
    ensure_roots[0] = object_new(&heap);
    ensure_roots[1] = object_new(&heap);
    ensure_context.ensure_roots = ensure_roots;
    ensure_context.garbage = object_new(&heap);
    CHECK(st_control_non_local_return(&control, root_home,
                                      ensure_context.old_pending)
          == ST_CONTROL_OK);
    st_control_ensure_init(&ensure_record);
    CHECK(st_control_ensure_push_with_roots(
              &control, &child_scope, &ensure_record,
              ensure_collect_and_replace, &ensure_context,
              ensure_roots, 2u) == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&control, &child_scope,
                                 ensure_context.normal_value, &leave)
          == ST_CONTROL_OK);
    CHECK(ensure_context.calls == 1u &&
          ensure_context.replacement_status == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NLR_CAUGHT);
    CHECK(st_heap_contains(&heap, leave.value));

    /* A malformed running ensure root aborts collection without sweeping. */
    invalid_root = object_new(&heap);
    invalid_survivor = object_new(&heap);
    invalid_frame = (StFrame) {
        .thread = &aot, .caller = &root_frame,
        .method = &fixture.unwind_method, .receiver = st_value_nil(),
        .roots = child_roots, .root_count = 1u, .safepoint_id = 1u
    };
    st_control_scope_init(&invalid_scope);
    CHECK(st_control_scope_enter(&control, &invalid_scope, &invalid_frame)
          == ST_CONTROL_OK);
    st_control_ensure_init(&invalid_ensure);
    invalid_context = (invalid_gc_context_t){ &heap, &invalid_frame,
                                               ST_HEAP_OK };
    invalid_root += 8u;
    CHECK(st_control_ensure_push_with_roots(
              &control, &invalid_scope, &invalid_ensure,
              ensure_invalid_root_gc, &invalid_context,
              &invalid_root, 1u) == ST_CONTROL_OK);
    CHECK(st_control_scope_leave(&control, &invalid_scope, st_value_nil(),
                                 &leave) == ST_CONTROL_OK);
    CHECK(invalid_context.status == ST_HEAP_ERR_DANGLING_REFERENCE);
    CHECK(st_heap_contains(&heap, invalid_root - 8u));
    CHECK(st_heap_contains(&heap, invalid_survivor));

    /* Sidecar identity is mandatory for an unwind-capable frame. */
    aot.control = NULL;
    CHECK(st_heap_collect(&heap, &root_frame, NULL, 0u, &stats)
          == ST_HEAP_ERR_INVALID_FRAME);
    CHECK(st_heap_contains(&heap, invalid_survivor));
    aot.control = &control;
    visit_status = st_aot_control_visit_roots(
        &root_frame, NULL, NULL, &visited);
    CHECK(visit_status == ST_CONTROL_ERR_INVALID_ARGUMENT && visited == 0u);
    CHECK(st_control_scope_leave(&control, &root_scope, st_value_nil(), &leave)
          == ST_CONTROL_OK);
    st_aot_thread_destroy(&aot);
    CHECK(st_control_thread_destroy(&control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&lookup);
    st_heap_destroy(&heap);
}

typedef struct {
    st_heap_t *heap;
    st_value_t target;
    StHomeToken *token;
    bool fail_prepare;
    size_t prepare_calls;
    size_t commit_calls;
    bool commit_saw_post_sweep;
} reclaim_context_t;

static st_heap_status_t reclaim_prepare(
    void *user, st_value_t exact_value, st_object_extent_t extent,
    uint32_t class_id, uint32_t shape_id, uintptr_t *cookie_out)
{
    reclaim_context_t *context = user;
    context->prepare_calls++;
    CHECK(extent.base == (void *)(uintptr_t)exact_value);
    CHECK(class_id == 1u && shape_id == 1u);
    *cookie_out = 0u;
    if (context->fail_prepare) return ST_HEAP_ERR_RECLAIM_PROTOCOL;
    if (exact_value == context->target)
        *cookie_out = (uintptr_t)context->token;
    return ST_HEAP_OK;
}

static void reclaim_commit(void *user, st_value_t exact_value,
                           uint32_t class_id, uint32_t shape_id,
                           uintptr_t cookie)
{
    reclaim_context_t *context = user;
    CHECK(exact_value == context->target && class_id == 1u && shape_id == 1u);
    CHECK((StHomeToken *)cookie == context->token);
    context->commit_saw_post_sweep = !st_heap_contains(context->heap,
                                                       exact_value);
    context->commit_calls++;
    st_home_token_release((StHomeToken *)cookie);
}

static void test_reclaim_two_phase_home_release(void)
{
    fixture_t fixture;
    heap_allocator_t allocator = { .fail_at = SIZE_MAX };
    st_heap_t heap = {0};
    int identity;
    st_control_thread_t control = {0};
    st_control_scope_t scope;
    StFrame frame = { .thread = &identity };
    StHomeToken *home = NULL;
    st_control_leave_result_t leave;
    reclaim_context_t context;
    st_heap_reclaim_observer_t observer;
    st_heap_collection_stats_t stats;
    st_value_t other;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
              (st_runtime_allocator_t){heap_allocate, heap_deallocate,
                                       &allocator}) == ST_HEAP_OK);
    CHECK(st_control_thread_init(&control, &identity,
                                 (st_control_allocator_t){0}) == ST_CONTROL_OK);
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&control, &scope, &frame) == ST_CONTROL_OK);
    CHECK(st_control_scope_establish_home(&control, &scope, &home)
          == ST_CONTROL_OK);
    CHECK(st_home_token_retain(home) == ST_CONTROL_OK); /* closure ownership */
    CHECK(st_control_scope_leave(&control, &scope, st_value_nil(), &leave)
          == ST_CONTROL_OK);
    CHECK(st_control_live_token_count(&control) == 1u);
    memset(&context, 0, sizeof(context));
    context.heap = &heap;
    context.target = object_new(&heap);
    context.token = home;
    other = object_new(&heap);
    observer = (st_heap_reclaim_observer_t) {
        reclaim_prepare, reclaim_commit, &context
    };
    CHECK(st_heap_reclaim_observer_install(&heap, observer) == ST_HEAP_OK);
    CHECK(st_heap_reclaim_observer_install(&heap, observer)
          == ST_HEAP_ERR_CONFLICT);

    /* Workspace OOM happens before observer prepare and is transactional. */
    allocator.fail_at = allocator.calls;
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_ERR_OUT_OF_MEMORY);
    CHECK(context.prepare_calls == 0u && context.commit_calls == 0u);
    CHECK(st_heap_contains(&heap, context.target) &&
          st_heap_contains(&heap, other));
    CHECK(st_control_live_token_count(&control) == 1u &&
          st_heap_collection_count(&heap) == 0u);
    allocator.fail_at = SIZE_MAX;

    context.fail_prepare = true;
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_ERR_RECLAIM_PROTOCOL);
    CHECK(st_heap_contains(&heap, context.target) &&
          st_heap_contains(&heap, other));
    CHECK(context.commit_calls == 0u &&
          st_control_live_token_count(&control) == 1u &&
          st_heap_collection_count(&heap) == 0u);
    context.fail_prepare = false;
    context.prepare_calls = 0u;
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(context.prepare_calls == 2u && context.commit_calls == 1u);
    CHECK(context.commit_saw_post_sweep);
    CHECK(st_control_live_token_count(&control) == 0u);
    CHECK(st_heap_object_count(&heap) == 0u && stats.reclaimed_objects == 2u);
    CHECK(st_heap_reclaim_observer_remove(&heap,
              (st_heap_reclaim_observer_t){reclaim_prepare, reclaim_commit,
                                           NULL}) == ST_HEAP_ERR_CONFLICT);
    CHECK(st_heap_reclaim_observer_remove(&heap, observer) == ST_HEAP_OK);
    CHECK(st_control_thread_destroy(&control) == ST_CONTROL_OK);
    st_heap_destroy(&heap);
    CHECK(allocator.live_blocks == 0u);
}

static bool exact_exception_class(
    void *user, uint32_t exception_class_id, uint32_t caught_class_id)
{
    (void)user;
    return exception_class_id == caught_class_id;
}

static void test_gc_exception_and_handler_roots(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_lookup_context_t lookup = {0};
    st_control_thread_t control = {0};
    st_aot_thread_t aot = {0};
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {2, 3, 4, 5, 6};
    st_value_t frame_roots[1] = { st_value_nil() };
    StFrame frame;
    st_control_scope_t scope;
    st_control_handler_t handler;
    st_control_leave_result_t leave;
    st_heap_collection_stats_t stats;
    st_value_t handler_roots[2];
    st_value_t exception;
    st_value_t garbage;
    st_value_t consumed = (st_value_t)ST_VALUE_INVALID;

    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_lookup_context_init(&lookup, &fixture.descriptors,
                                 (st_lookup_allocator_t){0})
          == ST_LOOKUP_FOUND);
    CHECK(st_control_thread_init(&control, &aot,
                                 (st_control_allocator_t){0})
          == ST_CONTROL_OK);
    CHECK(st_aot_thread_init(
        &aot, &lookup, immediate_ids, NULL, &control,
        NULL, NULL, NULL, NULL, NULL));
    frame = (StFrame) {
        .thread = &aot,
        .method = &fixture.unwind_method,
        .receiver = st_value_nil(),
        .roots = frame_roots,
        .root_count = 1u,
        .safepoint_id = 1u
    };
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&control, &scope, &frame) == ST_CONTROL_OK);
    handler_roots[0] = object_new(&heap);
    handler_roots[1] = object_new(&heap);
    exception = object_new(&heap);
    garbage = object_new(&heap);
    st_control_handler_init(&handler);
    CHECK(st_control_handler_push(
              &control, &scope, &handler, 1u,
              handler_roots, 2u) == ST_CONTROL_OK);
    CHECK(st_control_exception_signal(
              &control, exception, 1u,
              exact_exception_class, NULL) == ST_CONTROL_OK);
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_contains(&heap, handler_roots[0]));
    CHECK(st_heap_contains(&heap, handler_roots[1]));
    CHECK(st_heap_contains(&heap, exception));
    CHECK(!st_heap_contains(&heap, garbage));
    CHECK(st_control_handler_consume_exception(
              &control, &handler, &consumed) == ST_CONTROL_OK
          && consumed == exception);
    consumed = st_value_nil();
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_object_count(&heap) == 0u);
    CHECK(st_control_scope_leave(
              &control, &scope, st_value_nil(), &leave) == ST_CONTROL_OK
          && leave.kind == ST_CONTROL_LEAVE_NORMAL);
    st_aot_thread_destroy(&aot);
    CHECK(st_control_thread_destroy(&control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&lookup);
    st_heap_destroy(&heap);
}

int main(void)
{
    test_gc_pending_and_ensure_roots();
    test_reclaim_two_phase_home_release();
    test_gc_exception_and_handler_roots();
    if (failures != 0u)
        fprintf(stderr, "control GC regression: %u failure(s)\n", failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
