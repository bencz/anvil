#include "st_block_primitive_bridge.h"
#include "st_control_bridge.h"
#include "st_source_bundle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition_) check((condition_), __LINE__)

enum {
    CLASS_OBJECT = 1,
    CLASS_BLOCK,
    CLASS_ARRAY,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT = CLASS_METACLASS
};

typedef struct {
    bool fail;
} allocation_control_t;

typedef struct {
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    uint64_t closure_bitmap;
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    st_control_thread_t control;
    st_aot_closure_context_t closures;
    allocation_control_t closure_allocator;
    StMethodDescriptor caller_methods[4];
    StMethodDescriptor block_methods[8];
    st_root_map_t caller_root_maps[4];
    st_root_map_t block_root_maps[8];
    uint64_t caller_root_bitmaps[4];
    uint64_t block_root_bitmaps[8];
    st_aot_block_descriptor_t block_descriptors[8];
    const st_aot_block_descriptor_t *block_descriptor_pointers[8];
} fixture_t;

static int failures;
static fixture_t *collect_fixture;
static st_heap_status_t collect_status;
static unsigned loop_remaining;
static unsigned loop_body_count;

static void check(bool condition, int line)
{
    if (condition) return;
    fprintf(stderr, "block primitive check failed at line %d\n", line);
    failures++;
}

static st_value_t small_integer(int64_t value)
{
    st_value_t result = ST_VALUE_INVALID;
    if (!st_value_from_small_integer(value, &result)) abort();
    return result;
}

static void *controlled_allocate(void *user, size_t size)
{
    allocation_control_t *control = user;
    return control->fail ? NULL : malloc(size);
}

static void controlled_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static st_value_t block_code_0(StFrame *frame)
{
    return frame->argc == 0u ? small_integer(100) : st_value_false();
}

static st_value_t block_code_1(StFrame *frame)
{
    return frame->argc == 1u ? frame->argv[0] : st_value_false();
}

static st_value_t block_code_2(StFrame *frame)
{
    int64_t left;
    int64_t right;
    if (frame->argc != 2u
            || !st_value_to_small_integer(frame->argv[0], &left)
            || !st_value_to_small_integer(frame->argv[1], &right))
        return st_value_false();
    return small_integer(left + right);
}

static st_value_t block_code_3(StFrame *frame)
{
    if (frame->argc != 3u) return st_value_false();
    return frame->argv[2];
}

static st_value_t block_code_collect(StFrame *frame)
{
    st_heap_collection_stats_t stats;
    if (collect_fixture == NULL) return st_value_false();
    frame->safepoint_id = 1u;
    collect_status = st_heap_collect(
        &collect_fixture->heap, frame, NULL, 0u, &stats);
    frame->safepoint_id = 0u;
    if (collect_status != ST_HEAP_OK)
        return st_value_false();
    return frame->argv[0];
}

static st_value_t block_code_nlr(StFrame *frame)
{
    st_control_scope_t scope;
    st_value_t value = small_integer(777);
    st_value_t propagated = ST_VALUE_INVALID;
    if (st_aot_control_scope_enter(frame, &scope, 0u) != ST_CONTROL_OK)
        return st_value_false();
    if (st_aot_control_non_local_return(frame, frame->home, value)
            != ST_CONTROL_OK)
        return st_value_false();
    if (st_aot_control_scope_leave(
            frame, &scope, value, &propagated) != ST_CONTROL_OK)
        return st_value_false();
    return propagated;
}

static st_value_t block_code_loop_condition(StFrame *frame)
{
    if (frame->argc != 0u) return st_value_false();
    if (loop_remaining == 0u) return st_value_false();
    loop_remaining--;
    return st_value_true();
}

