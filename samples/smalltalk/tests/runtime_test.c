#include "st_runtime.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
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

static st_value_t return_true(StFrame *frame)
{
    (void)frame;
    return st_value_true();
}

static st_value_t return_false(StFrame *frame)
{
    (void)frame;
    return st_value_false();
}

typedef struct {
    uint64_t root_bitmap;
    st_root_map_t root_map;
    StMethodDescriptor method;
    StMethodBinding binding;
    StMethodEntry entry;
    st_method_slot_t method_slot;
    StClassDescriptor object_class;
    StClassDescriptor metaclass;
    uint64_t object_pointer_bitmap;
    StShapeDescriptor object_shape;
    StShapeDescriptor metaclass_shape;
    StShapeDescriptor alternate_shape;
    StShapeDescriptor incompatible_shape;
    const StClassDescriptor *classes[2];
    const StShapeDescriptor *shapes[4];
    st_runtime_descriptors_t descriptors;
} fixture_t;

static void fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->root_bitmap = UINT64_C(5);
    fixture->root_map = (st_root_map_t) {
        .safepoint_id = 1,
        .root_count = 3,
        .bitmap_word_count = 1,
        .live_root_bitmap = &fixture->root_bitmap
    };
    fixture->method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1,
        .owner_class_id = 1,
        .arity = 0,
        .frame_root_capacity = 3,
        .code_size = 16,
        .source_name = "Object.st",
        .source_name_length = sizeof("Object.st") - 1,
        .root_maps = &fixture->root_map,
        .root_map_count = 1
    };
    fixture->binding = (StMethodBinding) {
        .descriptor = &fixture->method,
        .code = return_true,
        .version = 1
    };
    CHECK(st_method_entry_init(&fixture->entry, &fixture->binding));
    fixture->method_slot = (st_method_slot_t) {
        .selector_id = 1,
        .entry = &fixture->entry
    };
    fixture->object_class = (StClassDescriptor) {
        .class_id = 1,
        .metaclass_id = 2,
        .default_shape_id = 1,
        .name = "Object",
        .name_length = sizeof("Object") - 1,
        .method_slots = &fixture->method_slot,
        .method_slot_count = 1
    };
    fixture->metaclass = (StClassDescriptor) {
        .class_id = 2,
        .metaclass_id = 2,
        .default_shape_id = 2,
        .flags = ST_CLASS_METACLASS,
        .name = "Object class",
        .name_length = sizeof("Object class") - 1
    };
    fixture->object_pointer_bitmap = UINT64_C(5);
    fixture->object_shape = (StShapeDescriptor) {
        .shape_id = 1,
        .class_id = 1,
        .allocation_alignment = 16,
        .minimum_allocation_size = 48,
        .fixed_word_count = 3,
        .indexed_format = ST_INDEXED_VALUES,
        .fixed_pointer_bitmap = &fixture->object_pointer_bitmap,
        .fixed_pointer_bitmap_word_count = 1
    };
    fixture->metaclass_shape = (StShapeDescriptor) {
        .shape_id = 2,
        .class_id = 2,
        .allocation_alignment = 8,
        .minimum_allocation_size = 24,
        .indexed_format = ST_INDEXED_NONE
    };
    fixture->alternate_shape = fixture->object_shape;
    fixture->alternate_shape.shape_id = 3;
    fixture->incompatible_shape = fixture->object_shape;
    fixture->incompatible_shape.shape_id = 4;
    fixture->incompatible_shape.indexed_format = ST_INDEXED_UINT8;
    fixture->classes[0] = &fixture->object_class;
    fixture->classes[1] = &fixture->metaclass;
    fixture->shapes[0] = &fixture->object_shape;
    fixture->shapes[1] = &fixture->metaclass_shape;
    fixture->shapes[2] = &fixture->alternate_shape;
    fixture->shapes[3] = &fixture->incompatible_shape;
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = 2,
        .shapes = fixture->shapes,
        .shape_count = 4
    };
}

