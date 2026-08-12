#include "st_aot_bootstrap.h"

#include <stdlib.h>
#include <string.h>

#define ST_AOT_BOOTSTRAP_MAGIC UINT64_C(0x5354424f4f543031)

enum {
    ST_BEHAVIOR_SUPERCLASS_SLOT = 0,
    ST_BEHAVIOR_SUBCLASSES_SLOT = 1,
    ST_BEHAVIOR_METHOD_DICTIONARY_SLOT = 2,
    ST_BEHAVIOR_INSTANCE_VARIABLES_SLOT = 3,
    ST_CLASS_NAME_SLOT = 4,
    ST_CLASS_COMMENT_SLOT = 5,
    ST_CLASS_CATEGORY_SLOT = 6,
    ST_CLASS_VARIABLES_SLOT = 7,
    ST_CLASS_NAMESPACE_SLOT = 8,
    ST_CLASS_SLOT_COUNT = 9,
    ST_METACLASS_INSTANCE_CLASS_SLOT = 4,
    ST_METACLASS_SLOT_COUNT = 5
};

struct st_aot_bootstrap_state {
    uint64_t magic;
    st_primitive_allocator_t allocator;
    const st_image_metadata_descriptor_t *metadata;
    const st_runtime_descriptors_t *descriptors;
    st_heap_t *heap;
    st_value_t *roots;
    size_t root_count;
    st_value_t *class_objects;
    size_t class_count;
    st_value_t *symbols;
    size_t symbol_count;
    st_value_t *selector_symbols;
    size_t selector_count;
    st_value_t *subclass_arrays;
    st_value_t *method_dictionaries;
    st_value_t *instance_variable_arrays;
    st_value_t *class_variable_arrays;
};

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

static bool normalize_allocator(st_primitive_allocator_t input,
                                st_primitive_allocator_t *output)
{
    if (output == NULL
            || ((input.allocate == NULL) != (input.deallocate == NULL))) {
        return false;
    }
    if (input.allocate == NULL) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static bool add_size(size_t left, size_t right, size_t *result_out)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result_out = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (left != 0u && right > SIZE_MAX / left) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static void release(st_primitive_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) {
        allocator.deallocate(allocator.user, pointer);
    }
}

static st_aot_bootstrap_state_t *basic_state(
    const st_aot_bootstrap_context_t *context)
{
    if (context == NULL || context->state == NULL
            || context->state->magic != ST_AOT_BOOTSTRAP_MAGIC) {
        return NULL;
    }
    return context->state;
}

static st_aot_bootstrap_state_t *live_state(
    const st_aot_bootstrap_context_t *context)
{
    st_aot_bootstrap_state_t *state = basic_state(context);
    if (state == NULL || !context->initialized
            || context->abi_version != ST_AOT_BOOTSTRAP_ABI_VERSION
            || context->image == NULL || context->symbols == NULL
            || context->lookup == NULL
            || st_image_runtime_heap(context->image) != state->heap
            || !st_image_runtime_root_provider_contains(
                context->image, &context->root_provider)) {
        return NULL;
    }
    return state;
}

static st_image_runtime_status_t bootstrap_root_span(
    void *owner, const st_value_t **roots_out, size_t *root_count_out)
{
    st_aot_bootstrap_context_t *context = owner;
    st_aot_bootstrap_state_t *state = basic_state(context);
    if (roots_out != NULL) {
        *roots_out = NULL;
    }
    if (root_count_out != NULL) {
        *root_count_out = 0u;
    }
    if (state == NULL || roots_out == NULL || root_count_out == NULL) {
        return ST_IMAGE_RUNTIME_ERR_INVALID_STATE;
    }
    *roots_out = state->root_count == 0u ? NULL : state->roots;
    *root_count_out = state->root_count;
    return ST_IMAGE_RUNTIME_OK;
}

static const st_image_entity_metadata_t *entity_at(
    const st_image_metadata_descriptor_t *metadata, uint32_t entity_id)
{
    if (metadata == NULL || entity_id == 0u
            || entity_id > metadata->entity_count
            || metadata->entities == NULL) {
        return NULL;
    }
    const st_image_entity_metadata_t *entity =
        &metadata->entities[entity_id - 1u];
    return entity->id == entity_id ? entity : NULL;
}

static uint32_t entity_runtime_id(
    const st_image_metadata_descriptor_t *metadata, uint32_t entity_id)
{
    if (metadata == NULL || entity_id == 0u
            || entity_id > metadata->entity_count
            || metadata->entity_runtime_class_ids == NULL) {
        return 0u;
    }
    return metadata->entity_runtime_class_ids[entity_id - 1u];
}

static const st_image_entity_metadata_t *runtime_entity(
    const st_image_metadata_descriptor_t *metadata, uint32_t runtime_id)
{
    if (metadata == NULL || runtime_id == 0u) {
        return NULL;
    }
    for (size_t index = 0u; index < metadata->entity_count; index++) {
        if (metadata->entity_runtime_class_ids[index] == runtime_id) {
            return &metadata->entities[index];
        }
    }
    return NULL;
}

static bool span_within(uint32_t offset, uint32_t count, uint32_t total)
{
    return offset <= total && count <= total - offset;
}

static bool pointer_bitmap_is_prefix(const StShapeDescriptor *shape,
                                     size_t bit_count)
{
    size_t words = bit_count == 0u ? 0u : (bit_count + 63u) / 64u;
    if (shape->fixed_pointer_bitmap_word_count != words
            || ((shape->fixed_pointer_bitmap == NULL) != (words == 0u))) {
        return false;
    }
    for (size_t word = 0u; word < words; word++) {
        uint64_t expected = UINT64_MAX;
        if (word + 1u == words && (bit_count & 63u) != 0u) {
            expected = (UINT64_C(1) << (bit_count & 63u)) - 1u;
        }
        if (shape->fixed_pointer_bitmap[word] != expected) {
            return false;
        }
    }
    return true;
}

