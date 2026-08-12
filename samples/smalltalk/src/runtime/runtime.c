#include "st_runtime.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define ST_BITS_PER_BITMAP_WORD 64u

_Static_assert(sizeof(st_value_t) == 8, "runtime requires 64-bit StValue");
_Static_assert(sizeof(StMethodDescriptor) == 96u,
               "method descriptor is part of the tagged64 metadata ABI");
_Static_assert(offsetof(st_heap_object_t, payload) == 24,
               "heap object prefix is part of the runtime ABI");
_Static_assert(_Alignof(st_heap_object_t) >= 8,
               "heap object header must remain naturally aligned");

static bool add_size(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool round_up(size_t value, size_t alignment, size_t *result)
{
    size_t mask;
    if (!is_power_of_two(alignment)) return false;
    mask = alignment - 1;
    if (value > SIZE_MAX - mask) return false;
    *result = (value + mask) & ~mask;
    return true;
}

static bool bitmap_size(size_t bit_count, size_t *word_count_out)
{
    if (bit_count > SIZE_MAX - (ST_BITS_PER_BITMAP_WORD - 1)) return false;
    *word_count_out = (bit_count + (ST_BITS_PER_BITMAP_WORD - 1)) /
                      ST_BITS_PER_BITMAP_WORD;
    return true;
}

static bool bitmap_is_canonical(const uint64_t *bitmap, size_t word_count,
                                size_t bit_count)
{
    size_t required;
    unsigned remainder;
    if (!bitmap_size(bit_count, &required) || word_count != required)
        return false;
    if (required == 0) return bitmap == NULL;
    if (!bitmap) return false;
    remainder = (unsigned)(bit_count & 63u);
    if (remainder != 0 &&
        (bitmap[required - 1] & ~(UINT64_MAX >> (64u - remainder))) != 0)
        return false;
    return true;
}

static size_t indexed_element_size(st_indexed_format_t format)
{
    switch (format) {
    case ST_INDEXED_NONE: return 0;
    case ST_INDEXED_VALUES: return sizeof(st_value_t);
    case ST_INDEXED_UINT8: return sizeof(uint8_t);
    case ST_INDEXED_UINT16: return sizeof(uint16_t);
    case ST_INDEXED_UINT32: return sizeof(uint32_t);
    case ST_INDEXED_UINT64: return sizeof(uint64_t);
    default: return 0;
    }
}

bool st_method_descriptor_is_valid(const StMethodDescriptor *descriptor)
{
    size_t index;
    bool saw_non_local_return = false;
    if (!descriptor || descriptor->abi_version != ST_METHOD_ABI_VERSION ||
        descriptor->selector_id == 0 || descriptor->owner_class_id == 0 ||
        descriptor->owner_class_id > ST_HEADER_CLASS_MAX ||
        (descriptor->flags & ~(uint32_t)ST_METHOD_FLAGS_MASK) != 0u ||
        ((descriptor->flags & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0 &&
         (descriptor->flags & ST_METHOD_CAN_UNWIND) == 0) ||
        ((!descriptor->source_name) !=
         (descriptor->source_name_length == 0)) ||
        descriptor->source_start_offset > descriptor->source_end_offset ||
        ((!descriptor->root_maps) != (descriptor->root_map_count == 0)) ||
        ((!descriptor->unwind_regions) !=
         (descriptor->unwind_region_count == 0)) ||
        (descriptor->unwind_region_count != 0 &&
         ((descriptor->flags & ST_METHOD_CAN_UNWIND) == 0 ||
          descriptor->code_size == 0)))
        return false;

    for (index = 0; index < descriptor->root_map_count; index++) {
        const st_root_map_t *map = &descriptor->root_maps[index];
        size_t words;
        if (map->safepoint_id == 0 ||
            map->root_count > descriptor->frame_root_capacity ||
            (index != 0 && descriptor->root_maps[index - 1].safepoint_id >=
                           map->safepoint_id) ||
            !bitmap_size(map->root_count, &words) ||
            words != map->bitmap_word_count ||
            !bitmap_is_canonical(map->live_root_bitmap,
                                 map->bitmap_word_count, map->root_count))
            return false;
    }
    for (index = 0; index < descriptor->unwind_region_count; index++) {
        const st_unwind_region_t *region =
            &descriptor->unwind_regions[index];
        if (region->start_pc_offset >= region->end_pc_offset ||
            region->end_pc_offset > descriptor->code_size ||
            region->landing_pad_pc_offset >= descriptor->code_size ||
            region->kind < ST_UNWIND_CATCH ||
            region->kind > ST_UNWIND_NON_LOCAL_RETURN ||
            ((region->kind == ST_UNWIND_CATCH) !=
             (region->catch_class_id != 0)) ||
            region->catch_class_id > ST_HEADER_CLASS_MAX)
            return false;
        if (region->kind == ST_UNWIND_NON_LOCAL_RETURN)
            saw_non_local_return = true;
        if (index != 0) {
            const st_unwind_region_t *previous =
                &descriptor->unwind_regions[index - 1];
            if (previous->start_pc_offset > region->start_pc_offset ||
                (previous->start_pc_offset == region->start_pc_offset &&
                 previous->end_pc_offset < region->end_pc_offset) ||
                (previous->start_pc_offset < region->start_pc_offset &&
                 region->start_pc_offset < previous->end_pc_offset &&
                 region->end_pc_offset > previous->end_pc_offset))
                return false;
        }
    }
    return !saw_non_local_return ||
           (descriptor->flags & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0;
}

bool st_method_binding_is_valid(const StMethodBinding *binding)
{
    return binding && binding->code && binding->version != 0 &&
           st_method_descriptor_is_valid(binding->descriptor);
}

bool st_method_entry_init(StMethodEntry *entry,
                          const StMethodBinding *initial_binding)
{
    if (!entry || !st_method_binding_is_valid(initial_binding)) return false;
    entry->selector_id = initial_binding->descriptor->selector_id;
    entry->owner_class_id = initial_binding->descriptor->owner_class_id;
    atomic_init(&entry->binding, initial_binding);
    return true;
}

const StMethodBinding *st_method_entry_load(const StMethodEntry *entry)
{
    return entry ? atomic_load_explicit(&entry->binding, memory_order_acquire)
                 : NULL;
}

bool st_method_entry_publish(StMethodEntry *entry,
                             const StMethodBinding *new_binding,
                             const StMethodBinding **old_binding_out)
{
    const StMethodBinding *old;
    if (old_binding_out) *old_binding_out = NULL;
    if (!entry || !st_method_binding_is_valid(new_binding) ||
        new_binding->descriptor->selector_id != entry->selector_id ||
        new_binding->descriptor->owner_class_id != entry->owner_class_id)
        return false;
    old = atomic_load_explicit(&entry->binding, memory_order_acquire);
    do {
        if (!old || new_binding->version <= old->version) return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &entry->binding, &old, new_binding, memory_order_acq_rel,
        memory_order_acquire));
    if (old_binding_out) *old_binding_out = old;
    return true;
}

bool st_method_entry_compare_exchange(
    StMethodEntry *entry, const StMethodBinding **expected_in_out,
    const StMethodBinding *new_binding)
{
    if (!entry || !expected_in_out || !*expected_in_out ||
        !st_method_binding_is_valid(new_binding) ||
        new_binding->descriptor->selector_id != entry->selector_id ||
        new_binding->descriptor->owner_class_id != entry->owner_class_id ||
        new_binding->version <= (*expected_in_out)->version)
        return false;
    return atomic_compare_exchange_strong_explicit(
        &entry->binding, expected_in_out, new_binding,
        memory_order_acq_rel, memory_order_acquire);
}

bool st_shape_descriptor_extent(const StShapeDescriptor *shape,
                                size_t indexed_capacity, size_t *extent_out)
{
    size_t fixed_size;
    size_t indexed_size;
    size_t total;
    size_t element_size;
    if (extent_out) *extent_out = 0;
    if (!shape || !extent_out ||
        !is_power_of_two(shape->allocation_alignment) ||
        shape->allocation_alignment < _Alignof(st_heap_object_t))
        return false;
    element_size = indexed_element_size(shape->indexed_format);
    if ((shape->indexed_format == ST_INDEXED_NONE &&
         indexed_capacity != 0) ||
        (shape->indexed_format != ST_INDEXED_NONE && element_size == 0) ||
        !multiply_size(shape->fixed_word_count, sizeof(uint64_t),
                       &fixed_size) ||
        !multiply_size(indexed_capacity, element_size,
                       &indexed_size) ||
        !add_size(offsetof(st_heap_object_t, payload), fixed_size, &total) ||
        !add_size(total, indexed_size, &total) ||
        !round_up(total, shape->allocation_alignment, &total))
        return false;
    *extent_out = total;
    return true;
}

bool st_shape_descriptor_is_valid(const StShapeDescriptor *shape)
{
    size_t extent;
    return shape && shape->shape_id != 0 &&
           shape->shape_id <= ST_HEADER_SHAPE_MAX &&
           shape->class_id != 0 && shape->class_id <= ST_HEADER_CLASS_MAX &&
           bitmap_is_canonical(shape->fixed_pointer_bitmap,
                               shape->fixed_pointer_bitmap_word_count,
                               shape->fixed_word_count) &&
           st_shape_descriptor_extent(shape, 0, &extent) &&
           shape->minimum_allocation_size == extent;
}

bool st_class_descriptor_is_valid(const StClassDescriptor *descriptor)
{
    size_t index;
    if (!descriptor || descriptor->class_id == 0 ||
        descriptor->class_id > ST_HEADER_CLASS_MAX ||
        descriptor->superclass_id > ST_HEADER_CLASS_MAX ||
        descriptor->metaclass_id == 0 ||
        descriptor->metaclass_id > ST_HEADER_CLASS_MAX ||
        descriptor->default_shape_id == 0 ||
        descriptor->default_shape_id > ST_HEADER_SHAPE_MAX ||
        (descriptor->flags & ~(uint32_t)ST_CLASS_FLAGS_MASK) != 0u ||
        !descriptor->name || descriptor->name_length == 0 ||
        ((!descriptor->method_slots) !=
         (descriptor->method_slot_count == 0)))
        return false;
    for (index = 0; index < descriptor->method_slot_count; index++) {
        const st_method_slot_t *slot = &descriptor->method_slots[index];
        const StMethodBinding *binding;
        if (slot->selector_id == 0 || !slot->entry ||
            slot->entry->selector_id != slot->selector_id ||
            (index != 0 && descriptor->method_slots[index - 1].selector_id >=
                           slot->selector_id))
            return false;
        binding = st_method_entry_load(slot->entry);
        if (!st_method_binding_is_valid(binding) ||
            slot->entry->owner_class_id !=
                binding->descriptor->owner_class_id ||
            binding->descriptor->selector_id != slot->selector_id)
            return false;
    }
    return true;
}

const StClassDescriptor *st_runtime_class(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id)
{
    const StClassDescriptor *result;
    if (!descriptors || !descriptors->classes || class_id == 0 ||
        (uint64_t)class_id > (uint64_t)descriptors->class_count)
        return NULL;
    result = descriptors->classes[class_id - 1];
    return result && result->class_id == class_id ? result : NULL;
}

const StShapeDescriptor *st_runtime_shape(
    const st_runtime_descriptors_t *descriptors, uint32_t shape_id)
{
    const StShapeDescriptor *result;
    if (!descriptors || !descriptors->shapes || shape_id == 0 ||
        (uint64_t)shape_id > (uint64_t)descriptors->shape_count)
        return NULL;
    result = descriptors->shapes[shape_id - 1];
    return result && result->shape_id == shape_id ? result : NULL;
}

st_runtime_status_t st_runtime_descriptors_validate(
    const st_runtime_descriptors_t *descriptors)
{
    size_t index;
    if (!descriptors || !descriptors->classes || !descriptors->shapes ||
        descriptors->class_count == 0 || descriptors->shape_count == 0)
        return ST_RUNTIME_ERR_INVALID_ARGUMENT;
    if (descriptors->class_count > ST_HEADER_CLASS_MAX ||
        descriptors->shape_count > ST_HEADER_SHAPE_MAX)
        return ST_RUNTIME_ERR_ID_OUT_OF_RANGE;

    for (index = 0; index < descriptors->class_count; index++) {
        const StClassDescriptor *class_descriptor = descriptors->classes[index];
        const StShapeDescriptor *default_shape;
        uint32_t cursor;
        size_t hops = 0;
        if (!st_class_descriptor_is_valid(class_descriptor) ||
            class_descriptor->class_id != index + 1 ||
            (class_descriptor->superclass_id != 0 &&
             !st_runtime_class(descriptors,
                               class_descriptor->superclass_id)) ||
            !st_runtime_class(descriptors, class_descriptor->metaclass_id) ||
            (st_runtime_class(descriptors,
                              class_descriptor->metaclass_id)->flags &
             ST_CLASS_METACLASS) == 0)
            return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
        default_shape = st_runtime_shape(descriptors,
                                         class_descriptor->default_shape_id);
        if (!default_shape ||
            default_shape->class_id != class_descriptor->class_id)
            return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
        cursor = class_descriptor->superclass_id;
        bool ordinary_region =
            (class_descriptor->flags & ST_CLASS_METACLASS) == 0u;
        while (cursor != 0) {
            const StClassDescriptor *ancestor =
                st_runtime_class(descriptors, cursor);
            if (!ancestor || ++hops > descriptors->class_count)
                return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
            bool ancestor_is_metaclass =
                (ancestor->flags & ST_CLASS_METACLASS) != 0u;
            if (ordinary_region && ancestor_is_metaclass) {
                return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
            }
            if (!ancestor_is_metaclass) {
                /* A metaclass chain may bridge once into Class/Behavior's
                 * ordinary superclass chain. It may never cross back. */
                ordinary_region = true;
            }
            cursor = ancestor->superclass_id;
        }
        for (size_t slot_index = 0;
             slot_index < class_descriptor->method_slot_count; slot_index++) {
            const st_method_slot_t *slot =
                &class_descriptor->method_slots[slot_index];
            const StMethodBinding *binding = st_method_entry_load(slot->entry);
            uint32_t owner = binding->descriptor->owner_class_id;
            uint32_t candidate = class_descriptor->class_id;
            size_t owner_hops = 0;
            if (binding->descriptor->selector_id != slot->selector_id ||
                !st_runtime_class(descriptors, owner))
                return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
            for (size_t region_index = 0;
                 region_index < binding->descriptor->unwind_region_count;
                 region_index++) {
                uint32_t catch_id = binding->descriptor
                    ->unwind_regions[region_index].catch_class_id;
                if (catch_id != 0 &&
                    !st_runtime_class(descriptors, catch_id))
                    return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
            }
            while (candidate != owner) {
                const StClassDescriptor *candidate_descriptor;
                if (candidate == 0 || ++owner_hops > descriptors->class_count)
                    return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
                candidate_descriptor = st_runtime_class(descriptors,
                                                        candidate);
                if (!candidate_descriptor)
                    return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
                candidate = candidate_descriptor->superclass_id;
            }
        }
    }
    for (index = 0; index < descriptors->shape_count; index++) {
        const StShapeDescriptor *shape = descriptors->shapes[index];
        if (!st_shape_descriptor_is_valid(shape) ||
            shape->shape_id != index + 1 ||
            !st_runtime_class(descriptors, shape->class_id))
            return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
    }
    return ST_RUNTIME_OK;
}

static void *default_allocate(void *user, size_t alignment, size_t size)
{
    (void)user;
    return aligned_alloc(alignment, size);
}

static void default_deallocate(void *user, void *pointer, size_t alignment,
                               size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    free(pointer);
}

static bool normalize_allocator(st_runtime_allocator_t *allocator)
{
    if (!allocator) return false;
    if ((allocator->allocate == NULL) != (allocator->deallocate == NULL))
        return false;
    if (!allocator->allocate) {
        allocator->allocate = default_allocate;
        allocator->deallocate = default_deallocate;
    }
    return true;
}

static bool bitmap_test(const uint64_t *bitmap, size_t bit)
{
    return (bitmap[bit >> 6] & (UINT64_C(1) << (bit & 63u))) != 0;
}

st_runtime_status_t st_object_allocate(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id,
    uint32_t shape_id, size_t indexed_length, size_t indexed_capacity,
    st_header_flags_t flags, st_runtime_allocator_t allocator,
    st_object_extent_t *extent_out, st_value_t *value_out)
{
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    st_heap_object_t *object;
    size_t allocation_size;
    size_t index;
    if (extent_out) *extent_out = (st_object_extent_t){ 0 };
    if (value_out) *value_out = 0;
    if (!extent_out || !value_out || !normalize_allocator(&allocator))
        return ST_RUNTIME_ERR_INVALID_ARGUMENT;
    if ((flags & ~ST_RUNTIME_ALLOCATION_FLAGS) != 0u)
        return ST_RUNTIME_ERR_INVALID_ARGUMENT;
    class_descriptor = st_runtime_class(descriptors, class_id);
    shape = st_runtime_shape(descriptors, shape_id);
    if (!class_descriptor || !shape ||
        !st_class_descriptor_is_valid(class_descriptor) ||
        !st_shape_descriptor_is_valid(shape) || shape->class_id != class_id)
        return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
    if ((shape->indexed_format == ST_INDEXED_NONE &&
         (indexed_length != 0 || indexed_capacity != 0)) ||
        indexed_length > indexed_capacity)
        return ST_RUNTIME_ERR_BAD_EXTENT;
    if (!st_shape_descriptor_extent(shape, indexed_capacity,
                                    &allocation_size))
        return ST_RUNTIME_ERR_OVERFLOW;
    object = allocator.allocate(allocator.user, shape->allocation_alignment,
                                allocation_size);
    if (!object) return ST_RUNTIME_ERR_OUT_OF_MEMORY;
    if (((uintptr_t)object & (shape->allocation_alignment - 1)) != 0) {
        allocator.deallocate(allocator.user, object,
                             shape->allocation_alignment,
                             allocation_size);
        return ST_RUNTIME_ERR_BAD_ALIGNMENT;
    }
    memset(object, 0, allocation_size);
    if (!st_object_header_init(&object->header, class_id, shape_id, 0,
                               ST_GC_WHITE, ST_GC_NURSERY, flags)) {
        allocator.deallocate(allocator.user, object,
                             shape->allocation_alignment,
                             allocation_size);
        return ST_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    object->indexed_length = indexed_length;
    object->indexed_capacity = indexed_capacity;
    for (index = 0; index < shape->fixed_word_count; index++) {
        if (bitmap_test(shape->fixed_pointer_bitmap, index))
            ((st_value_t *)object->payload)[index] = st_value_nil();
    }
    if (shape->indexed_format == ST_INDEXED_VALUES) {
        st_value_t *elements = (st_value_t *)(object->payload +
            shape->fixed_word_count * sizeof(uint64_t));
        for (index = 0; index < indexed_length; index++)
            elements[index] = st_value_nil();
    }
    extent_out->base = object;
    extent_out->byte_size = allocation_size;
    extent_out->allocation_alignment = shape->allocation_alignment;
    if (!st_value_from_object(object, value_out)) {
        allocator.deallocate(allocator.user, object,
                             shape->allocation_alignment,
                             allocation_size);
        *extent_out = (st_object_extent_t){ 0 };
        return ST_RUNTIME_ERR_BAD_ALIGNMENT;
    }
    return ST_RUNTIME_OK;
}

void st_object_deallocate(st_runtime_allocator_t allocator,
                          st_object_extent_t extent)
{
    if (!extent.base || !normalize_allocator(&allocator)) return;
    /* The caller must retain the allocator contract used for this extent.
     * The default allocator ignores these advisory values. */
    allocator.deallocate(allocator.user, extent.base,
                         extent.allocation_alignment, extent.byte_size);
}

st_runtime_status_t st_object_validate(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, st_object_view_t *view_out)
{
    void *decoded = NULL;
    st_heap_object_t *object;
    uint64_t header_word;
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape;
    size_t fixed_size;
    size_t required_extent;
    if (view_out) memset(view_out, 0, sizeof(*view_out));
    if (!descriptors || !view_out || !extent.base ||
        extent.byte_size < offsetof(st_heap_object_t, payload) ||
        !st_value_to_object_unchecked(value, &decoded) ||
        decoded != extent.base ||
        extent.allocation_alignment < _Alignof(st_heap_object_t) ||
        !is_power_of_two(extent.allocation_alignment) ||
        ((uintptr_t)extent.base & (_Alignof(st_heap_object_t) - 1)) != 0)
        return ST_RUNTIME_ERR_BAD_EXTENT;

    object = extent.base;
    header_word = st_object_header_load(&object->header);
    if (!st_object_header_word_is_valid(header_word))
        return ST_RUNTIME_ERR_BAD_OBJECT;
    class_descriptor = st_runtime_class(
        descriptors, st_object_header_class_id(header_word));
    shape = st_runtime_shape(descriptors,
                             st_object_header_shape_id(header_word));
    if (!class_descriptor || !shape ||
        !st_class_descriptor_is_valid(class_descriptor) ||
        !st_shape_descriptor_is_valid(shape) ||
        shape->class_id != class_descriptor->class_id)
        return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
    if (((uintptr_t)extent.base & (shape->allocation_alignment - 1)) != 0)
        return ST_RUNTIME_ERR_BAD_ALIGNMENT;
    if (extent.allocation_alignment != shape->allocation_alignment)
        return ST_RUNTIME_ERR_BAD_EXTENT;
    if ((shape->indexed_format == ST_INDEXED_NONE &&
         (object->indexed_length != 0 || object->indexed_capacity != 0)) ||
        object->indexed_length > object->indexed_capacity ||
        !st_shape_descriptor_extent(shape, object->indexed_capacity,
                                    &required_extent) ||
        !multiply_size(shape->fixed_word_count, sizeof(uint64_t),
                       &fixed_size))
        return ST_RUNTIME_ERR_BAD_OBJECT;
    if (extent.byte_size != required_extent)
        return ST_RUNTIME_ERR_BAD_EXTENT;

    view_out->object = object;
    view_out->class_descriptor = class_descriptor;
    view_out->shape_descriptor = shape;
    view_out->indexed_length = object->indexed_length;
    view_out->indexed_capacity = object->indexed_capacity;
    view_out->fixed_words = object->payload;
    view_out->indexed_elements = object->payload + fixed_size;
    return ST_RUNTIME_OK;
}

st_runtime_status_t st_object_visit_references(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, st_reference_visitor_fn visitor, void *user,
    size_t *visited_out)
{
    st_object_view_t view;
    st_runtime_status_t status;
    size_t index;
    size_t visited = 0;
    if (visited_out) *visited_out = 0;
    if (!visitor || !visited_out) return ST_RUNTIME_ERR_INVALID_ARGUMENT;
    status = st_object_validate(descriptors, value, extent, &view);
    if (status != ST_RUNTIME_OK) return status;
    /* Validate every tagged slot before invoking user code. A malformed late
     * slot must not leave a collector or verifier with a partially applied
     * traversal. */
    for (index = 0; index < view.shape_descriptor->fixed_word_count; index++) {
        if (!bitmap_test(view.shape_descriptor->fixed_pointer_bitmap, index))
            continue;
        if (!st_value_has_valid_encoding(
                ((st_value_t *)view.fixed_words)[index]))
            return ST_RUNTIME_ERR_BAD_OBJECT;
    }
    if (view.shape_descriptor->indexed_format == ST_INDEXED_VALUES) {
        st_value_t *elements = view.indexed_elements;
        for (index = 0; index < view.indexed_length; index++)
            if (!st_value_has_valid_encoding(elements[index]))
                return ST_RUNTIME_ERR_BAD_OBJECT;
    }
    for (index = 0; index < view.shape_descriptor->fixed_word_count; index++) {
        st_value_t *slot;
        if (!bitmap_test(view.shape_descriptor->fixed_pointer_bitmap, index))
            continue;
        slot = &((st_value_t *)view.fixed_words)[index];
        if (st_value_kind(*slot) == ST_VALUE_OBJECT) {
            if (!visitor(user, slot)) return ST_RUNTIME_ERR_VISITOR_ABORTED;
            if (!st_value_has_valid_encoding(*slot))
                return ST_RUNTIME_ERR_BAD_OBJECT;
            visited++;
        }
    }
    if (view.shape_descriptor->indexed_format == ST_INDEXED_VALUES) {
        st_value_t *elements = view.indexed_elements;
        for (index = 0; index < view.indexed_length; index++) {
            if (st_value_kind(elements[index]) == ST_VALUE_OBJECT) {
                if (!visitor(user, &elements[index]))
                    return ST_RUNTIME_ERR_VISITOR_ABORTED;
                if (!st_value_has_valid_encoding(elements[index]))
                    return ST_RUNTIME_ERR_BAD_OBJECT;
                visited++;
            }
        }
    }
    *visited_out = visited;
    return ST_RUNTIME_OK;
}

static bool compatible_shape(const StShapeDescriptor *left,
                             const StShapeDescriptor *right)
{
    return left->class_id == right->class_id &&
           left->allocation_alignment == right->allocation_alignment &&
           left->minimum_allocation_size == right->minimum_allocation_size &&
           left->fixed_word_count == right->fixed_word_count &&
           left->indexed_format == right->indexed_format &&
           left->fixed_pointer_bitmap_word_count ==
               right->fixed_pointer_bitmap_word_count &&
           (left->fixed_pointer_bitmap_word_count == 0 ||
            memcmp(left->fixed_pointer_bitmap, right->fixed_pointer_bitmap,
                   left->fixed_pointer_bitmap_word_count * sizeof(uint64_t)) ==
                0);
}

st_runtime_status_t st_object_transition_shape(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, uint32_t expected_shape_id,
    uint32_t target_shape_id)
{
    st_object_view_t view;
    const StShapeDescriptor *target;
    st_runtime_status_t status;
    uint64_t expected;
    uint64_t replacement;
    size_t target_extent;
    status = st_object_validate(descriptors, value, extent, &view);
    if (status != ST_RUNTIME_OK) return status;
    if (view.shape_descriptor->shape_id != expected_shape_id)
        return ST_RUNTIME_ERR_CONFLICT;
    target = st_runtime_shape(descriptors, target_shape_id);
    if (!target || !st_shape_descriptor_is_valid(target))
        return ST_RUNTIME_ERR_INVALID_DESCRIPTOR;
    if (!compatible_shape(view.shape_descriptor, target))
        return ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE;
    if (!st_shape_descriptor_extent(target, view.indexed_capacity,
                                    &target_extent) ||
        target_extent != extent.byte_size)
        return ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE;
    expected = st_object_header_load(&view.object->header);
    for (;;) {
        if (!st_object_header_word_is_valid(expected) ||
            st_object_header_shape_id(expected) != expected_shape_id ||
            st_object_header_class_id(expected) != target->class_id)
            return ST_RUNTIME_ERR_CONFLICT;
        if ((st_object_header_flags(expected) & ST_HEADER_IMMUTABLE) != 0)
            return ST_RUNTIME_ERR_IMMUTABLE;
        replacement = (expected & ~ST_HEADER_SHAPE_MASK) |
                      ((uint64_t)target_shape_id << ST_HEADER_SHAPE_SHIFT);
        if (atomic_compare_exchange_weak_explicit(
                &view.object->header.bits, &expected, replacement,
                memory_order_acq_rel, memory_order_acquire))
            return ST_RUNTIME_OK;
    }
}

const char *st_runtime_status_string(st_runtime_status_t status)
{
    switch (status) {
    case ST_RUNTIME_OK: return "ok";
    case ST_RUNTIME_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_RUNTIME_ERR_INVALID_DESCRIPTOR: return "invalid descriptor";
    case ST_RUNTIME_ERR_ID_OUT_OF_RANGE: return "id out of range";
    case ST_RUNTIME_ERR_OVERFLOW: return "size overflow";
    case ST_RUNTIME_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_RUNTIME_ERR_BAD_ALIGNMENT: return "bad alignment";
    case ST_RUNTIME_ERR_BAD_EXTENT: return "bad allocation extent";
    case ST_RUNTIME_ERR_BAD_OBJECT: return "malformed object";
    case ST_RUNTIME_ERR_IMMUTABLE: return "immutable object";
    case ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE: return "incompatible shape";
    case ST_RUNTIME_ERR_CONFLICT: return "concurrent state conflict";
    case ST_RUNTIME_ERR_VISITOR_ABORTED: return "reference visitor aborted";
    default: return "unknown runtime status";
    }
}