static void test_descriptor_validation(void)
{
    fixture_t fixture;
    size_t extent = 0;
    fixture_init(&fixture);
    CHECK(sizeof(StMethodDescriptor) == 96u);
    CHECK(st_method_descriptor_is_valid(&fixture.method));
    CHECK(st_method_binding_is_valid(&fixture.binding));
    CHECK(st_class_descriptor_is_valid(&fixture.object_class));
    CHECK(st_shape_descriptor_is_valid(&fixture.object_shape));
    CHECK(st_shape_descriptor_extent(&fixture.object_shape, 4, &extent));
    CHECK(extent == 80);
    CHECK(st_runtime_descriptors_validate(&fixture.descriptors) ==
          ST_RUNTIME_OK);
    CHECK(st_runtime_class(&fixture.descriptors, 1) == &fixture.object_class);
    CHECK(st_runtime_class(&fixture.descriptors, 0) == NULL);
    CHECK(st_runtime_shape(&fixture.descriptors, 4) ==
          &fixture.incompatible_shape);
    CHECK(st_runtime_shape(&fixture.descriptors, 5) == NULL);

    fixture.metaclass.superclass_id = fixture.object_class.class_id;
    CHECK(st_runtime_descriptors_validate(&fixture.descriptors) ==
          ST_RUNTIME_OK);
    fixture.metaclass.superclass_id = 0u;

    StShapeDescriptor malformed = fixture.object_shape;
    malformed.allocation_alignment = 3;
    CHECK(!st_shape_descriptor_is_valid(&malformed));
    malformed = fixture.object_shape;
    malformed.minimum_allocation_size--;
    CHECK(!st_shape_descriptor_is_valid(&malformed));
    malformed = fixture.object_shape;
    uint64_t high_bitmap = UINT64_C(1) << 63;
    malformed.fixed_pointer_bitmap = &high_bitmap;
    CHECK(!st_shape_descriptor_is_valid(&malformed));
    malformed = fixture.object_shape;
    malformed.fixed_word_count = SIZE_MAX / sizeof(uint64_t) + 1;
    CHECK(!st_shape_descriptor_extent(&malformed, 0, &extent));
    malformed = fixture.object_shape;
    malformed.fixed_word_count = 0;
    malformed.fixed_pointer_bitmap = NULL;
    malformed.fixed_pointer_bitmap_word_count = 0;
    malformed.indexed_format = ST_INDEXED_UINT64;
    CHECK(!st_shape_descriptor_extent(
        &malformed, SIZE_MAX / sizeof(uint64_t) + 1, &extent));

    StMethodDescriptor bad_method = fixture.method;
    uint64_t bad_roots = UINT64_C(1) << 63;
    st_root_map_t bad_map = fixture.root_map;
    bad_map.live_root_bitmap = &bad_roots;
    bad_method.root_maps = &bad_map;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    bad_method = fixture.method;
    bad_method.source_name = NULL;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    bad_method = fixture.method;
    bad_method.flags = UINT32_C(0x80000000);
    CHECK(!st_method_descriptor_is_valid(&bad_method));

    /* ABI v2 describes methods before native code has been linked. */
    StMethodDescriptor prelink = fixture.method;
    StMethodBinding prelink_binding = fixture.binding;
    prelink.code_size = 0u;
    prelink_binding.descriptor = &prelink;
    CHECK(st_method_descriptor_is_valid(&prelink));
    CHECK(st_method_binding_is_valid(&prelink_binding));
    prelink.abi_version = UINT32_C(1);
    CHECK(!st_method_descriptor_is_valid(&prelink));
    prelink.abi_version = ST_METHOD_ABI_VERSION;
    prelink.flags = ST_METHOD_CAN_UNWIND |
                    ST_METHOD_HAS_NON_LOCAL_RETURN;
    CHECK(st_method_descriptor_is_valid(&prelink));

    st_unwind_region_t unwind_regions[2] = {
        { .start_pc_offset = 0, .end_pc_offset = 12,
          .landing_pad_pc_offset = 12, .kind = ST_UNWIND_ENSURE },
        { .start_pc_offset = 2, .end_pc_offset = 8,
          .landing_pad_pc_offset = 13, .kind = ST_UNWIND_CATCH,
          .catch_class_id = 1 }
    };
    bad_method = fixture.method;
    bad_method.flags = ST_METHOD_CAN_UNWIND;
    bad_method.unwind_regions = unwind_regions;
    bad_method.unwind_region_count = 2;
    CHECK(st_method_descriptor_is_valid(&bad_method));
    StMethodBinding unwind_binding = {
        .descriptor = &bad_method,
        .version = 1u
    };
    CHECK(!st_method_binding_is_valid(&unwind_binding));
    unwind_binding.code = return_true;
    CHECK(st_method_binding_is_valid(&unwind_binding));
    bad_method.code_size = 0u;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    bad_method.code_size = fixture.method.code_size;
    unwind_regions[1].end_pc_offset = 14;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    unwind_regions[1].end_pc_offset = 8;
    unwind_regions[1].kind = ST_UNWIND_NON_LOCAL_RETURN;
    unwind_regions[1].catch_class_id = 0;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    bad_method.flags |= ST_METHOD_HAS_NON_LOCAL_RETURN;
    CHECK(st_method_descriptor_is_valid(&bad_method));

    /* Endpoint comparisons remain exact at the uint32_t boundary. */
    st_unwind_region_t boundary_region = {
        .start_pc_offset = UINT32_MAX - 1u,
        .end_pc_offset = UINT32_MAX,
        .landing_pad_pc_offset = UINT32_MAX - 1u,
        .kind = ST_UNWIND_ENSURE
    };
    bad_method = fixture.method;
    bad_method.flags = ST_METHOD_CAN_UNWIND;
    bad_method.code_size = UINT32_MAX;
    bad_method.unwind_regions = &boundary_region;
    bad_method.unwind_region_count = 1u;
    CHECK(st_method_descriptor_is_valid(&bad_method));
    bad_method.code_size = UINT32_MAX - 1u;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    bad_method.code_size = UINT32_MAX;
    boundary_region.start_pc_offset = UINT32_MAX;
    CHECK(!st_method_descriptor_is_valid(&bad_method));
    boundary_region.start_pc_offset = UINT32_MAX - 1u;
    boundary_region.landing_pad_pc_offset = UINT32_MAX;
    CHECK(!st_method_descriptor_is_valid(&bad_method));

    bad_method = fixture.method;
    bad_method.source_start_offset = SIZE_MAX;
    bad_method.source_end_offset = 0u;
    CHECK(!st_method_descriptor_is_valid(&bad_method));

    StClassDescriptor bad_class = fixture.object_class;
    bad_class.metaclass_id = 0;
    CHECK(!st_class_descriptor_is_valid(&bad_class));
    bad_class = fixture.object_class;
    bad_class.superclass_id = fixture.metaclass.class_id;
    fixture.classes[0] = &bad_class;
    CHECK(st_runtime_descriptors_validate(&fixture.descriptors) ==
          ST_RUNTIME_ERR_INVALID_DESCRIPTOR);
}

