#include "st_control.h"
#include "st_heap.h"
#include "st_lookup.h"
#include "st_send_bridge.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef GENERATED_ROOT_CAPACITY
#error "GENERATED_ROOT_CAPACITY must match the emitted method descriptor"
#endif

extern st_value_t st_Exception_signal_aot(StFrame *frame);

enum {
    CLASS_OBJECT = 1,
    CLASS_EXCEPTION,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT = CLASS_METACLASS
};

typedef struct {
    st_heap_t *heap;
} object_classifier_t;

static bool object_class(
    void *user, st_value_t value, uint32_t *class_id_out)
{
    object_classifier_t *classifier = user;
    st_object_view_t view;
    if (st_heap_object_view(classifier->heap, value, &view) != ST_HEAP_OK)
        return false;
    *class_id_out = view.class_descriptor->class_id;
    return true;
}

int main(void)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Exception", "UndefinedObject", "False", "True",
        "SmallInteger", "Character", "Metaclass"
    };
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    for (uint32_t index = 0u; index < CLASS_COUNT; index++) {
        uint32_t class_id = index + 1u;
        classes[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = class_id == CLASS_OBJECT ||
                             class_id == CLASS_METACLASS
                ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS
                ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        shapes[index] = (StShapeDescriptor) {
            .shape_id = class_id,
            .class_id = class_id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = sizeof(st_heap_object_t),
            .indexed_format = ST_INDEXED_NONE
        };
        class_pointers[index] = &classes[index];
        shape_pointers[index] = &shapes[index];
    }
    st_runtime_descriptors_t descriptors = {
        class_pointers, CLASS_COUNT, shape_pointers, CLASS_COUNT
    };
    if (st_runtime_descriptors_validate(&descriptors) != ST_RUNTIME_OK)
        return 1;

    uint64_t live_bitmap = GENERATED_ROOT_CAPACITY == 64u
        ? UINT64_MAX
        : (UINT64_C(1) << GENERATED_ROOT_CAPACITY) - UINT64_C(1);
    st_root_map_t root_maps[2] = {
        { 1u, GENERATED_ROOT_CAPACITY, 1u, &live_bitmap },
        { 2u, GENERATED_ROOT_CAPACITY, 1u, &live_bitmap }
    };
    StMethodDescriptor method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 101u,
        .owner_class_id = CLASS_EXCEPTION,
        .arity = 0u,
        .frame_root_capacity = GENERATED_ROOT_CAPACITY,
        .flags = ST_METHOD_CAN_UNWIND | ST_METHOD_PRIMITIVE,
        .code_size = 1u,
        .source_name = "Exception.st",
        .source_name_length = sizeof("Exception.st") - 1u,
        .root_maps = root_maps,
        .root_map_count = 2u
    };
    if (!st_method_descriptor_is_valid(&method)) return 2;

    st_heap_t heap = {0};
    st_lookup_context_t lookup = {0};
    st_control_thread_t control = {0};
    st_aot_thread_t thread = {0};
    if (st_heap_init(
            &heap, &descriptors,
            (st_runtime_allocator_t){0}) != ST_HEAP_OK)
        return 3;
    if (st_lookup_context_init(
            &lookup, &descriptors,
            (st_lookup_allocator_t){0}) != ST_LOOKUP_FOUND)
        return 4;
    if (st_control_thread_init(
            &control, &thread,
            (st_control_allocator_t){0}) != ST_CONTROL_OK)
        return 5;
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE,
        CLASS_SMALL_INTEGER, CLASS_CHARACTER
    };
    object_classifier_t classifier = { &heap };
    if (!st_aot_thread_init(
            &thread, &lookup, immediate_ids, NULL, &control, NULL,
            object_class, &classifier, NULL, NULL))
        return 6;
    st_value_t exception = (st_value_t)ST_VALUE_INVALID;
    if (st_heap_allocate(
            &heap, CLASS_EXCEPTION, CLASS_EXCEPTION,
            0u, 0u, 0u, &exception) != ST_HEAP_OK)
        return 7;
    st_value_t roots[GENERATED_ROOT_CAPACITY];
    for (uint32_t index = 0u; index < GENERATED_ROOT_CAPACITY; index++)
        roots[index] = st_value_nil();
    roots[0] = exception;
    StFrame frame = {
        .thread = &thread,
        .method = &method,
        .receiver = exception,
        .roots = roots,
        .root_count = GENERATED_ROOT_CAPACITY
    };
    if (st_Exception_signal_aot(&frame) != exception) return 8;
    st_control_pending_info_t pending;
    if (st_control_pending_get(&control, &pending) != ST_CONTROL_OK ||
        pending.kind != ST_CONTROL_PENDING_EXCEPTION ||
        pending.value != exception ||
        pending.exception_class_id != CLASS_EXCEPTION ||
        pending.has_handler)
        return 9;
    if (st_control_pending_clear(&control) != ST_CONTROL_OK) return 10;
    st_heap_collection_stats_t stats;
    if (st_heap_collect(&heap, NULL, NULL, 0u, &stats) != ST_HEAP_OK)
        return 11;
    st_aot_thread_destroy(&thread);
    if (st_control_thread_destroy(&control) != ST_CONTROL_OK) return 12;
    st_lookup_context_destroy(&lookup);
    st_heap_destroy(&heap);
    return 0;
}
