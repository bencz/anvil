#include "st_control_bridge.h"
#include "st_exception_primitive_bridge.h"
#include "st_source_bundle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition_) check((condition_), __LINE__)

enum {
    CLASS_OBJECT = 1,
    CLASS_BLOCK,
    CLASS_EXCEPTION,
    CLASS_ERROR,
    CLASS_SPECIFIC_ERROR,
    CLASS_OTHER_ERROR,
    CLASS_CLASS_OBJECT,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT = CLASS_METACLASS
};

enum {
    BLOCK_NORMAL = 0,
    BLOCK_SIGNAL_SPECIFIC,
    BLOCK_HANDLER,
    BLOCK_CLEANUP,
    BLOCK_REPLACE_EXCEPTION,
    BLOCK_NON_LOCAL_RETURN,
    BLOCK_COUNT
};

typedef struct {
    bool fail;
} allocation_control_t;

typedef struct {
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    st_runtime_descriptors_t descriptors;
    uint64_t closure_bitmap;
    st_heap_t heap;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    st_control_thread_t control;
    st_aot_closure_context_t closures;
    allocation_control_t allocator;
    StMethodDescriptor caller_methods[3];
    st_root_map_t caller_maps[3];
    uint64_t caller_bitmaps[3];
    StMethodDescriptor block_methods[BLOCK_COUNT];
    st_root_map_t block_maps[BLOCK_COUNT];
    uint64_t block_bitmaps[BLOCK_COUNT];
    st_aot_block_descriptor_t block_descriptors[BLOCK_COUNT];
    const st_aot_block_descriptor_t *block_pointers[BLOCK_COUNT];
    st_value_t class_objects[CLASS_COUNT + 1u];
    st_value_t specific_exception;
    st_value_t other_exception;
    unsigned cleanup_calls;
} fixture_t;

static int failures;
static fixture_t *active_fixture;

static void check(bool condition, int line)
{
    if (condition) return;
    fprintf(stderr, "exception primitive check failed at line %d\n", line);
    failures++;
}

