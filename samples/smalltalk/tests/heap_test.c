#include "st_heap.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live_blocks;
    size_t live_bytes;
} allocation_tracker_t;

typedef struct {
    void *base;
} misaligned_allocator_t;

static void *misaligned_allocate(void *user, size_t alignment, size_t size)
{
    misaligned_allocator_t *allocator = user;
    allocator->base = malloc(size + alignment);
    return allocator->base
        ? (unsigned char *)allocator->base + 1u : NULL;
}

static void misaligned_deallocate(void *user, void *pointer,
                                  size_t alignment, size_t size)
{
    misaligned_allocator_t *allocator = user;
    (void)pointer;
    (void)alignment;
    (void)size;
    free(allocator->base);
    allocator->base = NULL;
}

static void *tracked_allocate(void *user, size_t alignment, size_t size)
{
    allocation_tracker_t *tracker = user;
    void *result;
    if (tracker->calls++ == tracker->fail_at) return NULL;
    result = aligned_alloc(alignment, size);
    if (result) {
        ++tracker->live_blocks;
        tracker->live_bytes += size;
    }
    return result;
}

static void tracked_deallocate(void *user, void *pointer, size_t alignment,
                               size_t size)
{
    allocation_tracker_t *tracker = user;
    (void)alignment;
    CHECK(pointer != NULL);
    CHECK(tracker->live_blocks != 0u);
    CHECK(tracker->live_bytes >= size);
    if (tracker->live_blocks != 0u) --tracker->live_blocks;
    if (tracker->live_bytes >= size) tracker->live_bytes -= size;
    free(pointer);
}

static st_runtime_allocator_t tracker_allocator(allocation_tracker_t *tracker)
{
    return (st_runtime_allocator_t){
        tracked_allocate, tracked_deallocate, tracker
    };
}

typedef struct {
    uint64_t root_bitmap;
    st_root_map_t root_map;
    StMethodDescriptor method;
    StClassDescriptor object_class;
    StClassDescriptor metaclass;
    uint64_t pointer_bitmap;
    StShapeDescriptor object_shape;
    StShapeDescriptor metaclass_shape;
    const StClassDescriptor *classes[2];
    const StShapeDescriptor *shapes[2];
    st_runtime_descriptors_t descriptors;
} fixture_t;

static void fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->root_bitmap = UINT64_C(1);
    fixture->root_map = (st_root_map_t){
        .safepoint_id = 7u,
        .root_count = 2u,
        .bitmap_word_count = 1u,
        .live_root_bitmap = &fixture->root_bitmap
    };
    fixture->method = (StMethodDescriptor){
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = 1u,
        .arity = 0u,
        .frame_root_capacity = 2u,
        .code_size = 1u,
        .root_maps = &fixture->root_map,
        .root_map_count = 1u
    };
    fixture->object_class = (StClassDescriptor){
        .class_id = 1u,
        .metaclass_id = 2u,
        .default_shape_id = 1u,
        .name = "Object",
        .name_length = sizeof("Object") - 1u
    };
    fixture->metaclass = (StClassDescriptor){
        .class_id = 2u,
        .metaclass_id = 2u,
        .default_shape_id = 2u,
        .flags = ST_CLASS_METACLASS,
        .name = "Object class",
        .name_length = sizeof("Object class") - 1u
    };
    fixture->pointer_bitmap = UINT64_C(5);
    fixture->object_shape = (StShapeDescriptor){
        .shape_id = 1u,
        .class_id = 1u,
        .allocation_alignment = 16u,
        .minimum_allocation_size = 48u,
        .fixed_word_count = 3u,
        .indexed_format = ST_INDEXED_VALUES,
        .fixed_pointer_bitmap = &fixture->pointer_bitmap,
        .fixed_pointer_bitmap_word_count = 1u
    };
    fixture->metaclass_shape = (StShapeDescriptor){
        .shape_id = 2u,
        .class_id = 2u,
        .allocation_alignment = 8u,
        .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_NONE
    };
    fixture->classes[0] = &fixture->object_class;
    fixture->classes[1] = &fixture->metaclass;
    fixture->shapes[0] = &fixture->object_shape;
    fixture->shapes[1] = &fixture->metaclass_shape;
    fixture->descriptors = (st_runtime_descriptors_t){
        .classes = fixture->classes,
        .class_count = 2u,
        .shapes = fixture->shapes,
        .shape_count = 2u
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors) ==
          ST_RUNTIME_OK);
    CHECK(st_method_descriptor_is_valid(&fixture->method));
}

