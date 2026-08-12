#include "st_reflection_primitives.h"
#include "st_send_bridge.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef GENERATED_ROOT_CAPACITY
#error "GENERATED_ROOT_CAPACITY must match the emitted method descriptor"
#endif

#ifndef GENERATED_SAFEPOINT_COUNT
#error "GENERATED_SAFEPOINT_COUNT must match emitted root maps"
#endif

extern st_value_t st_Behavior_lookupSelector_aot(StFrame *frame);

enum {
    CLASS_OBJECT = 1,
    CLASS_PARENT,
    CLASS_CHILD,
    CLASS_SYMBOL,
    CLASS_METACLASS,
    CLASS_COMPILED_METHOD,
    CLASS_COUNT = CLASS_COMPILED_METHOD
};

enum {
    SELECTOR_INHERITED = 1,
    SELECTOR_OWN,
    SELECTOR_MISSING,
    SELECTOR_COUNT = SELECTOR_MISSING
};

static st_value_t leaf_method(StFrame *frame)
{
    (void)frame;
    return st_value_nil();
}

static st_value_t allocate_object(st_heap_t *heap, uint32_t class_id,
                                  uint32_t shape_id, size_t indexed_length,
                                  st_header_flags_t flags)
{
    st_value_t value = ST_VALUE_INVALID;
    if (st_heap_allocate(heap, class_id, shape_id,
                         indexed_length, indexed_length,
                         flags, &value) != ST_HEAP_OK) {
        return ST_VALUE_INVALID;
    }
    return value;
}

static bool compiled_method_fields(
    st_heap_t *heap, st_value_t value, st_value_t selector,
    st_value_t method_class, int64_t arity)
{
    st_object_view_t view;
    int64_t actual_arity;
    if (st_heap_object_view(heap, value, &view) != ST_HEAP_OK
            || view.class_descriptor->class_id
                != CLASS_COMPILED_METHOD
            || view.shape_descriptor->fixed_word_count != 3u) {
        return false;
    }
    st_value_t *slots = view.fixed_words;
    return slots[0] == selector && slots[1] == method_class
        && st_value_to_small_integer(slots[2], &actual_arity)
        && actual_arity == arity;
}