static void test_method_entry(void)
{
    fixture_t fixture;
    const StMethodBinding *old = NULL;
    const StMethodBinding *expected;
    fixture_init(&fixture);
    StMethodBinding second = fixture.binding;
    second.code = return_false;
    second.version = 2;
    CHECK(st_method_entry_publish(&fixture.entry, &second, &old));
    CHECK(old == &fixture.binding);
    CHECK(st_method_entry_load(&fixture.entry) == &second);
    CHECK(st_method_entry_load(&fixture.entry)->code(NULL) ==
          st_value_false());
    CHECK(!st_method_entry_publish(&fixture.entry, &fixture.binding, NULL));

    StMethodDescriptor wrong_selector = fixture.method;
    wrong_selector.selector_id = 2;
    StMethodBinding wrong = { &wrong_selector, return_true, 3 };
    CHECK(!st_method_entry_publish(&fixture.entry, &wrong, NULL));
    expected = &second;
    StMethodBinding third = fixture.binding;
    third.version = 3;
    CHECK(st_method_entry_compare_exchange(&fixture.entry, &expected, &third));
    CHECK(st_method_entry_load(&fixture.entry) == &third);
    expected = &second;
    StMethodBinding fourth = fixture.binding;
    fourth.version = 4;
    CHECK(!st_method_entry_compare_exchange(&fixture.entry, &expected,
                                             &fourth));
    CHECK(expected == &third);
}

typedef struct {
    StMethodEntry *entry;
    const StMethodBinding *bindings;
    size_t binding_count;
    _Atomic bool done;
    _Atomic unsigned errors;
} publication_state_t;

