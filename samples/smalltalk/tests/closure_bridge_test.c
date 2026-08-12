#include "st_closure_bridge.h"
#include "st_control_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) {                                            \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x);    \
    failures++;                                                               \
} } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_CLOSURE = 2,
    CLASS_CELL = 3,
    CLASS_NIL = 4,
    CLASS_FALSE = 5,
    CLASS_TRUE = 6,
    CLASS_INTEGER = 7,
    CLASS_CHARACTER = 8,
    CLASS_METACLASS = 9,
    CLASS_COUNT = 9
};

typedef struct { bool fail; } allocation_control_t;

static void *context_allocate(void *user, size_t size)
{
    allocation_control_t *control = user;
    return control->fail ? NULL : malloc(size);
}

static void context_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void *heap_allocate(void *user, size_t alignment, size_t size)
{
    allocation_control_t *control = user;
    return control->fail ? NULL : aligned_alloc(alignment, size);
}

static void heap_deallocate(void *user, void *pointer,
                            size_t alignment, size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    free(pointer);
}

static st_value_t capture_code(StFrame *frame)
{
    st_value_t value = st_value_nil();
    if (st_aot_closure_capture_load(
            frame, frame->receiver, 0u, &value) != ST_AOT_CLOSURE_OK)
        return st_value_false();
    return value;
}

static st_value_t home_code(StFrame *frame)
{
    st_value_t value = UINT64_C(0x2a1);
    st_control_scope_t scope;
    st_value_t result = st_value_nil();
    if (st_aot_control_scope_enter(frame, &scope, 0u) != ST_CONTROL_OK)
        return st_value_false();
    if (st_aot_control_non_local_return(frame, frame->home, value)
            != ST_CONTROL_OK)
        return st_value_false();
    if (st_aot_control_scope_leave(frame, &scope, value, &result)
            != ST_CONTROL_OK)
        return st_value_false();
    return result;
}

static st_value_t cell_code(StFrame *frame)
{
    st_value_t cell = st_value_nil();
    st_value_t value = st_value_nil();
    if (st_aot_closure_capture_load(frame, frame->receiver, 0u, &cell)
            != ST_AOT_CLOSURE_OK
            || st_aot_closure_cell_load(frame, cell, &value)
                != ST_AOT_CLOSURE_OK)
        return st_value_false();
    return value;
}