static st_value_t allocate_object(st_heap_t *heap, size_t indexed_length,
                                  size_t indexed_capacity)
{
    st_value_t value = 0;
    CHECK(st_heap_allocate(heap, 1u, 1u, indexed_length, indexed_capacity,
                           0u, &value) == ST_HEAP_OK);
    CHECK(st_value_kind(value) == ST_VALUE_OBJECT);
    return value;
}

static st_object_view_t object_view(st_heap_t *heap, st_value_t value)
{
    st_object_view_t view;
    CHECK(st_heap_object_view(heap, value, &view) == ST_HEAP_OK);
    return view;
}

static void set_fixed(st_heap_t *heap, st_value_t object, size_t slot,
                      st_value_t value)
{
    st_object_view_t view = object_view(heap, object);
    CHECK(slot < view.shape_descriptor->fixed_word_count);
    ((st_value_t *)view.fixed_words)[slot] = value;
}

static void set_indexed(st_heap_t *heap, st_value_t object, size_t index,
                        st_value_t value)
{
    st_object_view_t view = object_view(heap, object);
    CHECK(view.shape_descriptor->indexed_format == ST_INDEXED_VALUES);
    CHECK(index < view.indexed_length);
    ((st_value_t *)view.indexed_elements)[index] = value;
}

static StFrame root_frame(fixture_t *fixture, st_value_t *roots)
{
    return (StFrame){
        .thread = fixture,
        .method = &fixture->method,
        .receiver = st_value_nil(),
        .roots = roots,
        .root_count = 2u,
        .safepoint_id = 7u
    };
}

static void test_allocation_and_authorization(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t object;
    st_object_extent_t extent;
    st_object_view_t view;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    object = allocate_object(&heap, 2u, 4u);
    CHECK(st_heap_object_count(&heap) == 1u);
    CHECK(st_heap_allocated_bytes(&heap) == 80u);
    CHECK(st_heap_contains(&heap, object));
    CHECK(st_heap_authorize(&heap, object, &extent) == ST_HEAP_OK);
    CHECK(extent.base == (void *)(uintptr_t)object && extent.byte_size == 80u);
    CHECK(st_heap_object_view(&heap, object, &view) == ST_HEAP_OK);
    CHECK(view.indexed_length == 2u && view.indexed_capacity == 4u);
    CHECK(((st_value_t *)view.fixed_words)[0] == st_value_nil());
    CHECK(((st_value_t *)view.fixed_words)[2] == st_value_nil());
    CHECK(((st_value_t *)view.indexed_elements)[0] == st_value_nil());
    CHECK(!st_heap_contains(&heap, object + 8u));
    CHECK(st_heap_authorize(&heap, object + 8u, &extent) ==
          ST_HEAP_ERR_NOT_MEMBER);
    CHECK(st_heap_authorize(&heap, st_value_nil(), &extent) ==
          ST_HEAP_ERR_NOT_OBJECT);
    CHECK(st_heap_authorize(&heap, 0u, &extent) == ST_HEAP_ERR_BAD_OBJECT);
    CHECK(st_heap_allocate(&heap, 1u, 1u, 2u, 1u, 0u, &object) ==
          ST_HEAP_ERR_BAD_EXTENT);
    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, SIZE_MAX, 0u, &object) ==
          ST_HEAP_ERR_OVERFLOW);
    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, 0u, ST_HEADER_WEAK,
                           &object) == ST_HEAP_ERR_INVALID_ARGUMENT);
    CHECK(st_heap_object_count(&heap) == 1u);
    st_heap_destroy(&heap);
    CHECK(heap.state == NULL);
}