static st_value_t small_integer(int64_t value)
{
    st_value_t result = (st_value_t)ST_VALUE_INVALID;
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

static bool object_class(
    void *user, st_value_t value, uint32_t *class_id_out)
{
    fixture_t *fixture = user;
    st_object_view_t view;
    if (st_heap_object_view(&fixture->heap, value, &view) != ST_HEAP_OK)
        return false;
    *class_id_out = view.class_descriptor->class_id;
    return true;
}

static bool class_object(
    void *user, st_value_t value, uint32_t *class_id_out)
{
    fixture_t *fixture = user;
    st_object_view_t view;
    if (st_heap_object_view(&fixture->heap, value, &view) != ST_HEAP_OK ||
        view.class_descriptor->class_id != CLASS_CLASS_OBJECT)
        return false;
    for (uint32_t class_id = 1u; class_id <= CLASS_COUNT; class_id++) {
        if (fixture->class_objects[class_id] == value) {
            *class_id_out = class_id;
            return true;
        }
    }
    return false;
}

static bool class_inherits(
    void *user, uint32_t actual_class_id, uint32_t caught_class_id)
{
    fixture_t *fixture = user;
    uint32_t cursor = actual_class_id;
    for (size_t hops = 0u;
         cursor != 0u && hops < fixture->descriptors.class_count; hops++) {
        const StClassDescriptor *descriptor = st_runtime_class(
            &fixture->descriptors, cursor);
        if (descriptor == NULL) return false;
        if (cursor == caught_class_id) return true;
        cursor = descriptor->superclass_id;
    }
    return false;
}

static st_value_t block_normal(StFrame *frame)
{
    return frame->argc == 0u ? small_integer(41) : st_value_false();
}

static st_value_t publish_exception(
    StFrame *frame, st_value_t exception, uint32_t class_id)
{
    st_control_scope_t scope;
    st_value_t propagated = (st_value_t)ST_VALUE_INVALID;
    if (active_fixture == NULL || frame->argc != 0u ||
        st_aot_control_scope_enter(frame, &scope, 0u) != ST_CONTROL_OK ||
        st_control_exception_signal(
            &active_fixture->control, exception, class_id,
            class_inherits, active_fixture) != ST_CONTROL_OK ||
        st_aot_control_scope_leave(
            frame, &scope, exception, &propagated) != ST_CONTROL_OK)
        return st_value_false();
    return propagated;
}

static st_value_t block_signal_specific(StFrame *frame)
{
    return publish_exception(
        frame, active_fixture->specific_exception, CLASS_SPECIFIC_ERROR);
}

static st_value_t block_handler(StFrame *frame)
{
    return frame->argc == 1u ? frame->argv[0] : st_value_false();
}

static st_value_t block_cleanup(StFrame *frame)
{
    if (active_fixture == NULL || frame->argc != 0u)
        return st_value_false();
    active_fixture->cleanup_calls++;
    return small_integer(99);
}

static st_value_t block_replace_exception(StFrame *frame)
{
    if (active_fixture != NULL) active_fixture->cleanup_calls++;
    return publish_exception(
        frame, active_fixture->other_exception, CLASS_OTHER_ERROR);
}

static st_value_t block_non_local_return(StFrame *frame)
{
    st_control_scope_t scope;
    st_value_t value = small_integer(777);
    st_value_t propagated = (st_value_t)ST_VALUE_INVALID;

    if (frame->argc != 0u || frame->home == NULL ||
        st_aot_control_scope_enter(frame, &scope, 0u) != ST_CONTROL_OK ||
        st_aot_control_non_local_return(frame, frame->home, value) !=
            ST_CONTROL_OK ||
        st_aot_control_scope_leave(
            frame, &scope, value, &propagated) != ST_CONTROL_OK)
        return st_value_false();
    return propagated;
}

static st_value_t allocate_object(fixture_t *fixture, uint32_t class_id)
{
    st_value_t value = (st_value_t)ST_VALUE_INVALID;
    CHECK(st_heap_allocate(
              &fixture->heap, class_id, class_id,
              0u, 0u, 0u, &value) == ST_HEAP_OK);
    return value;
}

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Block", "Exception", "Error", "SpecificError",
        "OtherError", "ClassObject", "UndefinedObject", "False",
        "True", "SmallInteger", "Character", "Metaclass"
    };
    static const st_method_code_t codes[BLOCK_COUNT] = {
        block_normal,
        block_signal_specific,
        block_handler,
        block_cleanup,
        block_replace_exception,
        block_non_local_return
    };
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE,
        CLASS_SMALL_INTEGER, CLASS_CHARACTER
    };
    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t index = 0u; index < CLASS_COUNT; index++) {
        uint32_t class_id = index + 1u;
        uint32_t superclass = CLASS_OBJECT;
        if (class_id == CLASS_OBJECT || class_id == CLASS_METACLASS)
            superclass = 0u;
        else if (class_id == CLASS_ERROR)
            superclass = CLASS_EXCEPTION;
        else if (class_id == CLASS_SPECIFIC_ERROR ||
                 class_id == CLASS_OTHER_ERROR)
            superclass = CLASS_ERROR;
        fixture->classes[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = superclass,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS
                ? ST_CLASS_METACLASS : 0u,
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
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->class_pointers, CLASS_COUNT,
        fixture->shape_pointers, CLASS_COUNT
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK ||
        st_heap_init(&fixture->heap, &fixture->descriptors,
                     (st_runtime_allocator_t){0}) != ST_HEAP_OK)
        return false;

    for (uint32_t arity = 0u; arity <= 2u; arity++) {
        fixture->caller_bitmaps[arity] = UINT64_C(0xff);
        fixture->caller_maps[arity] = (st_root_map_t) {
            1u, 8u, 1u, &fixture->caller_bitmaps[arity]
        };
        fixture->caller_methods[arity] = (StMethodDescriptor) {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = 100u + arity,
            .owner_class_id = CLASS_BLOCK,
            .arity = arity,
            .frame_root_capacity = 8u,
            .flags = ST_METHOD_CAN_UNWIND,
            .code_size = 1u,
            .source_name = "exception primitive test",
            .source_name_length = sizeof("exception primitive test") - 1u,
            .root_maps = &fixture->caller_maps[arity],
            .root_map_count = 1u
        };
    }
    for (uint32_t index = 0u; index < BLOCK_COUNT; index++) {
        uint32_t arity = index == BLOCK_HANDLER ? 1u : 0u;
        fixture->block_bitmaps[index] = arity == 0u
            ? UINT64_C(1) : UINT64_C(3);
        fixture->block_maps[index] = (st_root_map_t) {
            1u, arity + 1u, 1u, &fixture->block_bitmaps[index]
        };
        bool can_unwind = index == BLOCK_SIGNAL_SPECIFIC ||
            index == BLOCK_REPLACE_EXCEPTION ||
            index == BLOCK_NON_LOCAL_RETURN;
        bool has_non_local_return = index == BLOCK_NON_LOCAL_RETURN;
        fixture->block_methods[index] = (StMethodDescriptor) {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = 200u + index,
            .owner_class_id = CLASS_BLOCK,
            .arity = arity,
            .frame_root_capacity = arity + 1u,
            .flags = (can_unwind ? ST_METHOD_CAN_UNWIND : 0u) |
                (has_non_local_return
                    ? ST_METHOD_HAS_NON_LOCAL_RETURN : 0u),
            .code_size = 1u,
            .source_name = "exception block",
            .source_name_length = sizeof("exception block") - 1u,
            .root_maps = &fixture->block_maps[index],
            .root_map_count = 1u
        };
        fixture->block_descriptors[index] = (st_aot_block_descriptor_t) {
            .abi_version = ST_AOT_BLOCK_ABI_VERSION,
            .arity = arity,
            .flags = has_non_local_return ? ST_AOT_BLOCK_HAS_HOME : 0u,
            .code = codes[index],
            .method = &fixture->block_methods[index]
        };
        fixture->block_pointers[index] =
            &fixture->block_descriptors[index];
    }
    st_aot_closure_options_t closure_options = {
        .heap = &fixture->heap,
        .closure_class_id = CLASS_BLOCK,
        .closure_shape_id = CLASS_BLOCK,
        .descriptors = fixture->block_pointers,
        .descriptor_count = BLOCK_COUNT,
        .allocate = controlled_allocate,
        .deallocate = controlled_deallocate,
        .allocator_user = &fixture->allocator
    };
    if (st_aot_closure_context_init(
            &fixture->closures, &closure_options) != ST_AOT_CLOSURE_OK ||
        st_lookup_context_init(
            &fixture->lookup, &fixture->descriptors,
            (st_lookup_allocator_t){0}) != ST_LOOKUP_FOUND ||
        st_control_thread_init(
            &fixture->control, &fixture->thread,
            (st_control_allocator_t){0}) != ST_CONTROL_OK ||
        !st_aot_thread_init(
            &fixture->thread, &fixture->lookup, immediate_ids,
            NULL, &fixture->control, &fixture->closures,
            object_class, fixture, NULL, NULL) ||
        st_control_exception_configure(
            &fixture->control, class_object, fixture) != ST_CONTROL_OK)
        return false;

    for (uint32_t class_id = 1u; class_id <= CLASS_COUNT; class_id++)
        fixture->class_objects[class_id] = allocate_object(
            fixture, CLASS_CLASS_OBJECT);
    fixture->specific_exception = allocate_object(
        fixture, CLASS_SPECIFIC_ERROR);
    fixture->other_exception = allocate_object(
        fixture, CLASS_OTHER_ERROR);
    active_fixture = fixture;
    return true;
}