static void *publication_writer(void *argument)
{
    publication_state_t *state = argument;
    size_t index;
    for (index = 1; index < state->binding_count; index++) {
        if (!st_method_entry_publish(state->entry, &state->bindings[index],
                                     NULL))
            atomic_fetch_add_explicit(&state->errors, 1,
                                      memory_order_relaxed);
    }
    atomic_store_explicit(&state->done, true, memory_order_release);
    return NULL;
}

static void *publication_reader(void *argument)
{
    publication_state_t *state = argument;
    uint64_t last_version = 0;
    do {
        const StMethodBinding *binding = st_method_entry_load(state->entry);
        st_value_t expected = (binding->version & 1) != 0
            ? st_value_true() : st_value_false();
        if (!st_method_binding_is_valid(binding) ||
            binding->version < last_version || binding->code(NULL) != expected)
            atomic_fetch_add_explicit(&state->errors, 1,
                                      memory_order_relaxed);
        last_version = binding->version;
    } while (!atomic_load_explicit(&state->done, memory_order_acquire));
    return NULL;
}

static void test_concurrent_publication(void)
{
    enum { BINDING_COUNT = 2048, READER_COUNT = 4 };
    fixture_t fixture;
    StMethodBinding *bindings = malloc(sizeof(*bindings) * BINDING_COUNT);
    pthread_t writer;
    pthread_t readers[READER_COUNT];
    publication_state_t state;
    size_t index;
    fixture_init(&fixture);
    CHECK(bindings != NULL);
    if (!bindings) return;
    for (index = 0; index < BINDING_COUNT; index++) {
        bindings[index] = (StMethodBinding) {
            .descriptor = &fixture.method,
            .code = ((index + 1) & 1) != 0 ? return_true : return_false,
            .version = index + 1
        };
    }
    CHECK(st_method_entry_init(&fixture.entry, &bindings[0]));
    state = (publication_state_t) {
        .entry = &fixture.entry,
        .bindings = bindings,
        .binding_count = BINDING_COUNT
    };
    atomic_init(&state.done, false);
    atomic_init(&state.errors, 0);
    for (index = 0; index < READER_COUNT; index++)
        CHECK(pthread_create(&readers[index], NULL, publication_reader,
                             &state) == 0);
    CHECK(pthread_create(&writer, NULL, publication_writer, &state) == 0);
    CHECK(pthread_join(writer, NULL) == 0);
    for (index = 0; index < READER_COUNT; index++)
        CHECK(pthread_join(readers[index], NULL) == 0);
    CHECK(atomic_load_explicit(&state.errors, memory_order_relaxed) == 0);
    CHECK(st_method_entry_load(&fixture.entry)->version == BINDING_COUNT);
    free(bindings);
}

typedef struct {
    size_t allocations;
    size_t deallocations;
    bool fail;
    bool misalign;
    unsigned char storage[256];
} allocation_state_t;

static void *test_allocate(void *user, size_t alignment, size_t size)
{
    allocation_state_t *state = user;
    uintptr_t begin;
    uintptr_t aligned;
    state->allocations++;
    if (state->fail || size > sizeof(state->storage) - alignment) return NULL;
    begin = (uintptr_t)state->storage;
    aligned = (begin + alignment - 1) & ~(uintptr_t)(alignment - 1);
    return (void *)(aligned + (state->misalign ? 1 : 0));
}

static void test_deallocate(void *user, void *pointer, size_t alignment,
                            size_t size)
{
    allocation_state_t *state = user;
    (void)pointer;
    (void)size;
    CHECK(alignment == 16);
    state->deallocations++;
}

static st_runtime_allocator_t test_allocator(allocation_state_t *state)
{
    return (st_runtime_allocator_t) {
        .allocate = test_allocate,
        .deallocate = test_deallocate,
        .user = state
    };
}

static bool count_reference(void *user, st_value_t *slot)
{
    size_t *count = user;
    CHECK(slot != NULL);
    (*count)++;
    return true;
}

static bool abort_reference(void *user, st_value_t *slot)
{
    (void)user;
    (void)slot;
    return false;
}

static bool invalidate_reference(void *user, st_value_t *slot)
{
    (void)user;
    *slot = 0;
    return true;
}