static st_value_t block_code_loop_body(StFrame *frame)
{
    if (frame->argc != 0u) return st_value_false();
    loop_body_count++;
    return st_value_nil();
}

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Block", "Array", "UndefinedObject", "False",
        "True", "SmallInteger", "Character", "Metaclass"
    };
    static st_method_code_t const codes[8] = {
        block_code_0, block_code_1, block_code_2, block_code_3,
        block_code_collect, block_code_nlr,
        block_code_loop_condition, block_code_loop_body
    };
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };

    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t index = 0u; index < CLASS_COUNT; ++index) {
        uint32_t class_id = index + 1u;
        fixture->classes[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = class_id == CLASS_OBJECT
                || class_id == CLASS_METACLASS ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shapes[index] = (StShapeDescriptor) {
            .shape_id = class_id,
            .class_id = class_id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = sizeof(st_heap_object_t),
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->class_pointers[index] = &fixture->classes[index];
        fixture->shape_pointers[index] = &fixture->shapes[index];
    }
    fixture->closure_bitmap = 0u;
    fixture->shapes[CLASS_BLOCK - 1u] = (StShapeDescriptor) {
        .shape_id = CLASS_BLOCK,
        .class_id = CLASS_BLOCK,
        .allocation_alignment = 8u,
        .minimum_allocation_size = sizeof(st_heap_object_t)
                                 + 4u * sizeof(uint64_t),
        .fixed_word_count = 4u,
        .indexed_format = ST_INDEXED_VALUES,
        .fixed_pointer_bitmap = &fixture->closure_bitmap,
        .fixed_pointer_bitmap_word_count = 1u
    };
    fixture->shapes[CLASS_ARRAY - 1u].indexed_format = ST_INDEXED_VALUES;
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->class_pointers, CLASS_COUNT,
        fixture->shape_pointers, CLASS_COUNT
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK)
        return false;
    if (st_heap_init(
            &fixture->heap, &fixture->descriptors,
            (st_runtime_allocator_t){0}) != ST_HEAP_OK)
        return false;

    for (uint32_t arity = 0u; arity < 4u; ++arity) {
        fixture->caller_root_bitmaps[arity] =
            (UINT64_C(1) << (arity + 1u)) - UINT64_C(1);
        fixture->caller_root_maps[arity] = (st_root_map_t) {
            .safepoint_id = 1u,
            .root_count = arity + 1u,
            .bitmap_word_count = 1u,
            .live_root_bitmap = &fixture->caller_root_bitmaps[arity]
        };
        fixture->caller_methods[arity] = (StMethodDescriptor) {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = 100u + arity,
            .owner_class_id = CLASS_BLOCK,
            .arity = arity,
            .frame_root_capacity = arity + 1u,
            .code_size = 1u,
            .source_name = "Block.st",
            .source_name_length = sizeof("Block.st") - 1u,
            .root_maps = &fixture->caller_root_maps[arity],
            .root_map_count = 1u
        };
    }
    for (uint32_t index = 0u; index < 8u; ++index) {
        uint32_t arity = index < 4u ? index : index == 4u ? 1u : 0u;
        fixture->block_root_bitmaps[index] =
            (UINT64_C(1) << (arity + 1u)) - UINT64_C(1);
        fixture->block_root_maps[index] = (st_root_map_t) {
            .safepoint_id = 1u,
            .root_count = arity + 1u,
            .bitmap_word_count = 1u,
            .live_root_bitmap = &fixture->block_root_bitmaps[index]
        };
        fixture->block_methods[index] = (StMethodDescriptor) {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = 200u + index,
            .owner_class_id = CLASS_BLOCK,
            .arity = arity,
            .frame_root_capacity = arity + 1u,
            .flags = index == 5u
                ? ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN
                : 0u,
            .code_size = 1u,
            .source_name = "test block",
            .source_name_length = sizeof("test block") - 1u,
            .root_maps = &fixture->block_root_maps[index],
            .root_map_count = 1u
        };
        fixture->block_descriptors[index] = (st_aot_block_descriptor_t) {
            .abi_version = ST_AOT_BLOCK_ABI_VERSION,
            .arity = arity,
            .flags = index == 5u ? ST_AOT_BLOCK_HAS_HOME : 0u,
            .code = codes[index],
            .method = &fixture->block_methods[index]
        };
        fixture->block_descriptor_pointers[index] =
            &fixture->block_descriptors[index];
    }
    st_aot_closure_options_t closure_options = {
        .heap = &fixture->heap,
        .closure_class_id = CLASS_BLOCK,
        .closure_shape_id = CLASS_BLOCK,
        .argument_array_class_id = CLASS_ARRAY,
        .argument_array_shape_id = CLASS_ARRAY,
        .descriptors = fixture->block_descriptor_pointers,
        .descriptor_count = 8u,
        .allocate = controlled_allocate,
        .deallocate = controlled_deallocate,
        .allocator_user = &fixture->closure_allocator
    };
    if (st_aot_closure_context_init(
            &fixture->closures, &closure_options) != ST_AOT_CLOSURE_OK)
        return false;
    if (st_lookup_context_init(
            &fixture->lookup, &fixture->descriptors,
            (st_lookup_allocator_t){0}) != ST_LOOKUP_FOUND)
        return false;
    if (st_control_thread_init(
            &fixture->control, &fixture->thread,
            (st_control_allocator_t){0}) != ST_CONTROL_OK)
        return false;
    if (!st_aot_thread_init(
            &fixture->thread, &fixture->lookup, immediate_ids, NULL,
            &fixture->control, &fixture->closures, NULL, NULL, NULL, NULL))
        return false;
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_heap_collection_stats_t stats;
    CHECK(st_heap_collect(&fixture->heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK);
    st_aot_thread_destroy(&fixture->thread);
    CHECK(st_aot_closure_context_destroy(&fixture->closures)
          == ST_AOT_CLOSURE_OK);
    CHECK(st_control_thread_destroy(&fixture->control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&fixture->lookup);
    st_heap_destroy(&fixture->heap);
}

static StFrame caller_frame(fixture_t *fixture, uint32_t arity,
                            st_value_t receiver, const st_value_t *arguments,
                            st_value_t *roots)
{
    roots[0] = receiver;
    for (uint32_t index = 0u; index < arity; ++index)
        roots[index + 1u] = arguments[index];
    return (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->caller_methods[arity],
        .receiver = receiver,
        .argv = arguments,
        .roots = roots,
        .argc = arity,
        .root_count = arity + 1u,
        .safepoint_id = 1u
    };
}

static st_value_t create_closure(
    fixture_t *fixture, uint32_t descriptor_index, StFrame *frame)
{
    st_value_t closure = ST_VALUE_INVALID;
    CHECK(st_aot_closure_create(
              frame, &fixture->block_descriptors[descriptor_index],
              frame->receiver, NULL, 0u, &closure) == ST_AOT_CLOSURE_OK);
    return closure;
}

static uint32_t execute_bridge(
    uint32_t arity, StFrame *frame, st_value_t receiver,
    const st_value_t *arguments, size_t argument_count,
    st_value_t *result, uint32_t *detail)
{
    switch (arity) {
    case 0u:
        return st_aot_block_value_primitive_execute(
            frame, receiver, arguments, argument_count, result, detail);
    case 1u:
        return st_aot_block_value_primitive_1_execute(
            frame, receiver, arguments, argument_count, result, detail);
    case 2u:
        return st_aot_block_value_primitive_2_execute(
            frame, receiver, arguments, argument_count, result, detail);
    case 3u:
        return st_aot_block_value_primitive_3_execute(
            frame, receiver, arguments, argument_count, result, detail);
    default:
        return UINT32_MAX;
    }
}

static void test_fixed_arities(fixture_t *fixture)
{
    st_value_t arguments[3] = {
        small_integer(11), small_integer(22), small_integer(33)
    };
    for (uint32_t arity = 0u; arity < 4u; ++arity) {
        st_value_t roots[4] = {0};
        StFrame creation = caller_frame(
            fixture, arity, st_value_true(), arguments, roots);
        st_value_t closure = create_closure(fixture, arity, &creation);
        StFrame call = caller_frame(
            fixture, arity, closure, arguments, roots);
        st_value_t result = ST_VALUE_INVALID;
        uint32_t detail = UINT32_MAX;
        CHECK(execute_bridge(
                  arity, &call, closure,
                  arguments, arity, &result, &detail)
              == ST_BLOCK_PRIMITIVE_OK);
        CHECK(detail == 0u);
        if (arity == 0u) CHECK(result == small_integer(100));
        if (arity == 1u) CHECK(result == arguments[0]);
        if (arity == 2u) CHECK(result == small_integer(33));
        if (arity == 3u) CHECK(result == arguments[2]);
    }
}

static void test_argument_array(fixture_t *fixture)
{
    st_value_t creation_roots[2] = {st_value_true(), st_value_nil()};
    StFrame creation = caller_frame(
        fixture, 1u, st_value_true(), &creation_roots[1], creation_roots);
    st_value_t closure = create_closure(fixture, 2u, &creation);
    st_value_t array = ST_VALUE_INVALID;
    CHECK(st_heap_allocate(
              &fixture->heap, CLASS_ARRAY, CLASS_ARRAY, 2u, 2u, 0u,
              &array) == ST_HEAP_OK);
    st_object_view_t view;
    CHECK(st_heap_object_view(&fixture->heap, array, &view) == ST_HEAP_OK);
    st_value_t *elements = view.indexed_elements;
    elements[0] = small_integer(19);
    elements[1] = small_integer(23);
    st_value_t method_arguments[1] = {array};
    st_value_t roots[2];
    StFrame call = caller_frame(
        fixture, 1u, closure, method_arguments, roots);
    st_value_t result = ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;
    CHECK(st_aot_block_value_arguments_primitive_execute(
              &call, closure, method_arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    CHECK(result == small_integer(42) && detail == 0u);
}

static void test_gc_and_oom(fixture_t *fixture)
{
    st_value_t object = ST_VALUE_INVALID;
    CHECK(st_heap_allocate(
              &fixture->heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
              &object) == ST_HEAP_OK);
    st_value_t arguments[1] = {object};
    st_value_t roots[2];
    StFrame creation = caller_frame(
        fixture, 1u, st_value_true(), arguments, roots);
    st_value_t closure = create_closure(fixture, 4u, &creation);
    StFrame call = caller_frame(
        fixture, 1u, closure, arguments, roots);
    st_value_t result = ST_VALUE_INVALID;
    uint32_t detail = 0u;
    collect_fixture = fixture;
    CHECK(st_aot_block_value_primitive_1_execute(
              &call, closure, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    collect_fixture = NULL;
    if (collect_status != ST_HEAP_OK)
        fprintf(stderr, "collect status: %s\n",
                st_heap_status_string(collect_status));
    CHECK(result == object && st_heap_contains(&fixture->heap, object));

    fixture->closure_allocator.fail = true;
    result = st_value_nil();
    detail = 0u;
    CHECK(st_aot_block_value_primitive_1_execute(
              &call, closure, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID
          && detail == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    fixture->closure_allocator.fail = false;
}

static void test_nlr_and_returned_home(fixture_t *fixture)
{
    st_value_t roots[1] = {st_value_true()};
    StFrame caller = caller_frame(
        fixture, 0u, st_value_true(), NULL, roots);
    st_control_scope_t home_scope;
    CHECK(st_aot_control_scope_enter(&caller, &home_scope, 1u)
          == ST_CONTROL_OK);
    st_value_t closure = create_closure(fixture, 5u, &caller);
    caller.receiver = closure;
    caller.roots[0] = closure;
    st_value_t result = ST_VALUE_INVALID;
    uint32_t detail = 0u;
    CHECK(st_aot_block_value_primitive_execute(
              &caller, closure, NULL, 0u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    st_control_pending_info_t pending;
    CHECK(st_control_pending_get(&fixture->control, &pending)
          == ST_CONTROL_OK);
    CHECK(pending.kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN
          && pending.value == small_integer(777));
    st_value_t caught = ST_VALUE_INVALID;
    CHECK(st_aot_control_scope_leave(
              &caller, &home_scope, st_value_nil(), &caught)
          == ST_CONTROL_OK);
    CHECK(caught == small_integer(777));

    caller = caller_frame(fixture, 0u, st_value_true(), NULL, roots);
    CHECK(st_aot_control_scope_enter(&caller, &home_scope, 1u)
          == ST_CONTROL_OK);
    closure = create_closure(fixture, 5u, &caller);
    CHECK(st_aot_control_scope_leave(
              &caller, &home_scope, st_value_nil(), &caught)
          == ST_CONTROL_OK);
    caller.receiver = closure;
    caller.roots[0] = closure;
    result = st_value_nil();
    detail = 0u;
    CHECK(st_aot_block_value_primitive_execute(
              &caller, closure, NULL, 0u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_BLOCK_RETURNED);
    CHECK(result == ST_VALUE_INVALID
          && detail == ST_AOT_CLOSURE_ERR_BLOCK_RETURNED);
}

static void test_while_true(fixture_t *fixture)
{
    st_value_t creation_roots[1];
    StFrame creation = caller_frame(
        fixture, 0u, st_value_true(), NULL, creation_roots);
    st_value_t condition = create_closure(fixture, 6u, &creation);
    st_value_t body = create_closure(fixture, 7u, &creation);
    st_value_t arguments[1] = {body};
    st_value_t roots[2];
    StFrame call = caller_frame(
        fixture, 1u, condition, arguments, roots);
    st_value_t result = ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;

    loop_remaining = 3u;
    loop_body_count = 0u;
    CHECK(st_aot_block_while_true_primitive_execute(
              &call, condition, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    CHECK(result == st_value_nil() && detail == 0u);
    CHECK(loop_remaining == 0u && loop_body_count == 3u);

    loop_remaining = 0u;
    loop_body_count = 0u;
    result = ST_VALUE_INVALID;
    CHECK(st_aot_block_while_true_primitive_execute(
              &call, condition, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    CHECK(result == st_value_nil() && loop_body_count == 0u);

    st_value_t non_boolean = create_closure(fixture, 0u, &creation);
    call = caller_frame(fixture, 1u, non_boolean, arguments, roots);
    result = st_value_nil();
    CHECK(st_aot_block_while_true_primitive_execute(
              &call, non_boolean, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_EXPECTED_BOOLEAN);
    CHECK(result == ST_VALUE_INVALID);

    st_value_t wrong_arity_body = create_closure(fixture, 1u, &creation);
    arguments[0] = wrong_arity_body;
    call = caller_frame(fixture, 1u, condition, arguments, roots);
    loop_remaining = 1u;
    result = st_value_nil();
    CHECK(st_aot_block_while_true_primitive_execute(
              &call, condition, arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_WRONG_BLOCK_ARITY);
    CHECK(result == ST_VALUE_INVALID
          && detail == ST_AOT_CLOSURE_ERR_WRONG_ARITY);

    st_control_scope_t home_scope;
    st_value_t home_arguments[1] = {st_value_nil()};
    StFrame home_call = caller_frame(
        fixture, 1u, st_value_true(), home_arguments, roots);
    CHECK(st_aot_control_scope_enter(&home_call, &home_scope, 1u)
          == ST_CONTROL_OK);
    st_value_t nlr_body = create_closure(fixture, 5u, &home_call);
    home_arguments[0] = nlr_body;
    home_call.receiver = condition;
    home_call.argv = home_arguments;
    home_call.roots[0] = condition;
    home_call.roots[1] = nlr_body;
    loop_remaining = 1u;
    result = ST_VALUE_INVALID;
    detail = UINT32_MAX;
    CHECK(st_aot_block_while_true_primitive_execute(
              &home_call, condition, home_arguments, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_OK);
    CHECK(result == small_integer(777) && detail == 0u);
    st_control_pending_info_t pending;
    CHECK(st_control_pending_get(&fixture->control, &pending)
          == ST_CONTROL_OK);
    CHECK(pending.kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN
          && pending.value == small_integer(777));
    st_value_t caught = ST_VALUE_INVALID;
    CHECK(st_aot_control_scope_leave(
              &home_call, &home_scope, st_value_nil(), &caught)
          == ST_CONTROL_OK);
    CHECK(caught == small_integer(777));
}

static void test_rejections_and_catalog(fixture_t *fixture)
{
    size_t count = 0u;
    const st_primitive_spec_t *specs = st_block_primitive_specs(&count);
    CHECK(specs != NULL && count == 6u);
    for (size_t index = 0u; index < count; ++index) {
        CHECK(specs[index].implementation_kind
              == ST_PRIMITIVE_RUNTIME_SYMBOL);
        CHECK(specs[index].intrinsic_id
              == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
        CHECK(specs[index].failure_policy == ST_PRIMITIVE_FALL_THROUGH);
        CHECK(specs[index].runtime_symbol != NULL);
    }
    st_value_t roots[2] = {st_value_true(), st_value_nil()};
    StFrame creation = caller_frame(
        fixture, 1u, st_value_true(), &roots[1], roots);
    st_value_t closure = create_closure(fixture, 1u, &creation);
    st_value_t argument = small_integer(1);
    StFrame call = caller_frame(fixture, 1u, closure, &argument, roots);
    st_value_t result = st_value_nil();
    uint32_t detail = 0u;
    CHECK(st_aot_block_value_primitive_execute(
              &call, closure, &argument, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY);
    CHECK(result == ST_VALUE_INVALID);

    CHECK(st_aot_block_value_primitive_2_execute(
              &call, closure, &argument, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_WRONG_METHOD_ARITY);
    CHECK(st_aot_block_value_primitive_1_execute(
              &call, st_value_true(), &argument, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_INVALID_FRAME);

    st_value_t foreign_storage[8] = {0};
    st_value_t foreign = ST_VALUE_INVALID;
    CHECK(st_value_from_object(foreign_storage, &foreign));
    call.receiver = foreign;
    call.roots[0] = foreign;
    CHECK(st_aot_block_value_primitive_1_execute(
              &call, foreign, &argument, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_INVALID_CLOSURE);

    call.receiver = closure;
    call.roots[0] = closure;
    st_value_t not_array[1] = {small_integer(4)};
    CHECK(st_aot_block_value_arguments_primitive_execute(
              &call, closure, not_array, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT_ARRAY);

    st_value_t array = ST_VALUE_INVALID;
    CHECK(st_heap_allocate(
              &fixture->heap, CLASS_ARRAY, CLASS_ARRAY, 1u, 1u, 0u,
              &array) == ST_HEAP_OK);
    st_object_view_t view;
    CHECK(st_heap_object_view(&fixture->heap, array, &view) == ST_HEAP_OK);
    ((st_value_t *)view.indexed_elements)[0] = foreign;
    st_value_t array_argument[1] = {array};
    call.argv = array_argument;
    call.roots[1] = array;
    CHECK(st_aot_block_value_arguments_primitive_execute(
              &call, closure, array_argument, 1u, &result, &detail)
          == ST_BLOCK_PRIMITIVE_ERR_INVALID_ARGUMENT_ARRAY);
}

static const char *image_directory(void)
{
    FILE *probe = fopen("st-image/Execution/Block.st", "rb");
    if (probe != NULL) {
        fclose(probe);
        return "st-image";
    }
    probe = fopen("samples/smalltalk/st-image/Execution/Block.st", "rb");
    if (probe != NULL) {
        fclose(probe);
        return "samples/smalltalk/st-image";
    }
    return NULL;
}

static void test_real_image_bindings(void)
{
    const char *image = image_directory();
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t resolution;
    const st_ast_unit_t **units = NULL;
    const st_primitive_spec_t *specs;
    size_t spec_count = 0u;

    CHECK(image != NULL);
    if (image == NULL) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) return;
    CHECK(st_primitive_catalog_init(
        &catalog, (st_primitive_allocator_t){0}));
    specs = st_block_primitive_specs(&spec_count);
    CHECK(spec_count == 6u);
    for (size_t index = 0u; index < spec_count; ++index) {
        CHECK(st_primitive_catalog_register(
                  &catalog, &specs[index], NULL) == ST_PRIMITIVE_OK);
    }
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) goto done;
    for (size_t index = 0u; index < bundle.count; ++index)
        units[index] = &bundle.files[index].ast;
    st_primitive_result_init(&resolution);
    CHECK(st_primitive_resolve(
              &resolution, units, bundle.count, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(resolution.binding_count == 6u);
    for (size_t index = 0u; index < resolution.binding_count; ++index) {
        CHECK(resolution.bindings[index].primitive != NULL);
        CHECK(resolution.bindings[index].primitive->implementation_kind
              == ST_PRIMITIVE_RUNTIME_SYMBOL);
        CHECK(resolution.bindings[index].primitive->failure_policy
              == ST_PRIMITIVE_FALL_THROUGH);
    }
    for (size_t index = 0u; index < resolution.diagnostic_count; ++index) {
        const st_primitive_diagnostic_t *diagnostic =
            &resolution.diagnostics[index];
        for (size_t spec_index = 0u; spec_index < spec_count; ++spec_index) {
            CHECK(diagnostic->requested_name.length
                      != specs[spec_index].name_length
                  || memcmp(
                      diagnostic->requested_name.data,
                      specs[spec_index].name,
                      specs[spec_index].name_length) != 0);
        }
    }
    st_primitive_result_destroy(&resolution);

done:
    free(units);
    st_primitive_catalog_destroy(&catalog);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture)) {
        fprintf(stderr, "failed to initialize block primitive fixture\n");
        return 2;
    }
    test_fixed_arities(&fixture);
    test_argument_array(&fixture);
    test_gc_and_oom(&fixture);
    test_rejections_and_catalog(&fixture);
    test_nlr_and_returned_home(&fixture);
    test_while_true(&fixture);
    fixture_destroy(&fixture);
    test_real_image_bindings();
    if (failures != 0) return 1;
    puts("smalltalk block primitives: ok");
    return 0;
}