static StFrame caller_frame(
    fixture_t *fixture, uint32_t arity, st_value_t receiver,
    const st_value_t *arguments, st_value_t roots[8])
{
    for (uint32_t index = 0u; index < 8u; index++)
        roots[index] = st_value_nil();
    roots[0] = receiver;
    for (uint32_t index = 0u; index < arity; index++)
        roots[index + 1u] = arguments[index];
    return (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->caller_methods[arity],
        .receiver = receiver,
        .argv = arguments,
        .roots = roots,
        .argc = arity,
        .root_count = 8u,
        .safepoint_id = 1u
    };
}

static st_value_t create_closure(
    fixture_t *fixture, uint32_t descriptor_index, StFrame *frame)
{
    st_value_t closure = (st_value_t)ST_VALUE_INVALID;
    CHECK(st_aot_closure_create(
              frame, &fixture->block_descriptors[descriptor_index],
              frame->receiver, NULL, 0u, &closure) == ST_AOT_CLOSURE_OK);
    return closure;
}

static void leave_scope(
    fixture_t *fixture, StFrame *frame, st_control_scope_t *scope,
    st_value_t normal_value, st_value_t *value_out)
{
    CHECK(st_aot_control_scope_leave(
              frame, scope, normal_value, value_out) == ST_CONTROL_OK);
    (void)fixture;
}