static void test_object_allocation_validation_and_scan(void)
{
    fixture_t fixture;
    st_object_extent_t extent = { 0 };
    st_object_extent_t referent_extent = { 0 };
    st_object_view_t view;
    st_value_t value = 0;
    st_value_t referent = 0;
    size_t visited = 0;
    size_t callbacks = 0;
    fixture_init(&fixture);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 2, 4, 0,
                             (st_runtime_allocator_t){ 0 }, &extent,
                             &value) == ST_RUNTIME_OK);
    CHECK(extent.byte_size == 80 && extent.allocation_alignment == 16);
    CHECK(st_object_validate(&fixture.descriptors, value, extent, &view) ==
          ST_RUNTIME_OK);
    CHECK(view.indexed_length == 2);
    CHECK(((st_value_t *)view.fixed_words)[0] == st_value_nil());
    CHECK(((uint64_t *)view.fixed_words)[1] == 0);
    CHECK(((st_value_t *)view.fixed_words)[2] == st_value_nil());
    CHECK(((st_value_t *)view.indexed_elements)[0] == st_value_nil());

    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, 0, 0,
                             (st_runtime_allocator_t){ 0 }, &referent_extent,
                             &referent) == ST_RUNTIME_OK);
    ((st_value_t *)view.fixed_words)[0] = referent;
    CHECK(st_value_from_small_integer(42,
          &((st_value_t *)view.fixed_words)[2]));
    ((st_value_t *)view.indexed_elements)[0] = referent;
    CHECK(st_object_visit_references(&fixture.descriptors, value, extent,
          count_reference, &callbacks, &visited) == ST_RUNTIME_OK);
    CHECK(visited == 2 && callbacks == 2);
    CHECK(st_object_visit_references(&fixture.descriptors, value, extent,
          abort_reference, NULL, &visited) ==
          ST_RUNTIME_ERR_VISITOR_ABORTED);
    CHECK(st_object_visit_references(&fixture.descriptors, value, extent,
          invalidate_reference, NULL, &visited) == ST_RUNTIME_ERR_BAD_OBJECT);
    ((st_value_t *)view.fixed_words)[0] = referent;

    st_object_extent_t short_extent = extent;
    short_extent.byte_size--;
    CHECK(st_object_validate(&fixture.descriptors, value, short_extent,
                             &view) == ST_RUNTIME_ERR_BAD_EXTENT);
    st_object_extent_t wrong_alignment = extent;
    wrong_alignment.allocation_alignment = 8;
    CHECK(st_object_validate(&fixture.descriptors, value, wrong_alignment,
                             &view) == ST_RUNTIME_ERR_BAD_EXTENT);
    CHECK(st_object_validate(&fixture.descriptors, referent, extent, &view) ==
          ST_RUNTIME_ERR_BAD_EXTENT);

    ((st_heap_object_t *)extent.base)->indexed_length = 5;
    CHECK(st_object_validate(&fixture.descriptors, value, extent, &view) ==
          ST_RUNTIME_ERR_BAD_OBJECT);
    ((st_heap_object_t *)extent.base)->indexed_length = 2;
    ((st_value_t *)((st_heap_object_t *)extent.base)->payload)[0] = 0;
    CHECK(st_object_visit_references(&fixture.descriptors, value, extent,
          count_reference, &callbacks, &visited) ==
          ST_RUNTIME_ERR_BAD_OBJECT);

    st_object_deallocate((st_runtime_allocator_t){ 0 }, referent_extent);
    st_object_deallocate((st_runtime_allocator_t){ 0 }, extent);
}

static void test_allocator_failures(void)
{
    fixture_t fixture;
    allocation_state_t state = { 0 };
    st_object_extent_t extent = { 0 };
    st_value_t value = UINT64_MAX;
    fixture_init(&fixture);
    state.fail = true;
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 1, 4, 0,
          test_allocator(&state), &extent, &value) ==
          ST_RUNTIME_ERR_OUT_OF_MEMORY);
    CHECK(value == 0 && extent.base == NULL && state.allocations == 1);
    state.fail = false;
    state.misalign = true;
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 1, 4, 0,
          test_allocator(&state), &extent, &value) ==
          ST_RUNTIME_ERR_BAD_ALIGNMENT);
    CHECK(state.deallocations == 1);
    state.misalign = false;
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 5, 4, 0,
          test_allocator(&state), &extent, &value) ==
          ST_RUNTIME_ERR_BAD_EXTENT);
    CHECK(state.allocations == 2);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, SIZE_MAX, 0,
          test_allocator(&state), &extent, &value) ==
          ST_RUNTIME_ERR_OVERFLOW);
    CHECK(state.allocations == 2);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 1, 4, 0,
          (st_runtime_allocator_t){ test_allocate, NULL, &state },
          &extent, &value) == ST_RUNTIME_ERR_INVALID_ARGUMENT);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, 0,
          ST_HEADER_WEAK, (st_runtime_allocator_t){ 0 }, &extent, &value)
          == ST_RUNTIME_ERR_INVALID_ARGUMENT);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, 0,
          ST_HEADER_FINALIZABLE, (st_runtime_allocator_t){ 0 }, &extent,
          &value) == ST_RUNTIME_ERR_INVALID_ARGUMENT);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, 0,
          ST_HEADER_REMEMBERED, (st_runtime_allocator_t){ 0 }, &extent,
          &value) == ST_RUNTIME_ERR_INVALID_ARGUMENT);
}