static bool indexed_values_layout(
    const st_runtime_descriptors_t *descriptors,
    uint32_t class_id, uint32_t shape_id, bool require_abstract)
{
    const StClassDescriptor *class_descriptor = st_runtime_class(
        descriptors, class_id);
    const StShapeDescriptor *shape = st_runtime_shape(descriptors, shape_id);
    return class_descriptor != NULL && shape != NULL
        && (class_descriptor->flags & ST_CLASS_METACLASS) == 0u
        && ((class_descriptor->flags & ST_CLASS_ABSTRACT) != 0u)
            == require_abstract
        && shape->class_id == class_id && shape->fixed_word_count == 0u
        && shape->fixed_pointer_bitmap == NULL
        && shape->fixed_pointer_bitmap_word_count == 0u
        && shape->indexed_format == ST_INDEXED_VALUES;
}

static bool validate_named_slots(
    const st_image_metadata_descriptor_t *metadata,
    const st_image_entity_metadata_t *entity,
    const char *const *names, size_t name_count)
{
    if (entity == NULL || entity->instance_slot_count != name_count
            || !span_within(entity->instance_slot_offset,
                            entity->instance_slot_count,
                            metadata->instance_slot_count)) {
        return false;
    }
    for (size_t index = 0u; index < name_count; index++) {
        const st_image_slot_metadata_t *slot = &metadata->instance_slots[
            entity->instance_slot_offset + index];
        if (slot->kind != ST_CLASS_GRAPH_INSTANCE_SLOT
                || slot->slot != index || slot->name == NULL
                || strcmp(slot->name, names[index]) != 0) {
            return false;
        }
    }
    return true;
}

static bool recipe_matches_shape(uint32_t recipe,
                                 const StShapeDescriptor *shape)
{
    if (shape == NULL) {
        return false;
    }
    switch ((st_image_layout_recipe_t)recipe) {
    case ST_IMAGE_LAYOUT_FIXED_POINTERS:
        return shape->indexed_format == ST_INDEXED_NONE
            && pointer_bitmap_is_prefix(shape, shape->fixed_word_count);
    case ST_IMAGE_LAYOUT_INDEXED_VALUES:
        return shape->indexed_format == ST_INDEXED_VALUES
            && pointer_bitmap_is_prefix(shape, shape->fixed_word_count);
    case ST_IMAGE_LAYOUT_INDEXED_UINT8:
        return shape->indexed_format == ST_INDEXED_UINT8
            && pointer_bitmap_is_prefix(shape, shape->fixed_word_count);
    case ST_IMAGE_LAYOUT_INDEXED_UINT16:
        return shape->indexed_format == ST_INDEXED_UINT16
            && pointer_bitmap_is_prefix(shape, shape->fixed_word_count);
    case ST_IMAGE_LAYOUT_INDEXED_UINT32:
        return shape->indexed_format == ST_INDEXED_UINT32
            && pointer_bitmap_is_prefix(shape, shape->fixed_word_count);
    case ST_IMAGE_LAYOUT_BOXED_FLOAT64:
        return shape->indexed_format == ST_INDEXED_NONE
            && shape->fixed_word_count == 1u
            && shape->fixed_pointer_bitmap_word_count == 1u
            && shape->fixed_pointer_bitmap != NULL
            && shape->fixed_pointer_bitmap[0] == 0u;
    case ST_IMAGE_LAYOUT_CLOSURE:
        return shape->indexed_format == ST_INDEXED_VALUES
            && shape->fixed_word_count == 4u;
    case ST_IMAGE_LAYOUT_CELL:
        return shape->indexed_format == ST_INDEXED_NONE
            && shape->fixed_word_count == 1u
            && pointer_bitmap_is_prefix(shape, 1u);
    case ST_IMAGE_LAYOUT_LARGE_INTEGER:
        return shape->indexed_format == ST_INDEXED_UINT32
            && shape->fixed_word_count == 1u
            && shape->fixed_pointer_bitmap_word_count == 1u
            && shape->fixed_pointer_bitmap != NULL
            && shape->fixed_pointer_bitmap[0] == 0u;
    default: return false;
    }
}