static void test_graph_collection_and_remembered_clear(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t a;
    st_value_t b;
    st_value_t c;
    st_value_t garbage;
    st_value_t roots[2];
    StFrame frame;
    st_heap_collection_stats_t stats;
    st_object_view_t view;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    a = allocate_object(&heap, 1u, 1u);
    b = allocate_object(&heap, 0u, 0u);
    c = allocate_object(&heap, 0u, 0u);
    garbage = allocate_object(&heap, 0u, 0u);
    set_fixed(&heap, a, 0u, b);
    set_indexed(&heap, a, 0u, c);
    set_fixed(&heap, b, 0u, a);
    view = object_view(&heap, b);
    CHECK(st_object_header_remember(&view.object->header));
    CHECK(st_object_header_try_mark_gray(&view.object->header));
    CHECK(st_object_header_try_mark_black(&view.object->header));
    CHECK((st_object_header_flags(st_object_header_load(&view.object->header)) &
           ST_HEADER_REMEMBERED) != 0u);
    roots[0] = a;
    roots[1] = 0u; /* Not live in the root map and intentionally invalid. */
    frame = root_frame(&fixture, roots);
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(stats.objects_before == 4u && stats.marked_objects == 3u);
    CHECK(stats.reclaimed_objects == 1u);
    CHECK(st_heap_object_count(&heap) == 3u);
    CHECK(st_heap_contains(&heap, a) && st_heap_contains(&heap, b) &&
          st_heap_contains(&heap, c) && !st_heap_contains(&heap, garbage));
    view = object_view(&heap, b);
    CHECK((st_object_header_flags(st_object_header_load(&view.object->header)) &
           ST_HEADER_REMEMBERED) == 0u);
    CHECK(st_object_header_color(st_object_header_load(&view.object->header)) ==
          ST_GC_WHITE);
    roots[0] = st_value_nil();
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(stats.reclaimed_objects == 3u && stats.bytes_after == 0u);
    CHECK(st_heap_object_count(&heap) == 0u);
    CHECK(st_heap_collection_count(&heap) == 2u);
    st_heap_destroy(&heap);
}

static void test_fixed_reference_boundary(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t owner;
    st_value_t child;
    st_value_t young;
    st_value_t immutable = 0u;
    st_value_t loaded = UINT64_MAX;
    void *foreign = NULL;
    st_value_t foreign_value = 0u;
    st_object_view_t owner_view;
    st_gc_generation_t generation;
    uint8_t age;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    owner = allocate_object(&heap, 0u, 0u);
    child = allocate_object(&heap, 0u, 0u);
    CHECK(st_heap_fixed_reference_load(&heap, owner, 0u, &loaded)
          == ST_HEAP_OK && loaded == st_value_nil());
    CHECK(st_heap_fixed_reference_store(&heap, owner, 0u, child)
          == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_load(&heap, owner, 0u, &loaded)
          == ST_HEAP_OK && loaded == child);
    CHECK(st_heap_fixed_reference_load(&heap, owner, 1u, &loaded)
          == ST_HEAP_ERR_BAD_SLOT && loaded == (st_value_t)ST_VALUE_INVALID);
    CHECK(st_heap_fixed_reference_store(&heap, owner, 3u, child)
          == ST_HEAP_ERR_BAD_SLOT);

    owner_view = object_view(&heap, owner);
    CHECK(st_object_header_survive(
              &owner_view.object->header, 1u, &generation, &age));
    CHECK(st_object_header_survive(
              &owner_view.object->header, 1u, &generation, &age)
          && generation == ST_GC_OLD);
    young = allocate_object(&heap, 0u, 0u);
    CHECK(st_heap_fixed_reference_store(&heap, owner, 0u, young)
          == ST_HEAP_OK);
    CHECK((st_object_header_flags(st_object_header_load(
              &owner_view.object->header)) & ST_HEADER_REMEMBERED) != 0u);

    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, 0u,
                           ST_HEADER_IMMUTABLE, &immutable) == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_store(&heap, immutable, 0u, child)
          == ST_HEAP_ERR_IMMUTABLE);
    foreign = aligned_alloc(16u, 48u);
    CHECK(foreign != NULL);
    if (foreign) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        CHECK(st_heap_fixed_reference_store(
                  &heap, owner, 0u, foreign_value)
              == ST_HEAP_ERR_DANGLING_REFERENCE);
        CHECK(st_heap_fixed_reference_load(
                  &heap, foreign_value, 0u, &loaded)
              == ST_HEAP_ERR_NOT_MEMBER);
        free(foreign);
    }
    st_heap_destroy(&heap);
}