typedef struct {
    const st_runtime_descriptors_t *descriptors;
    st_value_t value;
    st_object_extent_t extent;
    st_runtime_status_t status;
} transition_state_t;

static void *transition_worker(void *argument)
{
    transition_state_t *state = argument;
    state->status = st_object_transition_shape(state->descriptors,
                                                state->value, state->extent,
                                                1, 3);
    return NULL;
}

static void test_concurrent_shape_transition(void)
{
    fixture_t fixture;
    st_object_extent_t extent = { 0 };
    st_value_t value = 0;
    transition_state_t states[2];
    pthread_t threads[2];
    fixture_init(&fixture);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 1, 4, 0,
          (st_runtime_allocator_t){ 0 }, &extent, &value) == ST_RUNTIME_OK);
    states[0] = (transition_state_t) {
        .descriptors = &fixture.descriptors, .value = value, .extent = extent
    };
    states[1] = states[0];
    CHECK(pthread_create(&threads[0], NULL, transition_worker,
                         &states[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, transition_worker,
                         &states[1]) == 0);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(pthread_join(threads[1], NULL) == 0);
    CHECK((states[0].status == ST_RUNTIME_OK &&
           states[1].status == ST_RUNTIME_ERR_CONFLICT) ||
          (states[1].status == ST_RUNTIME_OK &&
           states[0].status == ST_RUNTIME_ERR_CONFLICT));
    st_object_deallocate((st_runtime_allocator_t){ 0 }, extent);
}

static void test_shape_transition(void)
{
    fixture_t fixture;
    st_object_extent_t extent = { 0 };
    st_value_t value = 0;
    st_object_view_t view;
    fixture_init(&fixture);
    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 1, 4, 0,
          (st_runtime_allocator_t){ 0 }, &extent, &value) == ST_RUNTIME_OK);
    CHECK(st_object_transition_shape(&fixture.descriptors, value, extent,
          1, 3) == ST_RUNTIME_OK);
    CHECK(st_object_validate(&fixture.descriptors, value, extent, &view) ==
          ST_RUNTIME_OK);
    CHECK(view.shape_descriptor->shape_id == 3);
    CHECK(st_object_transition_shape(&fixture.descriptors, value, extent,
          1, 3) == ST_RUNTIME_ERR_CONFLICT);
    CHECK(st_object_transition_shape(&fixture.descriptors, value, extent,
          3, 4) == ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE);
    st_object_deallocate((st_runtime_allocator_t){ 0 }, extent);

    CHECK(st_object_allocate(&fixture.descriptors, 1, 1, 0, 0,
          ST_HEADER_IMMUTABLE, (st_runtime_allocator_t){ 0 }, &extent,
          &value) == ST_RUNTIME_OK);
    CHECK(st_object_transition_shape(&fixture.descriptors, value, extent,
          1, 3) == ST_RUNTIME_ERR_IMMUTABLE);
    st_object_deallocate((st_runtime_allocator_t){ 0 }, extent);
}

int main(void)
{
    test_descriptor_validation();
    test_method_entry();
    test_concurrent_publication();
    test_object_allocation_validation_and_scan();
    test_allocator_failures();
    test_shape_transition();
    test_concurrent_shape_transition();
    if (failures != 0) {
        fprintf(stderr, "runtime regression: %u failure(s)\n", failures);
        return 1;
    }
    puts("runtime descriptor regression: PASS");
    return 0;
}