static st_aot_bootstrap_status_t validate_metadata(
    const st_aot_bootstrap_options_t *options,
    st_heap_t **heap_out, const st_runtime_descriptors_t **descriptors_out)
{
    static const char *const class_slots[ST_CLASS_SLOT_COUNT] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "name", "comment", "category",
        "classVariables", "namespace"
    };
    static const char *const metaclass_slots[ST_METACLASS_SLOT_COUNT] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "instanceClass"
    };
    const st_image_metadata_descriptor_t *metadata = options->metadata;
    const st_runtime_descriptors_t *descriptors;
    st_heap_t *heap;
    uint16_t endian_probe = UINT16_C(1);
    uint32_t native_endian = *(const uint8_t *)&endian_probe != 0u
        ? (uint32_t)ANVIL_ENDIAN_LITTLE : (uint32_t)ANVIL_ENDIAN_BIG;
    uint32_t expected_flags = ST_IMAGE_METADATA_FLAG_TYPED_RELOCATIONS
        | ST_IMAGE_METADATA_FLAG_METHOD_CODE
        | ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS
        | ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS;
    size_t entity_kind_count[3] = {0u, 0u, 0u};
    size_t runtime_seen = 0u;

    if (metadata == NULL || metadata->magic != ST_IMAGE_METADATA_MAGIC
            || metadata->abi_version != ST_IMAGE_METADATA_ABI_VERSION
            || metadata->pointer_size != sizeof(void *)
            || metadata->endian != native_endian
            || (metadata->flags & expected_flags) != expected_flags
            || (metadata->flags & ST_IMAGE_METADATA_FLAG_METADATA_ONLY) != 0u
            || metadata->entity_count == 0u
            || metadata->runtime_class_count == 0u
            || metadata->runtime_shape_count == 0u
            || metadata->runtime_layout_count
                != metadata->runtime_shape_count
            || metadata->entities == NULL
            || (metadata->selectors == NULL)
                != (metadata->selector_count == 0u)
            || (metadata->instance_slots == NULL)
                != (metadata->instance_slot_count == 0u)
            || (metadata->class_variables == NULL)
                != (metadata->class_variable_count == 0u)
            || metadata->entity_runtime_class_ids == NULL
            || metadata->runtime_layouts == NULL
            || metadata->runtime_descriptors == NULL
            || (metadata->methods == NULL) != (metadata->method_count == 0u)
            || (metadata->runtime_methods == NULL)
                != (metadata->method_count == 0u)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
    }

    heap = st_image_runtime_heap(options->image);
    descriptors = heap == NULL ? NULL : st_heap_descriptors(heap);
    if (heap == NULL || descriptors == NULL
            || descriptors != metadata->runtime_descriptors
            || options->lookup == NULL || !options->lookup->initialized
            || options->lookup->descriptors != descriptors
            || options->symbols == NULL || !options->symbols->initialized
            || options->symbols->abi_version != ST_SYMBOL_INTERN_ABI_VERSION
            || options->symbols->image != options->image
            || st_runtime_descriptors_validate(descriptors) != ST_RUNTIME_OK
            || descriptors->class_count != metadata->runtime_class_count
            || descriptors->shape_count != metadata->runtime_shape_count) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR;
    }
    size_t source_class_count = (size_t)metadata->class_count
        + (size_t)metadata->metaclass_count;
    if (source_class_count != metadata->runtime_class_count
            || source_class_count > metadata->entity_count) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
    }

    for (size_t index = 0u; index < metadata->entity_count; index++) {
        const st_image_entity_metadata_t *entity = &metadata->entities[index];
        uint32_t runtime_id = metadata->entity_runtime_class_ids[index];
        if (entity->id != index + 1u || entity->kind > ST_CLASS_GRAPH_NAMESPACE
                || entity->name == NULL
                || entity->namespace_id > metadata->entity_count
                || !span_within(entity->instance_slot_offset,
                                entity->instance_slot_count,
                                metadata->instance_slot_count)
                || !span_within(entity->class_variable_offset,
                                entity->class_variable_count,
                                metadata->class_variable_count)) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        entity_kind_count[entity->kind]++;
        if (entity->namespace_id != 0u) {
            const st_image_entity_metadata_t *namespace_entity = entity_at(
                metadata, entity->namespace_id);
            if (namespace_entity == NULL
                    || namespace_entity->kind != ST_CLASS_GRAPH_NAMESPACE) {
                return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
            }
        }
        if (entity->kind == ST_CLASS_GRAPH_NAMESPACE) {
            if (runtime_id != 0u) {
                return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
            }
            continue;
        }
        if (runtime_id == 0u || runtime_id > metadata->runtime_class_count) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        for (size_t prior = 0u; prior < index; prior++) {
            if (metadata->entity_runtime_class_ids[prior] == runtime_id) {
                return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
            }
        }
        runtime_seen++;
        const StClassDescriptor *descriptor = st_runtime_class(
            descriptors, runtime_id);
        uint32_t expected_super = entity_runtime_id(
            metadata, entity->superclass_id);
        if (entity->kind == ST_CLASS_GRAPH_METACLASS
                && entity->superclass_id == 0u) {
            expected_super = entity_runtime_id(
                metadata, options->class_object_layout_entity_id);
        }
        uint32_t expected_meta = entity->kind == ST_CLASS_GRAPH_METACLASS
            ? runtime_id : entity_runtime_id(metadata, entity->metaclass_id);
        if (descriptor == NULL || descriptor->superclass_id != expected_super
                || descriptor->metaclass_id != expected_meta
                || descriptor->name_length != strlen(entity->name)
                || memcmp(descriptor->name, entity->name,
                          descriptor->name_length) != 0
                || ((descriptor->flags & ST_CLASS_METACLASS) != 0u)
                    != (entity->kind == ST_CLASS_GRAPH_METACLASS)) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR;
        }
        if (entity->kind == ST_CLASS_GRAPH_METACLASS
                && descriptor->metaclass_id != descriptor->class_id) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_HIERARCHY;
        }
    }
    if (runtime_seen != metadata->runtime_class_count
            || entity_kind_count[ST_CLASS_GRAPH_CLASS]
                    != metadata->class_count
            || entity_kind_count[ST_CLASS_GRAPH_METACLASS]
                    != metadata->metaclass_count
            || entity_kind_count[ST_CLASS_GRAPH_NAMESPACE]
                    != metadata->namespace_count) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
    }

    for (size_t index = 0u; index < metadata->runtime_layout_count; index++) {
        const st_image_runtime_layout_metadata_t *layout =
            &metadata->runtime_layouts[index];
        const StShapeDescriptor *shape = st_runtime_shape(
            descriptors, layout->runtime_shape_id);
        if (layout->runtime_shape_id != index + 1u || shape == NULL
                || layout->runtime_class_id != shape->class_id
                || entity_runtime_id(metadata, layout->graph_entity_id)
                    != layout->runtime_class_id
                || !recipe_matches_shape(layout->recipe, shape)
                || ((layout->flags & ST_IMAGE_RUNTIME_LAYOUT_DEFAULT) != 0u)
                    != (st_runtime_class(descriptors, shape->class_id)
                            ->default_shape_id == shape->shape_id)
                || (layout->flags & ~ST_IMAGE_RUNTIME_LAYOUT_DEFAULT) != 0u) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
        }
    }

    for (size_t index = 0u; index < metadata->selector_count; index++) {
        if (metadata->selectors[index].id != index + 1u
                || metadata->selectors[index].spelling == NULL) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
    }
    for (size_t index = 0u; index < metadata->method_count; index++) {
        const st_image_method_metadata_t *method = &metadata->methods[index];
        const StMethodDescriptor *runtime_method;

        /* Method metadata is grouped by owning class so lookup tables can be
         * emitted without another indirection. Method IDs remain a dense
         * permutation, and the canonical descriptor array is indexed by
         * ID-1. Never assume the metadata traversal order is ID order. */
        if (method->id == 0u || method->id > metadata->method_count) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        runtime_method = &metadata->runtime_methods[method->id - 1u];
        if (method->selector_id == 0u
                || method->selector_id > metadata->selector_count
                || entity_runtime_id(metadata, method->owner) == 0u
                || method->runtime_descriptor != runtime_method
                || runtime_method->abi_version != ST_METHOD_ABI_VERSION
                || runtime_method->selector_id != method->selector_id
                || runtime_method->owner_class_id
                    != entity_runtime_id(metadata, method->owner)) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
    }
    size_t emitted_slot_count = 0u;
    for (size_t class_index = 0u;
         class_index < descriptors->class_count; class_index++) {
        const StClassDescriptor *class_descriptor =
            descriptors->classes[class_index];
        if (!add_size(emitted_slot_count,
                      class_descriptor->method_slot_count,
                      &emitted_slot_count)) {
            return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
        }
        for (size_t slot_index = 0u;
             slot_index < class_descriptor->method_slot_count; slot_index++) {
            const st_method_slot_t *slot =
                &class_descriptor->method_slots[slot_index];
            const StMethodBinding *binding = st_method_entry_load(slot->entry);
            size_t matches = 0u;
            for (size_t method_index = 0u;
                 method_index < metadata->method_count; method_index++) {
                const st_image_method_metadata_t *method =
                    &metadata->methods[method_index];
                if (method->runtime_descriptor == binding->descriptor) {
                    if (method->selector_id != slot->selector_id
                            || entity_runtime_id(metadata, method->owner)
                                != class_descriptor->class_id) {
                        return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
                    }
                    matches++;
                }
            }
            if (matches != 1u) {
                return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
            }
        }
    }
    if (emitted_slot_count != metadata->method_count) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
    }

    const st_image_entity_metadata_t *object = entity_at(
        metadata, options->object_entity_id);
    const st_image_entity_metadata_t *class_layout = entity_at(
        metadata, options->class_object_layout_entity_id);
    const st_image_entity_metadata_t *metaclass = entity_at(
        metadata, options->metaclass_entity_id);
    const st_image_entity_metadata_t *integer = entity_at(
        metadata, options->integer_entity_id);
    const st_image_entity_metadata_t *small_integer = entity_at(
        metadata, options->small_integer_entity_id);
    if (object == NULL || class_layout == NULL || metaclass == NULL
            || integer == NULL || small_integer == NULL
            || object->kind != ST_CLASS_GRAPH_CLASS
            || class_layout->kind != ST_CLASS_GRAPH_CLASS
            || metaclass->kind != ST_CLASS_GRAPH_CLASS
            || integer->kind != ST_CLASS_GRAPH_CLASS
            || small_integer->kind != ST_CLASS_GRAPH_CLASS
            || object->superclass_id != 0u
            || small_integer->superclass_id != integer->id
            || !validate_named_slots(metadata, class_layout, class_slots,
                                     ST_CLASS_SLOT_COUNT)
            || !validate_named_slots(metadata, metaclass, metaclass_slots,
                                     ST_METACLASS_SLOT_COUNT)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_HIERARCHY;
    }
    const StShapeDescriptor *class_shape = st_runtime_shape(
        descriptors, st_runtime_class(
            descriptors, entity_runtime_id(metadata, class_layout->id))
            ->default_shape_id);
    const StShapeDescriptor *metaclass_shape = st_runtime_shape(
        descriptors, st_runtime_class(
            descriptors, entity_runtime_id(metadata, metaclass->id))
            ->default_shape_id);
    if (class_shape == NULL || class_shape->fixed_word_count != 9u
            || class_shape->indexed_format != ST_INDEXED_NONE
            || !pointer_bitmap_is_prefix(class_shape, 9u)
            || metaclass_shape == NULL
            || metaclass_shape->fixed_word_count != 5u
            || metaclass_shape->indexed_format != ST_INDEXED_NONE
            || !pointer_bitmap_is_prefix(metaclass_shape, 5u)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
    }
    for (size_t index = 0u; index < descriptors->class_count; index++) {
        const StClassDescriptor *represented = descriptors->classes[index];
        const StClassDescriptor *actual = st_runtime_class(
            descriptors, represented->metaclass_id);
        const StShapeDescriptor *shape = actual == NULL ? NULL
            : st_runtime_shape(descriptors, actual->default_shape_id);
        if (actual == NULL || (actual->flags & ST_CLASS_METACLASS) == 0u
                || shape == NULL || shape->class_id != actual->class_id
                || shape->fixed_word_count != ST_CLASS_SLOT_COUNT
                || shape->indexed_format != ST_INDEXED_NONE
                || !pointer_bitmap_is_prefix(shape, ST_CLASS_SLOT_COUNT)) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
        }
    }
    if (!indexed_values_layout(descriptors, options->array_class_id,
                               options->array_shape_id, false)
            || !indexed_values_layout(
                descriptors, options->method_dictionary_class_id,
                options->method_dictionary_shape_id, true)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
    }
    *heap_out = heap;
    *descriptors_out = descriptors;
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t map_heap_status(st_heap_status_t status)
{
    switch (status) {
    case ST_HEAP_OK:
        return ST_AOT_BOOTSTRAP_OK;
    case ST_HEAP_ERR_OUT_OF_MEMORY:
        return ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY;
    case ST_HEAP_ERR_OVERFLOW:
        return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
    default:
        return ST_AOT_BOOTSTRAP_ERR_HEAP;
    }
}