typedef struct {
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    uint64_t closure_bitmap;
    uint64_t cell_bitmap;
    st_runtime_descriptors_t descriptors;
    allocation_control_t allocation;
    allocation_control_t closure_allocation;
    st_heap_t heap;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    st_control_thread_t control;
    st_aot_closure_context_t closures;
    StMethodDescriptor caller_method;
    StMethodDescriptor capture_method;
    StMethodDescriptor home_method;
    st_unwind_region_t home_unwind;
    st_aot_capture_descriptor_t value_capture;
    st_aot_capture_descriptor_t self_capture;
    st_aot_capture_descriptor_t cell_capture;
    st_aot_block_descriptor_t capture_descriptor;
    st_aot_block_descriptor_t self_descriptor;
    st_aot_block_descriptor_t home_descriptor;
    st_aot_block_descriptor_t cell_descriptor;
    const st_aot_block_descriptor_t *block_descriptors[4];
} fixture_t;

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Block", "Cell", "Nil", "False", "True",
        "SmallInteger", "Character", "Metaclass"
    };
    memset(fixture, 0, sizeof(*fixture));
    fixture->closure_bitmap = 0u;
    fixture->cell_bitmap = 1u;
    for (uint32_t index = 0u; index < CLASS_COUNT; index++) {
        fixture->classes[index] = (StClassDescriptor) {
            index + 1u,
            (index == 0u || index == CLASS_METACLASS - 1u)
                ? 0u : CLASS_OBJECT,
            CLASS_METACLASS, index + 1u,
            index == CLASS_METACLASS - 1u ? ST_CLASS_METACLASS
                : index == CLASS_CLOSURE - 1u || index == CLASS_CELL - 1u
                    ? ST_CLASS_ABSTRACT : 0u,
            names[index], strlen(names[index]), NULL, 0u
        };
        fixture->shapes[index] = (StShapeDescriptor) {
            index + 1u, index + 1u, 8u, 24u, 0u,
            ST_INDEXED_NONE, NULL, 0u
        };
        fixture->class_pointers[index] = &fixture->classes[index];
        fixture->shape_pointers[index] = &fixture->shapes[index];
    }
    fixture->shapes[CLASS_CLOSURE - 1u] = (StShapeDescriptor) {
        CLASS_CLOSURE, CLASS_CLOSURE, 8u, 56u, 4u,
        ST_INDEXED_VALUES, &fixture->closure_bitmap, 1u
    };
    fixture->shapes[CLASS_CELL - 1u] = (StShapeDescriptor) {
        CLASS_CELL, CLASS_CELL, 8u, 32u, 1u,
        ST_INDEXED_NONE, &fixture->cell_bitmap, 1u
    };
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->class_pointers, CLASS_COUNT,
        fixture->shape_pointers, CLASS_COUNT
    };
    fixture->caller_method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION, .selector_id = 1u,
        .owner_class_id = CLASS_OBJECT, .code_size = 1u
    };
    fixture->capture_method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION, .selector_id = 2u,
        .owner_class_id = CLASS_CLOSURE, .frame_root_capacity = 1u,
        .code_size = 1u
    };
    fixture->home_unwind = (st_unwind_region_t) {
        0u, 1u, 0u, ST_UNWIND_NON_LOCAL_RETURN, 0u
    };
    fixture->home_method = (StMethodDescriptor) {
        ST_METHOD_ABI_VERSION, 3u, CLASS_CLOSURE, 0u, 0u,
        ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN, 1u,
        NULL, 0u, 0u, 0u, NULL, 0u, &fixture->home_unwind, 1u
    };
    fixture->value_capture = (st_aot_capture_descriptor_t) {
        0u, ST_AOT_CAPTURE_VALUE
    };
    fixture->capture_descriptor = (st_aot_block_descriptor_t) {
        ST_AOT_BLOCK_ABI_VERSION, 0u, 1u, 0u, capture_code,
        &fixture->capture_method, &fixture->value_capture, 1u
    };
    fixture->self_capture = (st_aot_capture_descriptor_t) {
        0u, ST_AOT_CAPTURE_SELF
    };
    fixture->self_descriptor = (st_aot_block_descriptor_t) {
        ST_AOT_BLOCK_ABI_VERSION, 0u, 1u, 0u, capture_code,
        &fixture->capture_method, &fixture->self_capture, 1u
    };
    fixture->home_descriptor = (st_aot_block_descriptor_t) {
        ST_AOT_BLOCK_ABI_VERSION, 0u, 0u, ST_AOT_BLOCK_HAS_HOME,
        home_code, &fixture->home_method, NULL, 0u
    };
    fixture->cell_capture = (st_aot_capture_descriptor_t) {
        0u, ST_AOT_CAPTURE_CELL
    };
    fixture->cell_descriptor = (st_aot_block_descriptor_t) {
        ST_AOT_BLOCK_ABI_VERSION, 0u, 1u, ST_AOT_BLOCK_HAS_CELLS,
        cell_code, &fixture->capture_method, &fixture->cell_capture, 1u
    };
    fixture->block_descriptors[0] = &fixture->capture_descriptor;
    fixture->block_descriptors[1] = &fixture->self_descriptor;
    fixture->block_descriptors[2] = &fixture->home_descriptor;
    fixture->block_descriptors[3] = &fixture->cell_descriptor;
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK
            || st_heap_init(&fixture->heap, &fixture->descriptors,
                (st_runtime_allocator_t) {
                    heap_allocate, heap_deallocate, &fixture->allocation
                }) != ST_HEAP_OK
            || st_lookup_context_init(&fixture->lookup,
                &fixture->descriptors, (st_lookup_allocator_t){0})
                != ST_LOOKUP_FOUND
            || st_aot_closure_context_init(
                &fixture->closures, &(st_aot_closure_options_t) {
                    .heap = &fixture->heap,
                    .closure_class_id = CLASS_CLOSURE,
                    .closure_shape_id = CLASS_CLOSURE,
                    .cell_class_id = CLASS_CELL,
                    .cell_shape_id = CLASS_CELL,
                    .descriptors = fixture->block_descriptors,
                    .descriptor_count = 4u,
                    .allocate = context_allocate,
                    .deallocate = context_deallocate,
                    .allocator_user = &fixture->closure_allocation
                }) != ST_AOT_CLOSURE_OK
            || st_control_thread_init(&fixture->control, &fixture->thread,
                                      (st_control_allocator_t){0})
                != ST_CONTROL_OK)
        return false;
    uint32_t immediate[5] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_INTEGER, CLASS_CHARACTER
    };
    return st_aot_thread_init(
        &fixture->thread, &fixture->lookup, immediate, NULL,
        &fixture->control, &fixture->closures, NULL, NULL, NULL, NULL);
}

