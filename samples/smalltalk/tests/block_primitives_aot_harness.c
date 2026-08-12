#include "st_block_primitive_bridge.h"
#include "st_control.h"
#include "st_heap.h"
#include "st_lookup.h"
#include "st_send_bridge.h"

#include <stdint.h>
#include <string.h>

extern st_value_t st_Probe_blockValue0(StFrame *frame);
extern st_value_t st_Probe_blockValue1(StFrame *frame);
extern st_value_t st_Probe_blockWhileTrue(StFrame *frame);

static unsigned loop_remaining;
static unsigned loop_body_count;

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

static st_value_t small_integer(int64_t value)
{
    st_value_t result = ST_VALUE_INVALID;
    return st_value_from_small_integer(value, &result)
        ? result : ST_VALUE_INVALID;
}

static st_value_t value_0_code(StFrame *frame)
{
    return frame != NULL && frame->argc == 0u
        ? small_integer(100) : st_value_false();
}

static st_value_t value_1_code(StFrame *frame)
{
    return frame != NULL && frame->argc == 1u
        ? frame->argv[0] : st_value_false();
}

static st_value_t while_condition_code(StFrame *frame)
{
    if (frame == NULL || frame->argc != 0u) return st_value_false();
    if (loop_remaining == 0u) return st_value_false();
    loop_remaining--;
    return st_value_true();
}

static st_value_t while_body_code(StFrame *frame)
{
    if (frame == NULL || frame->argc != 0u) return st_value_false();
    loop_body_count++;
    return st_value_nil();
}