static void test_indexed_reference_boundary(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t owner;
    st_value_t child;
    st_value_t young;
    st_value_t immutable;
    st_value_t loaded = UINT64_MAX;
    st_object_view_t owner_view;
    st_gc_generation_t generation;
    uint8_t age;

    fixture_init(&fixture);
    CHECK(st_heap_init(
              &heap, &fixture.descriptors, (st_runtime_allocator_t) {0})
          == ST_HEAP_OK);
    owner = allocate_object(&heap, 2u, 3u);
    child = allocate_object(&heap, 0u, 0u);

    CHECK(st_heap_indexed_reference_load(&heap, owner, 0u, &loaded)
          == ST_HEAP_OK);
    CHECK(loaded == st_value_nil());
    CHECK(st_heap_indexed_reference_store(&heap, owner, 1u, child)
          == ST_HEAP_OK);
    CHECK(st_heap_indexed_reference_load(&heap, owner, 1u, &loaded)
          == ST_HEAP_OK);
    CHECK(loaded == child);
    CHECK(st_heap_indexed_reference_load(&heap, owner, 2u, &loaded)
          == ST_HEAP_ERR_BAD_SLOT);
    CHECK(loaded == (st_value_t)ST_VALUE_INVALID);
    CHECK(st_heap_indexed_reference_store(&heap, owner, 2u, child)
          == ST_HEAP_ERR_BAD_SLOT);

    owner_view = object_view(&heap, owner);
    CHECK(st_object_header_survive(
        &owner_view.object->header, 1u, &generation, &age));
    CHECK(st_object_header_survive(
              &owner_view.object->header, 1u, &generation, &age)
          && generation == ST_GC_OLD);
    young = allocate_object(&heap, 0u, 0u);
    CHECK(st_heap_indexed_reference_store(&heap, owner, 0u, young)
          == ST_HEAP_OK);
    CHECK((st_object_header_flags(st_object_header_load(
              &owner_view.object->header)) & ST_HEADER_REMEMBERED) != 0u);

    CHECK(st_heap_allocate(
              &heap, 1u, 1u, 1u, 1u, ST_HEADER_IMMUTABLE, &immutable)
          == ST_HEAP_OK);
    CHECK(st_heap_indexed_reference_store(&heap, immutable, 0u, child)
          == ST_HEAP_ERR_IMMUTABLE);
    CHECK(st_heap_indexed_reference_load(
              &heap, st_value_nil(), 0u, &loaded)
          == ST_HEAP_ERR_NOT_OBJECT);

    st_heap_destroy(&heap);
}