static void fixture_destroy(fixture_t *fixture)
{
    st_aot_thread_destroy(&fixture->thread);
    CHECK(st_aot_closure_context_destroy(&fixture->closures)
          == ST_AOT_CLOSURE_OK);
    CHECK(st_control_thread_destroy(&fixture->control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&fixture->lookup);
    st_heap_destroy(&fixture->heap);
}

static StFrame caller_frame(fixture_t *fixture)
{
    StFrame frame = {0};
    frame.thread = &fixture->thread;
    frame.method = &fixture->caller_method;
    frame.receiver = st_value_true();
    return frame;
}

static void test_capture_invoke_and_gc(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        CHECK(false);
        return;
    }
    StFrame caller = caller_frame(&fixture);
    st_value_t captured = st_value_nil();
    CHECK(st_heap_allocate(&fixture.heap, CLASS_OBJECT, CLASS_OBJECT,
                           0u, 0u, 0u, &captured) == ST_HEAP_OK);
    st_value_t closure = st_value_nil();
    CHECK(st_aot_closure_create(&caller, &fixture.capture_descriptor,
                                caller.receiver, &captured, 1u, &closure)
          == ST_AOT_CLOSURE_OK);
    st_object_view_t closure_view;
    CHECK(st_heap_object_view(&fixture.heap, closure, &closure_view)
          == ST_HEAP_OK);
    CHECK((st_object_header_flags(
              st_object_header_load(&closure_view.object->header))
           & ST_HEADER_IMMUTABLE) != 0u);
    st_aot_closure_target_t target;
    CHECK(st_aot_closure_resolve(&caller, closure, 0u, &target)
          == ST_AOT_CLOSURE_OK);
    CHECK(target.code == capture_code && target.frame_root_capacity == 1u
          && target.home == NULL && target.capture_count == 1u);
    st_value_t roots[1] = { closure };
    StFrame child = {
        .thread = &fixture.thread, .caller = &caller,
        .method = target.method, .home = target.home,
        .receiver = closure, .roots = roots, .root_count = 1u
    };
    CHECK(target.code(&child) == captured);
    st_heap_collection_stats_t stats;
    CHECK(st_heap_collect(&fixture.heap, NULL, roots, 1u, &stats)
          == ST_HEAP_OK);
    CHECK(st_heap_contains(&fixture.heap, closure)
          && st_heap_contains(&fixture.heap, captured));
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK);
    CHECK(!st_heap_contains(&fixture.heap, closure)
          && !st_heap_contains(&fixture.heap, captured));
    fixture_destroy(&fixture);
}