int main(void)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Parent", "Child", "Symbol", "Metaclass",
        "CompiledMethod"
    };
    static const char *const selector_spellings[SELECTOR_COUNT] = {
        "inherited", "own:", "missing"
    };
    StMethodDescriptor lookup_descriptors[2] = {
        {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = SELECTOR_INHERITED,
            .owner_class_id = CLASS_PARENT,
            .arity = 0u,
            .source_name = "Parent.st",
            .source_name_length = sizeof("Parent.st") - 1u
        },
        {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = SELECTOR_OWN,
            .owner_class_id = CLASS_CHILD,
            .arity = 1u,
            .source_name = "Child.st",
            .source_name_length = sizeof("Child.st") - 1u
        }
    };
    StMethodBinding bindings[2] = {
        { &lookup_descriptors[0], leaf_method, 1u },
        { &lookup_descriptors[1], leaf_method, 1u }
    };
    StMethodEntry entries[2];
    if (!st_method_entry_init(&entries[0], &bindings[0])
            || !st_method_entry_init(&entries[1], &bindings[1])) {
        return 1;
    }
    st_method_slot_t parent_slots[1] = {
        { SELECTOR_INHERITED, &entries[0] }
    };
    st_method_slot_t child_slots[1] = {
        { SELECTOR_OWN, &entries[1] }
    };

    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    for (uint32_t index = 0u; index < CLASS_COUNT; index++) {
        uint32_t class_id = index + 1u;
        uint32_t superclass = class_id == CLASS_OBJECT
                || class_id == CLASS_METACLASS
            ? 0u : CLASS_OBJECT;
        if (class_id == CLASS_CHILD) {
            superclass = CLASS_PARENT;
        }
        classes[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = superclass,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS
                ? ST_CLASS_METACLASS
                : class_id == CLASS_COMPILED_METHOD
                    ? ST_CLASS_ABSTRACT : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        shapes[index] = (StShapeDescriptor) {
            .shape_id = class_id,
            .class_id = class_id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        class_pointers[index] = &classes[index];
        shape_pointers[index] = &shapes[index];
    }
    classes[CLASS_PARENT - 1u].method_slots = parent_slots;
    classes[CLASS_PARENT - 1u].method_slot_count = 1u;
    classes[CLASS_CHILD - 1u].method_slots = child_slots;
    classes[CLASS_CHILD - 1u].method_slot_count = 1u;
    shapes[CLASS_SYMBOL - 1u].indexed_format = ST_INDEXED_UINT8;
    uint64_t method_bitmap = UINT64_C(0x7);
    shapes[CLASS_COMPILED_METHOD - 1u].minimum_allocation_size = 48u;
    shapes[CLASS_COMPILED_METHOD - 1u].fixed_word_count = 3u;
    shapes[CLASS_COMPILED_METHOD - 1u].fixed_pointer_bitmap =
        &method_bitmap;
    shapes[CLASS_COMPILED_METHOD - 1u]
        .fixed_pointer_bitmap_word_count = 1u;
    st_runtime_descriptors_t descriptors = {
        class_pointers, CLASS_COUNT, shape_pointers, CLASS_COUNT
    };
    if (st_runtime_descriptors_validate(&descriptors) != ST_RUNTIME_OK) {
        return 2;
    }

    st_heap_t heap = {0};
    if (st_heap_init(&heap, &descriptors,
                     (st_runtime_allocator_t) {0}) != ST_HEAP_OK) {
        return 3;
    }
    st_value_t class_objects[CLASS_COUNT];
    st_value_t selector_symbols[SELECTOR_COUNT];
    st_image_runtime_entry_t image_roots[CLASS_COUNT + SELECTOR_COUNT];
    size_t root_index = 0u;
    for (size_t index = 0u; index < CLASS_COUNT; index++) {
        class_objects[index] = allocate_object(
            &heap, CLASS_METACLASS, CLASS_METACLASS, 0u, 0u);
        if (class_objects[index] == ST_VALUE_INVALID) {
            return 4;
        }
        image_roots[root_index] = (st_image_runtime_entry_t) {
            (uint32_t)root_index + 1u, class_objects[index]
        };
        root_index++;
    }
    for (size_t index = 0u; index < SELECTOR_COUNT; index++) {
        size_t length = strlen(selector_spellings[index]);
        selector_symbols[index] = allocate_object(
            &heap, CLASS_SYMBOL, CLASS_SYMBOL, length,
            ST_HEADER_IMMUTABLE);
        st_object_view_t view;
        if (selector_symbols[index] == ST_VALUE_INVALID
                || st_heap_object_view(
                    &heap, selector_symbols[index], &view) != ST_HEAP_OK) {
            return 5;
        }
        memcpy(view.indexed_elements, selector_spellings[index], length);
        image_roots[root_index] = (st_image_runtime_entry_t) {
            (uint32_t)root_index + 1u, selector_symbols[index]
        };
        root_index++;
    }

    st_image_runtime_t image = {0};
    st_image_runtime_options_t image_options = {
        .descriptors = &descriptors,
        .borrowed_heap = &heap,
        .globals = image_roots,
        .global_count = root_index
    };
    if (st_image_runtime_init(&image, &image_options)
            != ST_IMAGE_RUNTIME_OK) {
        return 6;
    }
    st_lookup_context_t lookup = {0};
    if (st_lookup_context_init(
            &lookup, &descriptors,
            (st_lookup_allocator_t) {0}) != ST_LOOKUP_FOUND) {
        return 7;
    }
    st_reflection_context_t reflection = {0};
    st_reflection_context_options_t reflection_options = {
        .image = &image,
        .lookup = &lookup,
        .class_objects_by_id = class_objects,
        .class_object_count = CLASS_COUNT,
        .selector_symbols_by_id = selector_symbols,
        .selector_symbol_count = SELECTOR_COUNT,
        .symbol_class_id = CLASS_SYMBOL,
        .compiled_method_class_id = CLASS_COMPILED_METHOD,
        .compiled_method_shape_id = CLASS_COMPILED_METHOD
    };
    if (st_reflection_context_init(&reflection, &reflection_options)
            != ST_REFLECTION_PRIMITIVE_OK) {
        return 8;
    }

    st_aot_thread_t thread = {0};
    uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_OBJECT, CLASS_PARENT, CLASS_CHILD, CLASS_SYMBOL,
        CLASS_COMPILED_METHOD
    };
    if (!st_aot_thread_init(
            &thread, &lookup, immediate_ids,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL)
            || !st_aot_thread_image_attach(&thread, &image)
            || !st_aot_thread_reflection_attach(&thread, &reflection)) {
        return 9;
    }

    uint64_t live_bitmap = GENERATED_ROOT_CAPACITY == 64u
        ? UINT64_MAX
        : (UINT64_C(1) << GENERATED_ROOT_CAPACITY) - UINT64_C(1);
    st_root_map_t root_maps[GENERATED_SAFEPOINT_COUNT];
    for (uint32_t index = 0u; index < GENERATED_SAFEPOINT_COUNT; index++) {
        root_maps[index] = (st_root_map_t) {
            index + 1u,
            GENERATED_ROOT_CAPACITY,
            1u,
            &live_bitmap
        };
    }
    StMethodDescriptor generated_descriptor = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 100u,
        .owner_class_id = CLASS_CHILD,
        .arity = 1u,
        .frame_root_capacity = GENERATED_ROOT_CAPACITY,
        .flags = ST_METHOD_PRIMITIVE,
        .source_name = "Behavior.st",
        .source_name_length = sizeof("Behavior.st") - 1u,
        .root_maps = root_maps,
        .root_map_count = GENERATED_SAFEPOINT_COUNT
    };
    if (!st_method_descriptor_is_valid(&generated_descriptor)) {
        return 10;
    }
    st_value_t frame_roots[GENERATED_ROOT_CAPACITY];
    for (uint32_t index = 0u; index < GENERATED_ROOT_CAPACITY; index++) {
        frame_roots[index] = st_value_nil();
    }
    st_value_t argument = selector_symbols[SELECTOR_OWN - 1u];
    StFrame frame = {
        .thread = &thread,
        .method = &generated_descriptor,
        .receiver = class_objects[CLASS_CHILD - 1u],
        .argv = &argument,
        .roots = frame_roots,
        .argc = 1u,
        .root_count = GENERATED_ROOT_CAPACITY
    };
    st_value_t own = st_Behavior_lookupSelector_aot(&frame);
    if (!compiled_method_fields(
            &heap, own, argument,
            class_objects[CLASS_CHILD - 1u], 1)) {
        return 11;
    }
    if (st_Behavior_lookupSelector_aot(&frame) != own) {
        return 12;
    }

    argument = selector_symbols[SELECTOR_INHERITED - 1u];
    st_value_t inherited = st_Behavior_lookupSelector_aot(&frame);
    if (!compiled_method_fields(
            &heap, inherited, argument,
            class_objects[CLASS_PARENT - 1u], 0)) {
        return 13;
    }
    argument = selector_symbols[SELECTOR_MISSING - 1u];
    if (st_Behavior_lookupSelector_aot(&frame) != st_value_nil()) {
        return 14;
    }

    if (!st_aot_thread_reflection_detach(&thread, &reflection)
            || !st_aot_thread_image_detach(&thread, &image)) {
        return 15;
    }
    st_aot_thread_destroy(&thread);
    st_reflection_context_destroy(&reflection);
    st_lookup_context_destroy(&lookup);
    st_image_runtime_destroy(&image);
    st_heap_destroy(&heap);
    return 0;
}