static st_aot_bootstrap_status_t allocate_indexed(
    st_aot_bootstrap_state_t *state, uint32_t class_id, uint32_t shape_id,
    size_t count, st_value_t *value_out)
{
    return map_heap_status(st_heap_allocate(
        state->heap, class_id, shape_id, count, count,
        ST_HEADER_IMMUTABLE, value_out));
}

static st_aot_bootstrap_status_t allocate_state(
    st_aot_bootstrap_context_t *context,
    const st_aot_bootstrap_options_t *options,
    st_heap_t *heap, const st_runtime_descriptors_t *descriptors,
    st_primitive_allocator_t allocator,
    st_aot_bootstrap_state_t **state_out)
{
    const st_image_metadata_descriptor_t *metadata = options->metadata;
    st_aot_bootstrap_state_t *state;
    size_t symbol_count;
    size_t auxiliary_count;
    size_t root_count;
    size_t root_bytes;
    if (!add_size(metadata->selector_count, metadata->entity_count,
                  &symbol_count)
            || !add_size(symbol_count, metadata->instance_slot_count,
                         &symbol_count)
            || !add_size(symbol_count, metadata->class_variable_count,
                         &symbol_count)
            || !multiply_size(metadata->runtime_class_count, 4u,
                              &auxiliary_count)
            || !add_size(metadata->runtime_class_count, symbol_count,
                         &root_count)
            || !add_size(root_count, auxiliary_count, &root_count)
            || !multiply_size(root_count, sizeof(st_value_t), &root_bytes)) {
        return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
    }
    state = allocator.allocate(allocator.user, sizeof(*state));
    if (state == NULL) {
        return ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->allocator = allocator;
    state->metadata = metadata;
    state->descriptors = descriptors;
    state->heap = heap;
    state->class_count = metadata->runtime_class_count;
    state->symbol_count = symbol_count;
    state->selector_count = metadata->selector_count;
    state->root_count = root_count;
    state->roots = allocator.allocate(allocator.user, root_bytes);
    if (state->roots == NULL) {
        release(allocator, state);
        return ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY;
    }
    for (size_t index = 0u; index < root_count; index++) {
        state->roots[index] = st_value_nil();
    }
    state->class_objects = state->roots;
    state->symbols = state->class_objects + state->class_count;
    state->selector_symbols = state->symbols;
    state->subclass_arrays = state->symbols + state->symbol_count;
    state->method_dictionaries = state->subclass_arrays + state->class_count;
    state->instance_variable_arrays =
        state->method_dictionaries + state->class_count;
    state->class_variable_arrays =
        state->instance_variable_arrays + state->class_count;
    state->magic = ST_AOT_BOOTSTRAP_MAGIC;
    context->image = options->image;
    context->symbols = options->symbols;
    context->lookup = options->lookup;
    context->state = state;
    context->root_provider = (st_image_root_provider_t) {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION, context, bootstrap_root_span
    };
    *state_out = state;
    return ST_AOT_BOOTSTRAP_OK;
}

static void destroy_state(st_aot_bootstrap_state_t *state)
{
    st_primitive_allocator_t allocator;
    if (state == NULL) {
        return;
    }
    allocator = state->allocator;
    state->magic = 0u;
    release(allocator, state->roots);
    memset(state, 0, sizeof(*state));
    release(allocator, state);
}

static st_aot_bootstrap_status_t build_spellings(
    st_aot_bootstrap_state_t *state, st_symbol_utf8_t **spellings_out)
{
    const st_image_metadata_descriptor_t *metadata = state->metadata;
    size_t bytes;
    st_symbol_utf8_t *spellings;
    size_t output = 0u;
    *spellings_out = NULL;
    if (!multiply_size(state->symbol_count, sizeof(*spellings), &bytes)) {
        return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
    }
    spellings = state->allocator.allocate(state->allocator.user, bytes);
    if (spellings == NULL) {
        return ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY;
    }
    for (size_t index = 0u; index < metadata->selector_count; index++) {
        const char *text = metadata->selectors[index].spelling;
        spellings[output++] = (st_symbol_utf8_t) { text, strlen(text) };
    }
    for (size_t index = 0u; index < metadata->entity_count; index++) {
        const char *text = metadata->entities[index].name;
        spellings[output++] = (st_symbol_utf8_t) { text, strlen(text) };
    }
    for (size_t index = 0u; index < metadata->instance_slot_count; index++) {
        const char *text = metadata->instance_slots[index].name;
        if (text == NULL) {
            release(state->allocator, spellings);
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        spellings[output++] = (st_symbol_utf8_t) { text, strlen(text) };
    }
    for (size_t index = 0u; index < metadata->class_variable_count; index++) {
        const char *text = metadata->class_variables[index].name;
        if (text == NULL) {
            release(state->allocator, spellings);
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        spellings[output++] = (st_symbol_utf8_t) { text, strlen(text) };
    }
    if (output != state->symbol_count) {
        abort();
    }
    *spellings_out = spellings;
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t allocate_managed_graph(
    st_aot_bootstrap_state_t *state,
    const st_aot_bootstrap_options_t *options)
{
    const st_image_metadata_descriptor_t *metadata = state->metadata;
    for (size_t index = 0u; index < state->class_count; index++) {
        const StClassDescriptor *represented = state->descriptors->classes[index];
        const StClassDescriptor *actual = st_runtime_class(
            state->descriptors, represented->metaclass_id);
        st_aot_bootstrap_status_t status = map_heap_status(st_heap_allocate(
            state->heap, actual->class_id, actual->default_shape_id,
            0u, 0u, ST_HEADER_IMMUTABLE, &state->class_objects[index]));
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }

        size_t subclass_count = 0u;
        for (size_t candidate = 0u; candidate < state->class_count;
             candidate++) {
            if (state->descriptors->classes[candidate]->superclass_id
                    == represented->class_id) {
                subclass_count++;
            }
        }
        const st_image_entity_metadata_t *entity = runtime_entity(
            metadata, represented->class_id);
        if (entity == NULL) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA;
        }
        size_t dictionary_length;
        if (!multiply_size(represented->method_slot_count, 2u,
                           &dictionary_length)) {
            return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
        }
        status = allocate_indexed(
            state, options->array_class_id, options->array_shape_id,
            subclass_count, &state->subclass_arrays[index]);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        status = allocate_indexed(
            state, options->method_dictionary_class_id,
            options->method_dictionary_shape_id, dictionary_length,
            &state->method_dictionaries[index]);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        status = allocate_indexed(
            state, options->array_class_id, options->array_shape_id,
            entity->instance_slot_count,
            &state->instance_variable_arrays[index]);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        status = allocate_indexed(
            state, options->array_class_id, options->array_shape_id,
            entity->class_variable_count,
            &state->class_variable_arrays[index]);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
    }
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t indexed_value_view(
    st_aot_bootstrap_state_t *state, st_value_t value,
    uint32_t class_id, uint32_t shape_id, size_t length,
    st_object_view_t *view_out)
{
    st_heap_status_t status = st_heap_object_view(state->heap, value, view_out);
    if (status != ST_HEAP_OK) {
        return map_heap_status(status);
    }
    if (view_out->class_descriptor->class_id != class_id
            || view_out->shape_descriptor->shape_id != shape_id
            || view_out->shape_descriptor->indexed_format != ST_INDEXED_VALUES
            || view_out->indexed_length != length
            || view_out->indexed_capacity != length
            || (length != 0u && view_out->indexed_elements == NULL)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
    }
    return ST_AOT_BOOTSTRAP_OK;
}

static st_value_t entity_symbol(const st_aot_bootstrap_state_t *state,
                                uint32_t entity_id)
{
    return state->symbols[state->selector_count + entity_id - 1u];
}

static st_value_t instance_slot_symbol(
    const st_aot_bootstrap_state_t *state, size_t slot_index)
{
    return state->symbols[state->selector_count
        + state->metadata->entity_count + slot_index];
}

static st_value_t class_variable_symbol(
    const st_aot_bootstrap_state_t *state, size_t slot_index)
{
    return state->symbols[state->selector_count
        + state->metadata->entity_count
        + state->metadata->instance_slot_count + slot_index];
}

static st_aot_bootstrap_status_t validate_prepared_symbols(
    st_aot_bootstrap_state_t *state,
    const st_aot_bootstrap_options_t *options)
{
    for (size_t index = 0u; index < state->symbol_count; index++) {
        st_object_view_t view;
        if (st_heap_object_view(state->heap, state->symbols[index], &view)
                != ST_HEAP_OK
                || view.class_descriptor->class_id
                    != options->symbol_class_id
                || (view.class_descriptor->flags & ST_CLASS_ABSTRACT) == 0u
                || view.shape_descriptor->fixed_word_count != 0u
                || (view.shape_descriptor->indexed_format != ST_INDEXED_UINT8
                    && view.shape_descriptor->indexed_format
                        != ST_INDEXED_UINT16
                    && view.shape_descriptor->indexed_format
                        != ST_INDEXED_UINT32)
                || (st_object_header_flags(
                    st_object_header_load(&view.object->header))
                    & ST_HEADER_IMMUTABLE) == 0u) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
        }
    }
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t fill_managed_graph(
    st_aot_bootstrap_state_t *state,
    const st_aot_bootstrap_options_t *options)
{
    const st_image_metadata_descriptor_t *metadata = state->metadata;
    for (size_t index = 0u; index < state->class_count; index++) {
        const StClassDescriptor *represented = state->descriptors->classes[index];
        const st_image_entity_metadata_t *entity = runtime_entity(
            metadata, represented->class_id);
        st_object_view_t class_view;
        st_object_view_t subclass_view;
        st_object_view_t ivar_view;
        st_object_view_t class_var_view;
        st_aot_bootstrap_status_t status;
        size_t subclass_count = 0u;
        for (size_t candidate = 0u; candidate < state->class_count;
             candidate++) {
            if (state->descriptors->classes[candidate]->superclass_id
                    == represented->class_id) {
                subclass_count++;
            }
        }
        if (entity == NULL
                || st_heap_object_view(state->heap,
                                       state->class_objects[index],
                                       &class_view) != ST_HEAP_OK
                || class_view.class_descriptor->class_id
                    != represented->metaclass_id
                || class_view.shape_descriptor->fixed_word_count
                    != ST_CLASS_SLOT_COUNT
                || class_view.shape_descriptor->indexed_format
                    != ST_INDEXED_NONE) {
            return ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT;
        }
        status = indexed_value_view(
            state, state->subclass_arrays[index], options->array_class_id,
            options->array_shape_id, subclass_count, &subclass_view);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        status = indexed_value_view(
            state, state->instance_variable_arrays[index],
            options->array_class_id, options->array_shape_id,
            entity->instance_slot_count, &ivar_view);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        status = indexed_value_view(
            state, state->class_variable_arrays[index],
            options->array_class_id, options->array_shape_id,
            entity->class_variable_count, &class_var_view);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }

        st_value_t *subclasses = subclass_view.indexed_elements;
        size_t output = 0u;
        for (size_t candidate = 0u; candidate < state->class_count;
             candidate++) {
            if (state->descriptors->classes[candidate]->superclass_id
                    == represented->class_id) {
                subclasses[output++] = state->class_objects[candidate];
            }
        }
        if (output != subclass_count) {
            abort();
        }
        st_value_t *ivars = ivar_view.indexed_elements;
        for (size_t slot = 0u; slot < entity->instance_slot_count; slot++) {
            ivars[slot] = instance_slot_symbol(
                state, entity->instance_slot_offset + slot);
        }
        st_value_t *class_vars = class_var_view.indexed_elements;
        for (size_t slot = 0u; slot < entity->class_variable_count; slot++) {
            class_vars[slot] = class_variable_symbol(
                state, entity->class_variable_offset + slot);
        }

        st_value_t *slots = class_view.fixed_words;
        slots[ST_BEHAVIOR_SUPERCLASS_SLOT] = represented->superclass_id == 0u
            ? st_value_nil()
            : state->class_objects[represented->superclass_id - 1u];
        slots[ST_BEHAVIOR_SUBCLASSES_SLOT] = state->subclass_arrays[index];
        slots[ST_BEHAVIOR_METHOD_DICTIONARY_SLOT] =
            state->method_dictionaries[index];
        slots[ST_BEHAVIOR_INSTANCE_VARIABLES_SLOT] =
            state->instance_variable_arrays[index];
        if (entity->kind == ST_CLASS_GRAPH_METACLASS) {
            uint32_t instance_class_id = entity_runtime_id(
                metadata, entity->instance_class_id);
            if (instance_class_id == 0u
                    || instance_class_id > state->class_count) {
                return ST_AOT_BOOTSTRAP_ERR_INVALID_HIERARCHY;
            }
            slots[ST_METACLASS_INSTANCE_CLASS_SLOT] =
                state->class_objects[instance_class_id - 1u];
            /* Slots 5..8 are physical ABI-v5 padding inherited from the Class
             * object layout. The source MetaClass contract ends at slot 4. */
        } else {
            slots[ST_CLASS_NAME_SLOT] = entity_symbol(state, entity->id);
            /* Comment/category are absent from ABI-v5 source metadata. */
            slots[ST_CLASS_COMMENT_SLOT] = st_value_nil();
            slots[ST_CLASS_CATEGORY_SLOT] = st_value_nil();
            slots[ST_CLASS_VARIABLES_SLOT] =
                state->class_variable_arrays[index];
            slots[ST_CLASS_NAMESPACE_SLOT] = entity->namespace_id == 0u
                ? st_value_nil()
                : entity_symbol(state, entity->namespace_id);
        }
    }
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t fill_method_dictionaries(
    st_aot_bootstrap_context_t *context,
    const st_aot_bootstrap_options_t *options)
{
    st_aot_bootstrap_state_t *state = context->state;
    for (size_t index = 0u; index < state->class_count; index++) {
        const StClassDescriptor *descriptor = state->descriptors->classes[index];
        size_t dictionary_length;
        st_object_view_t dictionary;
        if (!multiply_size(descriptor->method_slot_count, 2u,
                           &dictionary_length)) {
            return ST_AOT_BOOTSTRAP_ERR_OVERFLOW;
        }
        st_aot_bootstrap_status_t status = indexed_value_view(
            state, state->method_dictionaries[index],
            options->method_dictionary_class_id,
            options->method_dictionary_shape_id,
            dictionary_length, &dictionary);
        if (status != ST_AOT_BOOTSTRAP_OK) {
            return status;
        }
        st_value_t *pairs = dictionary.indexed_elements;
        for (size_t method = 0u; method < descriptor->method_slot_count;
             method++) {
            const st_method_slot_t *slot = &descriptor->method_slots[method];
            st_value_t mirror = ST_VALUE_INVALID;
            if (slot->selector_id == 0u
                    || slot->selector_id > state->selector_count
                    || st_reflection_compiled_method_for_entry(
                        &context->reflection, slot->entry, &mirror)
                        != ST_REFLECTION_PRIMITIVE_OK) {
                return ST_AOT_BOOTSTRAP_ERR_REFLECTION;
            }
            pairs[method * 2u] =
                state->selector_symbols[slot->selector_id - 1u];
            pairs[method * 2u + 1u] = mirror;
        }
    }
    return ST_AOT_BOOTSTRAP_OK;
}

static st_aot_bootstrap_status_t authenticate_roots(
    st_aot_bootstrap_state_t *state)
{
    for (size_t index = 0u; index < state->root_count; index++) {
        st_object_view_t view;
        if (st_heap_object_view(state->heap, state->roots[index], &view)
                != ST_HEAP_OK) {
            return ST_AOT_BOOTSTRAP_ERR_HEAP;
        }
    }
    return ST_AOT_BOOTSTRAP_OK;
}

st_aot_bootstrap_status_t st_aot_bootstrap_context_init(
    st_aot_bootstrap_context_t *context,
    const st_aot_bootstrap_options_t *options)
{
    st_primitive_allocator_t allocator;
    st_heap_t *heap = NULL;
    const st_runtime_descriptors_t *descriptors = NULL;
    st_aot_bootstrap_state_t *state = NULL;
    st_symbol_utf8_t *spellings = NULL;
    st_symbol_intern_batch_t symbol_batch = {0};
    st_aot_bootstrap_status_t status;
    bool provider_attached = false;
    bool reflection_ready = false;

    if (context == NULL || options == NULL || context->initialized
            || context->state != NULL || context->image != NULL
            || context->symbols != NULL || context->lookup != NULL
            || context->reflection.initialized
            || options->image == NULL || options->metadata == NULL
            || options->object_entity_id == 0u
            || options->class_object_layout_entity_id == 0u
            || options->metaclass_entity_id == 0u
            || options->integer_entity_id == 0u
            || options->small_integer_entity_id == 0u
            || options->array_class_id == 0u
            || options->array_shape_id == 0u
            || options->method_dictionary_class_id == 0u
            || options->method_dictionary_shape_id == 0u
            || options->symbol_class_id == 0u
            || options->compiled_method_class_id == 0u
            || options->compiled_method_shape_id == 0u
            || !normalize_allocator(options->allocator, &allocator)) {
        return ST_AOT_BOOTSTRAP_ERR_INVALID_ARGUMENT;
    }
    status = validate_metadata(options, &heap, &descriptors);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        return status;
    }
    status = allocate_state(context, options, heap, descriptors, allocator,
                            &state);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    status = build_spellings(state, &spellings);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    st_symbol_intern_status_t symbol_status =
        st_symbol_intern_batch_prepare_utf8(
            &symbol_batch, options->symbols, spellings,
            state->symbol_count);
    release(allocator, spellings);
    spellings = NULL;
    if (symbol_status != ST_SYMBOL_INTERN_OK) {
        status = symbol_status == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY
            ? ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY
            : symbol_status == ST_SYMBOL_INTERN_ERR_OVERFLOW
                ? ST_AOT_BOOTSTRAP_ERR_OVERFLOW
                : ST_AOT_BOOTSTRAP_ERR_SYMBOLS;
        goto failure;
    }
    size_t prepared_count = 0u;
    const st_value_t *prepared = st_symbol_intern_batch_values(
        &symbol_batch, &prepared_count);
    if (prepared == NULL || prepared_count != state->symbol_count) {
        status = ST_AOT_BOOTSTRAP_ERR_SYMBOLS;
        goto failure;
    }
    memcpy(state->symbols, prepared,
           state->symbol_count * sizeof(*state->symbols));
    status = validate_prepared_symbols(state, options);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    status = allocate_managed_graph(state, options);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    status = fill_managed_graph(state, options);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    status = authenticate_roots(state);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }

    if (!st_image_runtime_root_provider_attach(
            options->image, &context->root_provider)) {
        status = ST_AOT_BOOTSTRAP_ERR_CONFLICT;
        goto failure;
    }
    provider_attached = true;
    st_reflection_context_options_t reflection_options = {
        .image = options->image,
        .lookup = options->lookup,
        .class_objects_by_id = state->class_objects,
        .class_object_count = state->class_count,
        .selector_symbols_by_id = state->selector_symbols,
        .selector_symbol_count = state->selector_count,
        .symbol_class_id = options->symbol_class_id,
        .compiled_method_class_id = options->compiled_method_class_id,
        .compiled_method_shape_id = options->compiled_method_shape_id,
        .allocator = allocator
    };
    st_reflection_primitive_status_t reflection_status =
        st_reflection_context_init(&context->reflection,
                                   &reflection_options);
    if (reflection_status != ST_REFLECTION_PRIMITIVE_OK) {
        status = reflection_status == ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY
            ? ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY
            : reflection_status == ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR
                ? ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR
            : reflection_status
                    == ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT
                ? ST_AOT_BOOTSTRAP_ERR_INVALID_STATE
            : ST_AOT_BOOTSTRAP_ERR_REFLECTION;
        goto failure;
    }
    reflection_ready = true;
    status = fill_method_dictionaries(context, options);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    status = authenticate_roots(state);
    if (status != ST_AOT_BOOTSTRAP_OK) {
        goto failure;
    }
    symbol_status = st_symbol_intern_batch_commit(&symbol_batch);
    if (symbol_status != ST_SYMBOL_INTERN_OK) {
        status = symbol_status == ST_SYMBOL_INTERN_ERR_CONFLICT
            ? ST_AOT_BOOTSTRAP_ERR_CONFLICT
            : ST_AOT_BOOTSTRAP_ERR_SYMBOLS;
        goto failure;
    }
    st_symbol_intern_batch_destroy(&symbol_batch);
    context->abi_version = ST_AOT_BOOTSTRAP_ABI_VERSION;
    context->initialized = true;
    return ST_AOT_BOOTSTRAP_OK;

failure:
    release(allocator, spellings);
    if (reflection_ready) {
        st_reflection_context_destroy(&context->reflection);
    }
    if (provider_attached
            && !st_image_runtime_root_provider_detach(
                options->image, &context->root_provider)) {
        abort();
    }
    st_symbol_intern_batch_destroy(&symbol_batch);
    destroy_state(state);
    memset(context, 0, sizeof(*context));
    return status;
}

void st_aot_bootstrap_context_destroy(st_aot_bootstrap_context_t *context)
{
    st_aot_bootstrap_state_t *state = basic_state(context);
    if (context == NULL || state == NULL) {
        return;
    }
    if (context->reflection.initialized) {
        st_reflection_context_destroy(&context->reflection);
    }
    if (st_image_runtime_heap(context->image) != NULL
            && !st_image_runtime_root_provider_detach(
                context->image, &context->root_provider)) {
        abort();
    }
    memset(context, 0, sizeof(*context));
    destroy_state(state);
}

const st_value_t *st_aot_bootstrap_class_objects(
    const st_aot_bootstrap_context_t *context, size_t *count_out)
{
    st_aot_bootstrap_state_t *state = live_state(context);
    if (count_out != NULL) {
        *count_out = 0u;
    }
    if (state == NULL || count_out == NULL) {
        return NULL;
    }
    *count_out = state->class_count;
    return state->class_objects;
}

const st_value_t *st_aot_bootstrap_selector_symbols(
    const st_aot_bootstrap_context_t *context, size_t *count_out)
{
    st_aot_bootstrap_state_t *state = live_state(context);
    if (count_out != NULL) {
        *count_out = 0u;
    }
    if (state == NULL || count_out == NULL) {
        return NULL;
    }
    *count_out = state->selector_count;
    return state->selector_symbols;
}

const st_reflection_context_t *st_aot_bootstrap_reflection(
    const st_aot_bootstrap_context_t *context)
{
    return live_state(context) == NULL ? NULL : &context->reflection;
}

const char *st_aot_bootstrap_status_string(st_aot_bootstrap_status_t status)
{
    switch (status) {
    case ST_AOT_BOOTSTRAP_OK:
        return "ok";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_STATE:
        return "invalid state";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA:
        return "invalid ABI-v5 metadata";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR:
        return "descriptor mismatch";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT:
        return "managed layout mismatch";
    case ST_AOT_BOOTSTRAP_ERR_INVALID_HIERARCHY:
        return "invalid class hierarchy";
    case ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case ST_AOT_BOOTSTRAP_ERR_OVERFLOW:
        return "size overflow";
    case ST_AOT_BOOTSTRAP_ERR_HEAP:
        return "heap authentication failed";
    case ST_AOT_BOOTSTRAP_ERR_SYMBOLS:
        return "Symbol batch failed";
    case ST_AOT_BOOTSTRAP_ERR_REFLECTION:
        return "reflection bootstrap failed";
    case ST_AOT_BOOTSTRAP_ERR_CONFLICT:
        return "bootstrap publication conflict";
    default:
        return "unknown AOT bootstrap status";
    }
}