static void test_home_nlr_reclaim_and_returned(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        CHECK(false);
        return;
    }
    StFrame outer = caller_frame(&fixture);
    st_control_scope_t outer_scope;
    CHECK(st_aot_control_scope_enter(&outer, &outer_scope, 1u)
          == ST_CONTROL_OK);
    CHECK(st_control_live_token_count(&fixture.control) == 1u);
    st_value_t closure = st_value_nil();
    CHECK(st_aot_closure_create(&outer, &fixture.home_descriptor,
                                outer.receiver, NULL, 0u, &closure)
          == ST_AOT_CLOSURE_OK);
    st_aot_closure_target_t target;
    CHECK(st_aot_closure_resolve(&outer, closure, 0u, &target)
          == ST_AOT_CLOSURE_OK && target.home == outer.home);
    StFrame child = {
        .thread = &fixture.thread, .caller = &outer,
        .method = target.method, .home = target.home,
        .receiver = closure
    };
    CHECK(target.code(&child) == UINT64_C(0x2a1));
    st_value_t caught = st_value_nil();
    CHECK(st_aot_control_scope_leave(
              &outer, &outer_scope, st_value_false(), &caught)
          == ST_CONTROL_OK && caught == UINT64_C(0x2a1));
    CHECK(st_control_live_token_count(&fixture.control) == 1u);
    child.caller = NULL;
    child.home = target.home;
    st_control_scope_t returned_scope;
    CHECK(st_aot_control_scope_enter(&child, &returned_scope, 0u)
          == ST_CONTROL_OK);
    st_control_status_t returned_status = st_aot_control_non_local_return(
        &child, child.home, UINT64_C(0x3a1));
    if (returned_status != ST_CONTROL_ERR_BLOCK_RETURNED)
        fprintf(stderr, "returned-home status: %s\n",
                st_control_status_string(returned_status));
    CHECK(returned_status == ST_CONTROL_ERR_BLOCK_RETURNED);
    CHECK(st_aot_control_scope_leave(
              &child, &returned_scope, st_value_true(), &caught)
          == ST_CONTROL_OK && caught == st_value_true());
    st_heap_collection_stats_t stats;
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK && stats.reclaimed_objects == 1u);
    CHECK(st_control_live_token_count(&fixture.control) == 0u);
    fixture_destroy(&fixture);
}

static void test_oom_is_transactional(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        CHECK(false);
        return;
    }
    StFrame outer = caller_frame(&fixture);
    st_control_scope_t scope;
    CHECK(st_aot_control_scope_enter(&outer, &scope, 1u) == ST_CONTROL_OK);
    fixture.allocation.fail = true;
    st_value_t closure = UINT64_MAX;
    CHECK(st_aot_closure_create(&outer, &fixture.home_descriptor,
                                outer.receiver, NULL, 0u, &closure)
          == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    CHECK(closure == st_value_nil()
          && st_control_live_token_count(&fixture.control) == 1u);
    fixture.allocation.fail = false;
    st_value_t cell = UINT64_MAX;
    fixture.allocation.fail = true;
    CHECK(st_aot_closure_cell_create(&outer, st_value_true(), &cell)
          == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    CHECK(cell == st_value_nil());
    fixture.allocation.fail = false;
    st_value_t value;
    CHECK(st_aot_control_scope_leave(
              &outer, &scope, st_value_nil(), &value) == ST_CONTROL_OK);
    CHECK(st_control_live_token_count(&fixture.control) == 0u);
    fixture_destroy(&fixture);
}

static void test_registry_growth_self_and_cells(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        CHECK(false);
        return;
    }
    StFrame caller = caller_frame(&fixture);
    st_value_t closures[96];
    for (size_t index = 0u; index < 96u; index++) {
        st_value_t capture = st_value_nil();
        CHECK(st_aot_closure_create(
                  &caller, &fixture.capture_descriptor, caller.receiver,
                  &capture, 1u,
                  &closures[index]) == ST_AOT_CLOSURE_OK);
    }
    for (size_t index = 0u; index < 96u; index++) {
        st_aot_closure_target_t target;
        CHECK(st_aot_closure_resolve(&caller, closures[index], 0u, &target)
              == ST_AOT_CLOSURE_OK);
    }
    CHECK(st_aot_closure_context_destroy(&fixture.closures)
          == ST_AOT_CLOSURE_ERR_BUSY);
    st_heap_collection_stats_t stats;
    CHECK(st_heap_collect(&fixture.heap, NULL, closures, 96u, &stats)
          == ST_HEAP_OK && stats.reclaimed_objects == 0u);
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK && stats.reclaimed_objects == 96u);

    st_value_t arbitrary = st_value_false();
    st_value_t closure = st_value_nil();
    CHECK(st_aot_closure_create(
              &caller, &fixture.self_descriptor, caller.receiver,
              &arbitrary, 1u, &closure)
          == ST_AOT_CLOSURE_ERR_INVALID_CAPTURE);
    st_value_t self = caller.receiver;
    CHECK(st_aot_closure_create(
              &caller, &fixture.self_descriptor, caller.receiver,
              &self, 1u, &closure)
          == ST_AOT_CLOSURE_OK);
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK && stats.reclaimed_objects == 1u);

    st_value_t cell = st_value_nil();
    CHECK((fixture.classes[CLASS_CELL - 1u].flags & ST_CLASS_ABSTRACT) != 0u);
    /* The bridge is the privileged allocator for compiler-private cells;
     * ordinary Behavior>>new rejects this exact abstract class separately. */
    CHECK(st_aot_closure_cell_create(&caller, st_value_false(), &cell)
          == ST_AOT_CLOSURE_OK);
    st_object_view_t cell_view;
    CHECK(st_heap_object_view(&fixture.heap, cell, &cell_view) == ST_HEAP_OK
          && cell_view.class_descriptor->class_id == CLASS_CELL
          && cell_view.shape_descriptor->shape_id == CLASS_CELL);
    CHECK(st_aot_closure_create(
              &caller, &fixture.cell_descriptor, caller.receiver,
              &cell, 1u, &closure) == ST_AOT_CLOSURE_OK);
    CHECK(st_aot_closure_invoke(&caller, closure, NULL, 0u, &self)
          == ST_AOT_CLOSURE_OK && self == st_value_false());
    CHECK(st_aot_closure_cell_store(&caller, cell, st_value_true())
          == ST_AOT_CLOSURE_OK);
    CHECK(st_aot_closure_invoke(&caller, closure, NULL, 0u, &self)
          == ST_AOT_CLOSURE_OK && self == st_value_true());
    st_value_t cell_roots[1] = { closure };
    CHECK(st_heap_collect(&fixture.heap, NULL, cell_roots, 1u, &stats)
          == ST_HEAP_OK && st_heap_contains(&fixture.heap, cell));
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK && !st_heap_contains(&fixture.heap, cell));
    fixture_destroy(&fixture);
}