static void test_receiver_argv_roots_and_caller(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t receiver;
    st_value_t argument;
    st_value_t shadow;
    st_value_t caller_root;
    st_value_t garbage;
    st_value_t argument_vector[1];
    st_value_t top_roots[2];
    st_value_t caller_roots[2];
    StMethodDescriptor argument_method;
    StMethodDescriptor smaller_map_method;
    st_root_map_t smaller_map;
    StFrame caller;
    StFrame top;
    st_heap_collection_stats_t stats;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    receiver = allocate_object(&heap, 0u, 0u);
    argument = allocate_object(&heap, 0u, 0u);
    shadow = allocate_object(&heap, 0u, 0u);
    caller_root = allocate_object(&heap, 0u, 0u);
    garbage = allocate_object(&heap, 0u, 0u);
    argument_method = fixture.method;
    argument_method.arity = 1u;
    CHECK(st_method_descriptor_is_valid(&argument_method));
    caller_roots[0] = caller_root;
    caller_roots[1] = 0u;
    caller = root_frame(&fixture, caller_roots);
    argument_vector[0] = argument;
    top_roots[0] = shadow;
    top_roots[1] = 0u;
    top = root_frame(&fixture, top_roots);
    top.method = &argument_method;
    top.receiver = receiver;
    top.argv = argument_vector;
    top.argc = 1u;
    top.caller = &caller;
    CHECK(st_heap_collect(&heap, &top, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(stats.marked_objects == 4u && stats.reclaimed_objects == 1u);
    CHECK(st_heap_contains(&heap, receiver));
    CHECK(st_heap_contains(&heap, argument));
    CHECK(st_heap_contains(&heap, shadow));
    CHECK(st_heap_contains(&heap, caller_root));
    CHECK(!st_heap_contains(&heap, garbage));
    smaller_map = fixture.root_map;
    smaller_map.root_count = 1u;
    smaller_map_method = argument_method;
    smaller_map_method.root_maps = &smaller_map;
    CHECK(st_method_descriptor_is_valid(&smaller_map_method));
    top.method = &smaller_map_method;
    /* The physical vector remains at frame_root_capacity=2 while this
     * safepoint describes only its first logical slot. */
    CHECK(top.root_count == smaller_map_method.frame_root_capacity);
    CHECK(st_heap_collect(&heap, &top, NULL, 0u, &stats) == ST_HEAP_OK);
    st_heap_destroy(&heap);
}

static void test_failed_collections_are_nondestructive(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t a;
    st_value_t b;
    st_value_t roots[2];
    StFrame frame;
    StMethodDescriptor unwind_method;
    st_heap_collection_stats_t stats;
    void *foreign;
    st_value_t foreign_value;
    size_t bytes;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    a = allocate_object(&heap, 0u, 0u);
    b = allocate_object(&heap, 0u, 0u);
    bytes = st_heap_allocated_bytes(&heap);
    roots[0] = a + 8u;
    roots[1] = 0u;
    frame = root_frame(&fixture, roots);
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_DANGLING_REFERENCE);
    CHECK(st_heap_object_count(&heap) == 2u &&
          st_heap_allocated_bytes(&heap) == bytes &&
          st_heap_collection_count(&heap) == 0u);
    foreign = aligned_alloc(16u, 32u);
    CHECK(foreign != NULL);
    foreign_value = 0u;
    if (foreign) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        roots[0] = foreign_value;
        CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
              ST_HEAP_ERR_DANGLING_REFERENCE);
        CHECK(st_heap_object_count(&heap) == 2u);
        free(foreign);
    }
    roots[0] = 0u;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_ROOT);
    CHECK(st_heap_object_count(&heap) == 2u);
    roots[0] = a;
    frame.safepoint_id = 99u;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_FRAME);
    frame.safepoint_id = 7u;
    frame.thread = NULL;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_FRAME);
    frame.thread = &fixture;
    frame.flags = 1u;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_FRAME);
    frame.flags = 0u;
    frame.home = (StHomeToken *)(uintptr_t)8u;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_FRAME);
    frame.home = NULL;
    unwind_method = fixture.method;
    unwind_method.flags = ST_METHOD_CAN_UNWIND;
    CHECK(st_method_descriptor_is_valid(&unwind_method));
    frame.method = &unwind_method;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_INVALID_FRAME);
    frame.method = &fixture.method;
    frame.caller = &frame;
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_FRAME_CYCLE);
    frame.caller = NULL;
    set_fixed(&heap, a, 0u, b + 8u);
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_DANGLING_REFERENCE);
    CHECK(st_heap_contains(&heap, a) && st_heap_contains(&heap, b));
    set_fixed(&heap, a, 0u, 0u);
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) ==
          ST_HEAP_ERR_BAD_OBJECT);
    CHECK(st_heap_object_count(&heap) == 2u &&
          st_heap_collection_count(&heap) == 0u);
    set_fixed(&heap, a, 0u, st_value_nil());
    CHECK(st_heap_collect(&heap, &frame, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_object_count(&heap) == 1u && st_heap_contains(&heap, a));
    st_heap_destroy(&heap);
}