int main(void)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Block", "Array", "UndefinedObject", "False", "True",
        "SmallInteger", "Character", "Metaclass"
    };
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    uint64_t closure_bitmap = 0u;
    for (uint32_t index = 0u; index < CLASS_COUNT; ++index) {
        uint32_t class_id = index + 1u;
        classes[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = class_id == CLASS_OBJECT
                || class_id == CLASS_METACLASS ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
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
    shapes[CLASS_BLOCK - 1u] = (StShapeDescriptor) {
        .shape_id = CLASS_BLOCK,
        .class_id = CLASS_BLOCK,
        .allocation_alignment = 8u,
        .minimum_allocation_size = sizeof(st_heap_object_t)
                                 + 4u * sizeof(uint64_t),
        .fixed_word_count = 4u,
        .indexed_format = ST_INDEXED_VALUES,
        .fixed_pointer_bitmap = &closure_bitmap,
        .fixed_pointer_bitmap_word_count = 1u
    };
    shapes[CLASS_ARRAY - 1u].indexed_format = ST_INDEXED_VALUES;
    st_runtime_descriptors_t descriptors = {
        class_pointers, CLASS_COUNT, shape_pointers, CLASS_COUNT
    };
    if (st_runtime_descriptors_validate(&descriptors) != ST_RUNTIME_OK)
        return 1;

    uint64_t target_bitmap_0 = UINT64_C(1);
    uint64_t target_bitmap_1 = UINT64_C(3);
    st_root_map_t target_maps[4] = {
        {1u, 1u, 1u, &target_bitmap_0},
        {1u, 2u, 1u, &target_bitmap_1},
        {1u, 1u, 1u, &target_bitmap_0},
        {1u, 1u, 1u, &target_bitmap_0}
    };
    StMethodDescriptor target_methods[4] = {
        {
            ST_METHOD_ABI_VERSION, 201u, CLASS_BLOCK, 0u, 1u, 0u, 1u,
            "block-0", 7u, 0u, 0u, &target_maps[0], 1u, NULL, 0u
        },
        {
            ST_METHOD_ABI_VERSION, 202u, CLASS_BLOCK, 1u, 2u, 0u, 1u,
            "block-1", 7u, 0u, 0u, &target_maps[1], 1u, NULL, 0u
        },
        {
            ST_METHOD_ABI_VERSION, 203u, CLASS_BLOCK, 0u, 1u, 0u, 1u,
            "condition", 9u, 0u, 0u, &target_maps[2], 1u, NULL, 0u
        },
        {
            ST_METHOD_ABI_VERSION, 204u, CLASS_BLOCK, 0u, 1u, 0u, 1u,
            "body", 4u, 0u, 0u, &target_maps[3], 1u, NULL, 0u
        }
    };
    st_aot_block_descriptor_t block_descriptors[4] = {
        {
            ST_AOT_BLOCK_ABI_VERSION, 0u, 0u, 0u,
            value_0_code, &target_methods[0], NULL, 0u
        },
        {
            ST_AOT_BLOCK_ABI_VERSION, 1u, 0u, 0u,
            value_1_code, &target_methods[1], NULL, 0u
        },
        {
            ST_AOT_BLOCK_ABI_VERSION, 0u, 0u, 0u,
            while_condition_code, &target_methods[2], NULL, 0u
        },
        {
            ST_AOT_BLOCK_ABI_VERSION, 0u, 0u, 0u,
            while_body_code, &target_methods[3], NULL, 0u
        }
    };
    const st_aot_block_descriptor_t *block_pointers[4] = {
        &block_descriptors[0], &block_descriptors[1],
        &block_descriptors[2], &block_descriptors[3]
    };
    st_heap_t heap = {0};
    if (st_heap_init(
            &heap, &descriptors, (st_runtime_allocator_t){0}) != ST_HEAP_OK)
        return 2;
    st_aot_closure_context_t closures = {0};
    st_aot_closure_options_t closure_options = {
        .heap = &heap,
        .closure_class_id = CLASS_BLOCK,
        .closure_shape_id = CLASS_BLOCK,
        .argument_array_class_id = CLASS_ARRAY,
        .argument_array_shape_id = CLASS_ARRAY,
        .descriptors = block_pointers,
        .descriptor_count = 4u
    };
    if (st_aot_closure_context_init(&closures, &closure_options)
            != ST_AOT_CLOSURE_OK)
        return 3;
    st_lookup_context_t lookup = {0};
    if (st_lookup_context_init(
            &lookup, &descriptors,
            (st_lookup_allocator_t){0}) != ST_LOOKUP_FOUND)
        return 4;
    st_aot_thread_t thread = {0};
    st_control_thread_t control = {0};
    if (st_control_thread_init(
            &control, &thread,
            (st_control_allocator_t){0}) != ST_CONTROL_OK)
        return 5;
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    if (!st_aot_thread_init(
            &thread, &lookup, immediate_ids, NULL, &control, &closures,
            NULL, NULL, NULL, NULL))
        return 6;

    uint64_t caller_bitmap = UINT64_C(0xff);
    st_root_map_t caller_maps[2] = {
        {1u, 8u, 1u, &caller_bitmap},
        {2u, 8u, 1u, &caller_bitmap}
    };
    StMethodDescriptor caller_methods[2] = {
        {
            ST_METHOD_ABI_VERSION, 301u, CLASS_BLOCK, 0u, 8u,
            ST_METHOD_CAN_UNWIND, 1u, "value", 5u, 0u, 0u,
            caller_maps, 2u, NULL, 0u
        },
        {
            ST_METHOD_ABI_VERSION, 302u, CLASS_BLOCK, 1u, 8u,
            ST_METHOD_CAN_UNWIND, 1u, "value:", 6u, 0u, 0u,
            caller_maps, 2u, NULL, 0u
        }
    };
    st_value_t creation_roots[2] = {st_value_true(), st_value_nil()};
    StFrame creation = {
        .thread = &thread,
        .method = &caller_methods[1],
        .receiver = st_value_true(),
        .argv = &creation_roots[1],
        .roots = creation_roots,
        .argc = 1u,
        .root_count = 8u
    };
    st_value_t padded_creation_roots[8] = {0};
    padded_creation_roots[0] = st_value_true();
    padded_creation_roots[1] = st_value_nil();
    creation.roots = padded_creation_roots;
    st_value_t closures_values[4] = {
        ST_VALUE_INVALID, ST_VALUE_INVALID,
        ST_VALUE_INVALID, ST_VALUE_INVALID
    };
    if (st_aot_closure_create(
            &creation, &block_descriptors[0], creation.receiver, NULL, 0u,
            &closures_values[0]) != ST_AOT_CLOSURE_OK
            || st_aot_closure_create(
                &creation, &block_descriptors[1], creation.receiver, NULL, 0u,
                &closures_values[1]) != ST_AOT_CLOSURE_OK
            || st_aot_closure_create(
                &creation, &block_descriptors[2], creation.receiver, NULL, 0u,
                &closures_values[2]) != ST_AOT_CLOSURE_OK
            || st_aot_closure_create(
                &creation, &block_descriptors[3], creation.receiver, NULL, 0u,
                &closures_values[3]) != ST_AOT_CLOSURE_OK)
        return 7;

    st_value_t roots[8] = {0};
    roots[0] = closures_values[0];
    StFrame value_frame = {
        .thread = &thread,
        .method = &caller_methods[0],
        .receiver = closures_values[0],
        .roots = roots,
        .root_count = 8u
    };
    if (st_Probe_blockValue0(&value_frame) != small_integer(100)) return 8;
    st_value_t argument = small_integer(321);
    memset(roots, 0, sizeof(roots));
    roots[0] = closures_values[1];
    roots[1] = argument;
    value_frame.method = &caller_methods[1];
    value_frame.receiver = closures_values[1];
    value_frame.argv = &argument;
    value_frame.argc = 1u;
    if (st_Probe_blockValue1(&value_frame) != argument) return 9;

    loop_remaining = 3u;
    loop_body_count = 0u;
    st_value_t body = closures_values[3];
    memset(roots, 0, sizeof(roots));
    roots[0] = closures_values[2];
    roots[1] = body;
    value_frame.method = &caller_methods[1];
    value_frame.receiver = closures_values[2];
    value_frame.argv = &body;
    value_frame.argc = 1u;
    if (st_Probe_blockWhileTrue(&value_frame) != st_value_nil()) return 10;
    if (loop_remaining != 0u || loop_body_count != 3u) return 11;

    st_heap_collection_stats_t stats;
    if (st_heap_collect(&heap, NULL, NULL, 0u, &stats) != ST_HEAP_OK)
        return 12;
    st_aot_thread_destroy(&thread);
    if (st_aot_closure_context_destroy(&closures) != ST_AOT_CLOSURE_OK)
        return 13;
    if (st_control_thread_destroy(&control) != ST_CONTROL_OK) return 14;
    st_lookup_context_destroy(&lookup);
    st_heap_destroy(&heap);
    return 0;
}