static void test_hash_growth_oom_is_transactional(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        CHECK(false);
        return;
    }
    StFrame caller = caller_frame(&fixture);
    st_value_t capture = st_value_nil();
    st_value_t closures[6];
    size_t initial_objects = st_heap_object_count(&fixture.heap);
    fixture.closure_allocation.fail = true;
    st_value_t entry_failure = UINT64_MAX;
    CHECK(st_aot_closure_create(
              &caller, &fixture.capture_descriptor, caller.receiver,
              &capture, 1u,
              &entry_failure) == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    CHECK(entry_failure == st_value_nil()
          && st_heap_object_count(&fixture.heap) == initial_objects);
    fixture.closure_allocation.fail = false;
    for (size_t index = 0u; index < 6u; index++)
        CHECK(st_aot_closure_create(
                  &caller, &fixture.capture_descriptor, caller.receiver,
                  &capture, 1u,
                  &closures[index]) == ST_AOT_CLOSURE_OK);
    size_t objects = st_heap_object_count(&fixture.heap);
    fixture.closure_allocation.fail = true;
    st_value_t unpublished = UINT64_MAX;
    CHECK(st_aot_closure_create(
              &caller, &fixture.capture_descriptor, caller.receiver,
              &capture, 1u,
              &unpublished) == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    CHECK(unpublished == st_value_nil()
          && st_heap_object_count(&fixture.heap) == objects);
    fixture.closure_allocation.fail = false;
    st_heap_collection_stats_t stats;
    CHECK(st_heap_collect(&fixture.heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK && stats.reclaimed_objects == 6u);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_capture_invoke_and_gc();
    test_home_nlr_reclaim_and_returned();
    test_oom_is_transactional();
    test_registry_growth_self_and_cells();
    test_hash_growth_oom_is_transactional();
    if (failures) {
        fprintf(stderr, "%u closure bridge regression(s) failed\n", failures);
        return 1;
    }
    puts("Smalltalk AOT closure bridge: PASS");
    return 0;
}