static void test_signal(fixture_t *fixture)
{
    st_value_t roots[8];
    StFrame frame = caller_frame(
        fixture, 0u, fixture->specific_exception, NULL, roots);
    st_control_scope_t scope;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    st_value_t result = (st_value_t)ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;
    CHECK(st_aot_exception_signal_primitive_execute(
              &frame, fixture->specific_exception, NULL, 0u,
              &result, &detail) == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(result == fixture->specific_exception && detail == 0u);
    st_control_pending_info_t pending;
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && pending.value == fixture->specific_exception
          && pending.exception_class_id == CLASS_SPECIFIC_ERROR
          && !pending.has_handler);
    st_value_t propagated = (st_value_t)ST_VALUE_INVALID;
    leave_scope(fixture, &frame, &scope, result, &propagated);
    CHECK(propagated == fixture->specific_exception);
    CHECK(st_control_pending_clear(&fixture->control) == ST_CONTROL_OK);

}

static void test_caught_and_nested_class_matching(fixture_t *fixture)
{
    st_value_t creation_roots[8];
    StFrame creation = caller_frame(
        fixture, 0u, st_value_true(), NULL, creation_roots);
    st_value_t protected = create_closure(
        fixture, BLOCK_SIGNAL_SPECIFIC, &creation);
    creation.roots[0] = protected;
    st_value_t handler = create_closure(fixture, BLOCK_HANDLER, &creation);
    st_value_t arguments[2] = {
        fixture->class_objects[CLASS_ERROR], handler
    };
    st_value_t roots[8];
    StFrame frame = caller_frame(
        fixture, 2u, protected, arguments, roots);
    st_control_scope_t scope;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    st_value_t result = (st_value_t)ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;
    CHECK(st_aot_block_on_exception_primitive_execute(
              &frame, protected, arguments, 2u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(result == fixture->specific_exception && detail == 0u);
    st_control_pending_info_t pending;
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_NONE);
    st_value_t final = (st_value_t)ST_VALUE_INVALID;
    leave_scope(fixture, &frame, &scope, result, &final);
    CHECK(final == fixture->specific_exception);

    /* A sibling class does not catch; the exception remains pending for an
     * outer handler/application boundary. */
    arguments[0] = fixture->class_objects[CLASS_OTHER_ERROR];
    frame = caller_frame(fixture, 2u, protected, arguments, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    result = (st_value_t)ST_VALUE_INVALID;
    detail = UINT32_MAX;
    CHECK(st_aot_block_on_exception_primitive_execute(
              &frame, protected, arguments, 2u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && !pending.has_handler);
    leave_scope(fixture, &frame, &scope, result, &final);
    CHECK(st_control_pending_clear(&fixture->control) == ST_CONTROL_OK);

}

static void test_ensure_paths(fixture_t *fixture)
{
    st_value_t creation_roots[8];
    StFrame creation = caller_frame(
        fixture, 0u, st_value_true(), NULL, creation_roots);
    st_value_t normal = create_closure(fixture, BLOCK_NORMAL, &creation);
    creation.roots[0] = normal;
    st_value_t signaling = create_closure(
        fixture, BLOCK_SIGNAL_SPECIFIC, &creation);
    creation.roots[0] = signaling;
    st_value_t cleanup = create_closure(fixture, BLOCK_CLEANUP, &creation);
    creation.roots[0] = cleanup;
    st_value_t replacement = create_closure(
        fixture, BLOCK_REPLACE_EXCEPTION, &creation);

    st_value_t argument[1] = { cleanup };
    st_value_t roots[8];
    StFrame frame = caller_frame(fixture, 1u, normal, argument, roots);
    st_control_scope_t scope;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    st_value_t result = (st_value_t)ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;
    fixture->cleanup_calls = 0u;
    CHECK(st_aot_block_unwind_primitive_execute(
              &frame, normal, argument, 1u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(result == small_integer(41) && detail == 0u
          && fixture->cleanup_calls == 1u);
    st_value_t final = (st_value_t)ST_VALUE_INVALID;
    leave_scope(fixture, &frame, &scope, result, &final);
    CHECK(final == small_integer(41) && fixture->cleanup_calls == 1u);

    frame = caller_frame(fixture, 1u, signaling, argument, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    fixture->cleanup_calls = 0u;
    CHECK(st_aot_block_unwind_primitive_execute(
              &frame, signaling, argument, 1u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    st_control_pending_info_t pending;
    CHECK(fixture->cleanup_calls == 1u);
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && pending.value == fixture->specific_exception);
    leave_scope(fixture, &frame, &scope, result, &final);
    CHECK(st_control_pending_clear(&fixture->control) == ST_CONTROL_OK);

    argument[0] = replacement;
    frame = caller_frame(fixture, 1u, signaling, argument, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    fixture->cleanup_calls = 0u;
    CHECK(st_aot_block_unwind_primitive_execute(
              &frame, signaling, argument, 1u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(fixture->cleanup_calls == 1u);
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_EXCEPTION
          && pending.value == fixture->other_exception
          && pending.exception_class_id == CLASS_OTHER_ERROR);
    leave_scope(fixture, &frame, &scope, result, &final);
    CHECK(st_control_pending_clear(&fixture->control) == ST_CONTROL_OK);

    /* Cleanup also runs before an NLR reaches its authenticated home.  The
     * primitive must preserve the pending NLR so the ordinary AOT epilogue,
     * rather than the primitive itself, performs the return. */
    argument[0] = cleanup;
    frame = caller_frame(fixture, 1u, st_value_true(), argument, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 1u) == ST_CONTROL_OK);
    st_value_t non_local = create_closure(
        fixture, BLOCK_NON_LOCAL_RETURN, &frame);
    frame.receiver = non_local;
    frame.roots[0] = non_local;
    fixture->cleanup_calls = 0u;
    CHECK(st_aot_block_unwind_primitive_execute(
              &frame, non_local, argument, 1u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_OK);
    CHECK(fixture->cleanup_calls == 1u);
    CHECK(st_control_pending_get(&fixture->control, &pending) == ST_CONTROL_OK
          && pending.kind == ST_CONTROL_PENDING_NON_LOCAL_RETURN
          && pending.value == small_integer(777));
    leave_scope(fixture, &frame, &scope, st_value_nil(), &final);
    CHECK(final == small_integer(777) && fixture->cleanup_calls == 1u);
}

static void test_faults_and_catalog(fixture_t *fixture)
{
    size_t count = 0u;
    const st_primitive_spec_t *specs = st_exception_primitive_specs(&count);
    CHECK(specs != NULL && count == 3u);
    CHECK(specs[0].implementation_kind ==
          ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL);
    CHECK(specs[0].failure_policy == ST_PRIMITIVE_CANNOT_FAIL);
    CHECK(specs[1].method_arity == 2u && specs[2].method_arity == 1u);

    st_value_t roots[8];
    StFrame frame = caller_frame(
        fixture, 0u, fixture->specific_exception, NULL, roots);
    st_value_t result = st_value_nil();
    uint32_t detail = 0u;
    CHECK(st_aot_exception_signal_primitive_execute(
              &frame, fixture->specific_exception, NULL, 1u,
              &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_ERR_WRONG_METHOD_ARITY);
    CHECK(result == ST_VALUE_INVALID);
    frame.receiver = st_value_true();
    CHECK(st_aot_exception_signal_primitive_execute(
              &frame, fixture->specific_exception, NULL, 0u,
              &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_ERR_INVALID_FRAME);

    st_value_t foreign_object = UINT64_C(0xdeadbeef0);
    frame = caller_frame(
        fixture, 0u, foreign_object, NULL, roots);
    st_control_scope_t scope;
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    CHECK(st_aot_exception_signal_primitive_execute(
              &frame, foreign_object, NULL, 0u,
              &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION);
    st_value_t final = (st_value_t)ST_VALUE_INVALID;
    leave_scope(fixture, &frame, &scope, st_value_nil(), &final);

    st_value_t creation_roots[8];
    StFrame creation = caller_frame(
        fixture, 0u, st_value_true(), NULL, creation_roots);
    st_value_t normal = create_closure(fixture, BLOCK_NORMAL, &creation);
    creation.roots[0] = normal;
    st_value_t cleanup = create_closure(fixture, BLOCK_CLEANUP, &creation);
    creation.roots[0] = cleanup;
    st_value_t handler = create_closure(fixture, BLOCK_HANDLER, &creation);
    st_value_t handler_arguments[2] = { foreign_object, handler };
    frame = caller_frame(
        fixture, 2u, normal, handler_arguments, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    CHECK(st_aot_block_on_exception_primitive_execute(
              &frame, normal, handler_arguments, 2u,
              &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_ERR_INVALID_EXCEPTION_CLASS);
    leave_scope(fixture, &frame, &scope, st_value_nil(), &final);

    st_value_t argument[1] = { cleanup };
    frame = caller_frame(fixture, 1u, normal, argument, roots);
    CHECK(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK);
    fixture->allocator.fail = true;
    CHECK(st_aot_block_unwind_primitive_execute(
              &frame, normal, argument, 1u, &result, &detail)
          == ST_EXCEPTION_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID
          && detail == ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY);
    fixture->allocator.fail = false;
    leave_scope(fixture, &frame, &scope, st_value_nil(), &final);
}

static const char *image_directory(void)
{
    FILE *probe = fopen("st-image/Exceptions/Exception.st", "rb");
    if (probe != NULL) {
        fclose(probe);
        return "st-image";
    }
    probe = fopen(
        "samples/smalltalk/st-image/Exceptions/Exception.st", "rb");
    if (probe != NULL) {
        fclose(probe);
        return "samples/smalltalk/st-image";
    }
    return NULL;
}

static void test_real_image_catalog(void)
{
    const char *image = image_directory();
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t resolution;
    const st_ast_unit_t **units = NULL;
    size_t spec_count = 0u;
    const st_primitive_spec_t *specs = st_exception_primitive_specs(
        &spec_count);
    CHECK(image != NULL && specs != NULL && spec_count == 3u);
    if (image == NULL || specs == NULL) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) return;
    CHECK(st_primitive_catalog_init(
        &catalog, (st_primitive_allocator_t){0}));
    for (size_t index = 0u; index < spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &catalog, &specs[index], NULL) == ST_PRIMITIVE_OK);
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) goto done;
    for (size_t index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;
    st_primitive_result_init(&resolution);
    CHECK(st_primitive_resolve(
              &resolution, units, bundle.count, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(resolution.binding_count == 3u);
    size_t signal_count = 0u;
    size_t handler_count = 0u;
    size_t ensure_count = 0u;
    for (size_t index = 0u; index < resolution.binding_count; index++) {
        const st_primitive_t *primitive =
            resolution.bindings[index].primitive;
        CHECK(primitive != NULL && primitive->implementation_kind ==
              ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL);
        if (primitive != NULL && primitive->name.length ==
                sizeof("ExceptionSignal") - 1u &&
            memcmp(primitive->name.data, "ExceptionSignal",
                   sizeof("ExceptionSignal") - 1u) == 0)
            signal_count++;
        if (primitive != NULL && primitive->name.length ==
                sizeof("BlockOnExceptionPrimitive") - 1u &&
            memcmp(primitive->name.data, "BlockOnExceptionPrimitive",
                   sizeof("BlockOnExceptionPrimitive") - 1u) == 0)
            handler_count++;
        if (primitive != NULL && primitive->name.length ==
                sizeof("BlockUnwindPrimitive") - 1u &&
            memcmp(primitive->name.data, "BlockUnwindPrimitive",
                   sizeof("BlockUnwindPrimitive") - 1u) == 0)
            ensure_count++;
    }
    CHECK(signal_count == 1u && handler_count == 1u && ensure_count == 1u);
    for (size_t index = 0u; index < resolution.diagnostic_count; index++) {
        const st_primitive_diagnostic_t *diagnostic =
            &resolution.diagnostics[index];
        for (size_t spec_index = 0u; spec_index < spec_count; spec_index++)
            CHECK(diagnostic->requested_name.length !=
                      specs[spec_index].name_length ||
                  memcmp(diagnostic->requested_name.data,
                         specs[spec_index].name,
                         specs[spec_index].name_length) != 0);
    }
    st_primitive_result_destroy(&resolution);
done:
    free(units);
    st_primitive_catalog_destroy(&catalog);
    st_source_bundle_destroy(&bundle);
}

static void fixture_destroy(fixture_t *fixture)
{
    st_heap_collection_stats_t stats;
    active_fixture = NULL;
    CHECK(st_heap_collect(&fixture->heap, NULL, NULL, 0u, &stats)
          == ST_HEAP_OK);
    st_aot_thread_destroy(&fixture->thread);
    CHECK(st_aot_closure_context_destroy(&fixture->closures)
          == ST_AOT_CLOSURE_OK);
    CHECK(st_control_thread_destroy(&fixture->control) == ST_CONTROL_OK);
    st_lookup_context_destroy(&fixture->lookup);
    st_heap_destroy(&fixture->heap);
}

int main(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    if (failures == 0) {
        test_signal(&fixture);
        test_caught_and_nested_class_matching(&fixture);
        test_ensure_paths(&fixture);
        test_faults_and_catalog(&fixture);
        test_real_image_catalog();
        fixture_destroy(&fixture);
    }
    if (failures != 0) {
        fprintf(stderr, "exception primitive regression: %d failure(s)\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk AOT exception primitives: PASS");
    return EXIT_SUCCESS;
}