static void test_fault_injection(void)
{
    fixture_t fixture;
    allocation_tracker_t tracker = { .fail_at = SIZE_MAX };
    st_runtime_allocator_t allocator = tracker_allocator(&tracker);
    st_heap_t heap = {0};
    st_value_t value = UINT64_MAX;
    st_heap_collection_stats_t stats;
    size_t live_after_init;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors, allocator) == ST_HEAP_OK);
    live_after_init = tracker.live_blocks;

    tracker.fail_at = tracker.calls;
    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, 0u, 0u, &value) ==
          ST_HEAP_ERR_OUT_OF_MEMORY);
    CHECK(value == 0u && st_heap_object_count(&heap) == 0u &&
          tracker.live_blocks == live_after_init);

    tracker.fail_at = tracker.calls + 1u;
    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, 0u, 0u, &value) ==
          ST_HEAP_ERR_OUT_OF_MEMORY);
    CHECK(value == 0u && st_heap_object_count(&heap) == 0u &&
          tracker.live_blocks == live_after_init);

    tracker.fail_at = tracker.calls + 2u;
    CHECK(st_heap_allocate(&heap, 1u, 1u, 0u, 0u, 0u, &value) ==
          ST_HEAP_ERR_OUT_OF_MEMORY);
    CHECK(value == 0u && st_heap_object_count(&heap) == 0u);

    tracker.fail_at = SIZE_MAX;
    value = allocate_object(&heap, 0u, 0u);
    CHECK(st_heap_object_count(&heap) == 1u);
    tracker.fail_at = tracker.calls;
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats) ==
          ST_HEAP_ERR_OUT_OF_MEMORY);
    CHECK(st_heap_object_count(&heap) == 1u && st_heap_contains(&heap, value));
    CHECK(st_heap_collection_count(&heap) == 0u);
    tracker.fail_at = SIZE_MAX;
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_object_count(&heap) == 0u);
    st_heap_destroy(&heap);
    CHECK(tracker.live_blocks == 0u && tracker.live_bytes == 0u);
}

static void test_global_roots_and_stress_cycle(void)
{
    enum { OBJECT_COUNT = 12000 };
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t *values = malloc(sizeof(*values) * OBJECT_COUNT);
    st_value_t root;
    st_heap_collection_stats_t stats;
    size_t index;
    fixture_init(&fixture);
    CHECK(values != NULL);
    if (!values) return;
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    for (index = 0u; index < OBJECT_COUNT; ++index) {
        values[index] = allocate_object(&heap, 0u, 0u);
        if (index != 0u) set_fixed(&heap, values[index], 0u,
                                   values[index - 1u]);
    }
    set_fixed(&heap, values[0], 0u, values[OBJECT_COUNT - 1u]);
    root = values[OBJECT_COUNT - 1u];
    CHECK(st_heap_collect(&heap, NULL, &root, 1u, &stats) == ST_HEAP_OK);
    CHECK(stats.marked_objects == OBJECT_COUNT &&
          stats.reclaimed_objects == 0u);
    for (index = 0u; index < OBJECT_COUNT; ++index)
        CHECK(st_heap_contains(&heap, values[index]));
    root = st_value_nil();
    CHECK(st_heap_collect(&heap, NULL, &root, 1u, &stats) == ST_HEAP_OK);
    CHECK(stats.reclaimed_objects == OBJECT_COUNT &&
          st_heap_object_count(&heap) == 0u);
    st_heap_destroy(&heap);
    free(values);
}

static void test_lifecycle_and_statuses(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    StShapeDescriptor bad_shape;
    misaligned_allocator_t misaligned = {0};
    fixture_init(&fixture);
    CHECK(st_heap_init(NULL, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) ==
          ST_HEAP_ERR_INVALID_ARGUMENT);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
          (st_runtime_allocator_t){ tracked_allocate, NULL, NULL }) ==
          ST_HEAP_ERR_INVALID_ARGUMENT);
    bad_shape = fixture.object_shape;
    bad_shape.minimum_allocation_size = 1u;
    fixture.shapes[0] = &bad_shape;
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) ==
          ST_HEAP_ERR_INVALID_DESCRIPTOR);
    fixture.shapes[0] = &fixture.object_shape;
    CHECK(st_heap_init(&heap, &fixture.descriptors,
          (st_runtime_allocator_t){ misaligned_allocate,
                                    misaligned_deallocate, &misaligned }) ==
          ST_HEAP_ERR_BAD_ALIGNMENT);
    CHECK(heap.state == NULL && misaligned.base == NULL);
    CHECK(strcmp(st_heap_status_string(ST_HEAP_ERR_FRAME_CYCLE),
                 "cyclic frame chain") == 0);
    st_heap_destroy(&heap);
}

int main(void)
{
    test_allocation_and_authorization();
    test_graph_collection_and_remembered_clear();
    test_fixed_reference_boundary();
    test_indexed_reference_boundary();
    test_receiver_argv_roots_and_caller();
    test_failed_collections_are_nondestructive();
    test_fault_injection();
    test_global_roots_and_stress_cycle();
    test_lifecycle_and_statuses();
    if (failures != 0u) {
        fprintf(stderr, "heap regression: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("heap regression: PASS");
    return EXIT_SUCCESS;
}
