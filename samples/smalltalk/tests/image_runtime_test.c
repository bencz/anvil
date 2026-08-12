#include "st_image_runtime.h"
#include "st_lookup.h"
#include "st_send_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                        \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_STRING,
    CLASS_EXTERNAL_STREAM,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT
};

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT - 1];
    StShapeDescriptor shape_storage[CLASS_COUNT - 1];
    const StClassDescriptor *classes[CLASS_COUNT - 1];
    const StShapeDescriptor *shapes[CLASS_COUNT - 1];
    st_runtime_descriptors_t descriptors;
    uint64_t stream_bitmap;
} descriptors_t;

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live;
} allocation_t;

static void *tracked_allocate(void *user, size_t alignment, size_t size)
{
    allocation_t *allocation = user;
    allocation->calls++;
    if (allocation->fail_on != 0u
            && allocation->calls == allocation->fail_on)
        return NULL;
    void *result = aligned_alloc(alignment, size);
    if (result != NULL) allocation->live++;
    return result;
}

static void tracked_deallocate(void *user, void *pointer, size_t alignment,
                               size_t size)
{
    allocation_t *allocation = user;
    (void)alignment;
    (void)size;
    if (pointer != NULL) {
        CHECK(allocation->live != 0u);
        allocation->live--;
        free(pointer);
    }
}

static void descriptors_init(descriptors_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1] = {
        "Object", "ByteString", "ExternalStream", "UndefinedObject",
        "False", "True", "SmallInteger", "Character", "Metaclass"
    };
    memset(fixture, 0, sizeof(*fixture));
    fixture->stream_bitmap = UINT64_C(1);
    for (uint32_t id = 1u; id < CLASS_COUNT; id++) {
        size_t index = (size_t)id - 1u;
        fixture->class_storage[index] = (StClassDescriptor) {
            .class_id = id,
            .superclass_id = id == CLASS_OBJECT || id == CLASS_METACLASS
                ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id,
            .flags = id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shape_storage[index] = (StShapeDescriptor) {
            .shape_id = id,
            .class_id = id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->shape_storage[CLASS_STRING - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->shape_storage[CLASS_EXTERNAL_STREAM - 1u]
        .minimum_allocation_size = 32u;
    fixture->shape_storage[CLASS_EXTERNAL_STREAM - 1u].fixed_word_count = 1u;
    fixture->shape_storage[CLASS_EXTERNAL_STREAM - 1u]
        .fixed_pointer_bitmap = &fixture->stream_bitmap;
    fixture->shape_storage[CLASS_EXTERNAL_STREAM - 1u]
        .fixed_pointer_bitmap_word_count = 1u;
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, CLASS_COUNT - 1u,
        fixture->shapes, CLASS_COUNT - 1u
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static st_image_runtime_options_t options_for(
    descriptors_t *descriptors, st_heap_t *heap,
    const st_image_runtime_entry_t *globals, size_t global_count,
    const st_image_runtime_entry_t *literals, size_t literal_count)
{
    return (st_image_runtime_options_t) {
        .descriptors = &descriptors->descriptors,
        .borrowed_heap = heap,
        .globals = globals,
        .global_count = global_count,
        .literals = literals,
        .literal_count = literal_count,
        .string_layout = { CLASS_STRING, CLASS_STRING },
        .external_stream_layout = {
            CLASS_EXTERNAL_STREAM, CLASS_EXTERNAL_STREAM, 0u
        }
    };
}

typedef struct {
    st_value_t values[8];
    size_t count;
    size_t stop_after;
} visitor_t;

typedef struct {
    const st_value_t *values;
    size_t count;
} provider_roots_t;

static st_image_runtime_status_t provider_roots(
    void *owner, const st_value_t **roots_out, size_t *root_count_out)
{
    provider_roots_t *roots = owner;
    if (roots == NULL || roots_out == NULL || root_count_out == NULL)
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    *roots_out = roots->values;
    *root_count_out = roots->count;
    return ST_IMAGE_RUNTIME_OK;
}

static bool record_root(void *user, const st_value_t *root)
{
    visitor_t *visitor = user;
    if (root == NULL || visitor->count >= 8u) return false;
    visitor->values[visitor->count++] = *root;
    return visitor->stop_after == 0u
        || visitor->count < visitor->stop_after;
}

static void test_bootstrap_load_roots_and_gc(void)
{
    descriptors_t descriptors;
    st_heap_t heap = {0};
    st_value_t existing = ST_VALUE_INVALID;
    st_value_t garbage = ST_VALUE_INVALID;
    st_image_runtime_t image = {0};
    st_lookup_context_t lookup = {0};
    st_aot_thread_t thread = {0};
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    st_value_t seven;
    st_image_runtime_entry_t globals[4];
    st_image_runtime_entry_t literals[2];
    StMethodDescriptor method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_OBJECT,
        .arity = 0u,
        .code_size = 0u
    };
    StFrame frame = {0};
    st_value_t result;
    st_value_t string;
    st_value_t transcript;
    st_value_t dynamic_root = ST_VALUE_INVALID;
    st_object_view_t view;
    int64_t descriptor_value = 0;
    visitor_t visitor = {0};
    st_heap_collection_stats_t stats;

    descriptors_init(&descriptors);
    CHECK(st_heap_init(&heap, &descriptors.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_heap_allocate(&heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
                           &existing) == ST_HEAP_OK);
    CHECK(st_value_from_small_integer(7, &seven));
    globals[0] = (st_image_runtime_entry_t){ 2u, seven };
    globals[1] = (st_image_runtime_entry_t){ 4u, ST_VALUE_INVALID };
    globals[2] = (st_image_runtime_entry_t){ 1u, existing };
    globals[3] = (st_image_runtime_entry_t){ 3u, ST_VALUE_INVALID };
    literals[0] = (st_image_runtime_entry_t){ 2u, ST_VALUE_INVALID };
    literals[1] = (st_image_runtime_entry_t){ 1u, st_value_true() };
    st_image_runtime_options_t options = options_for(
        &descriptors, &heap, globals, 4u, literals, 2u);
    CHECK(st_image_runtime_init(&image, &options) == ST_IMAGE_RUNTIME_OK);
    CHECK(!image.owns_heap && st_image_runtime_heap(&image) == &heap
          && st_image_runtime_heap_const(&image) == &heap);
    CHECK(st_lookup_context_init(&lookup, &descriptors.descriptors,
                                 (st_lookup_allocator_t){0})
          == ST_LOOKUP_FOUND);
    CHECK(st_aot_thread_init(&thread, &lookup, immediate, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL));
    CHECK(st_aot_thread_image_attach(&thread, &image));
    CHECK(!st_aot_thread_image_attach(&thread, &image));
    frame = (StFrame) {
        .thread = &thread,
        .method = &method,
        .receiver = st_value_nil()
    };

    result = st_value_nil();
    CHECK(st_image_runtime_global_load(&frame, 0u, &result)
          == ST_IMAGE_RUNTIME_OK && result == existing);
    CHECK(st_image_runtime_global_load(&frame, 1u, &result)
          == ST_IMAGE_RUNTIME_OK && result == seven);
    CHECK(st_image_runtime_global_load(&frame, 2u, &result)
          == ST_IMAGE_RUNTIME_ERR_UNINITIALIZED
          && result == ST_VALUE_INVALID);
    CHECK(st_image_runtime_bootstrap_global_value(
              &image, 2u, st_value_false()) == ST_IMAGE_RUNTIME_OK);
    CHECK(st_image_runtime_global_load(&frame, 2u, &result)
          == ST_IMAGE_RUNTIME_OK && result == st_value_false());
    CHECK(st_image_runtime_bootstrap_global_value(
              &image, 2u, st_value_true()) == ST_IMAGE_RUNTIME_ERR_CONFLICT);
    CHECK(st_image_runtime_bootstrap_global_value(
              &image, 4u, st_value_true())
          == ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE);
    result = st_value_nil();
    CHECK(st_image_runtime_literal_load(&frame, 2u, &result)
          == ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE
          && result == ST_VALUE_INVALID);

    static const unsigned char hello[] = {
        'H', 'e', 'l', 'l', 'o', 0u, 0xc3u, 0xa1u
    };
    CHECK(st_image_runtime_bootstrap_string_literal(
              &image, 1u, hello, sizeof(hello), &string)
          == ST_IMAGE_RUNTIME_OK);
    CHECK(st_heap_object_view(&heap, string, &view) == ST_HEAP_OK
          && view.indexed_length == sizeof(hello)
          && memcmp(view.indexed_elements, hello, sizeof(hello)) == 0
          && (st_object_header_flags(
                  st_object_header_load(&view.object->header))
              & ST_HEADER_IMMUTABLE) != 0u);
    CHECK(st_image_runtime_literal_load(&frame, 1u, &result)
          == ST_IMAGE_RUNTIME_OK && result == string);
    result = st_value_nil();
    CHECK(st_image_runtime_bootstrap_string_literal(
              &image, 1u, hello, sizeof(hello), &result)
          == ST_IMAGE_RUNTIME_ERR_CONFLICT
          && result == ST_VALUE_INVALID);

    CHECK(st_image_runtime_bootstrap_transcript(&image, 3u, &transcript)
          == ST_IMAGE_RUNTIME_OK);
    CHECK(st_heap_object_view(&heap, transcript, &view) == ST_HEAP_OK
          && st_value_to_small_integer(
              ((st_value_t *)view.fixed_words)[0], &descriptor_value)
          && descriptor_value == 1);
    CHECK(st_image_runtime_global_load(&frame, 3u, &result)
          == ST_IMAGE_RUNTIME_OK && result == transcript);

    CHECK(st_image_runtime_visit_roots(&image, record_root, &visitor,
                                       &visitor.count)
          == ST_IMAGE_RUNTIME_OK);
    CHECK(visitor.count == 6u
          && visitor.values[0] == existing
          && visitor.values[1] == seven
          && visitor.values[2] == st_value_false()
          && visitor.values[3] == transcript
          && visitor.values[4] == st_value_true()
          && visitor.values[5] == string);
    visitor = (visitor_t){ .stop_after = 2u };
    size_t visited = 99u;
    CHECK(st_image_runtime_visit_roots(&image, record_root, &visitor,
                                       &visited)
          == ST_IMAGE_RUNTIME_ERR_VISITOR_ABORTED && visited == 0u);

    CHECK(st_heap_allocate(&heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
                           &garbage) == ST_HEAP_OK);
    CHECK(st_image_runtime_collect(&image, NULL, &stats)
          == ST_IMAGE_RUNTIME_OK
          && stats.reclaimed_objects == 1u
          && !st_heap_contains(&heap, garbage)
          && st_heap_contains(&heap, existing)
          && st_heap_contains(&heap, string)
          && st_heap_contains(&heap, transcript));

    CHECK(st_heap_allocate(&heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
                           &dynamic_root) == ST_HEAP_OK);
    provider_roots_t provider_span = { &dynamic_root, 1u };
    st_image_root_provider_t provider = {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION, &provider_span, provider_roots
    };
    CHECK(st_image_runtime_root_provider_attach(&image, &provider));
    CHECK(!st_image_runtime_root_provider_attach(&image, &provider));
    CHECK(st_image_runtime_root_provider_contains(&image, &provider));
    CHECK(st_image_runtime_root_provider_find(&image, provider_roots)
          == &provider);
    st_value_t second_dynamic_root;
    CHECK(st_heap_allocate(&heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
                           &second_dynamic_root) == ST_HEAP_OK);
    provider_roots_t second_provider_span = { &second_dynamic_root, 1u };
    st_image_root_provider_t second_provider = {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION,
        &second_provider_span,
        provider_roots
    };
    CHECK(st_image_runtime_root_provider_attach(&image, &second_provider));
    CHECK(st_image_runtime_root_provider_contains(&image, &second_provider));
    visitor = (visitor_t){0};
    CHECK(st_image_runtime_visit_roots(&image, record_root, &visitor,
                                       &visitor.count)
          == ST_IMAGE_RUNTIME_OK
          && visitor.count == 8u
          && visitor.values[6] == dynamic_root
          && visitor.values[7] == second_dynamic_root);
    CHECK(st_heap_allocate(&heap, CLASS_OBJECT, CLASS_OBJECT, 0u, 0u, 0u,
                           &garbage) == ST_HEAP_OK);
    CHECK(st_image_runtime_collect(&image, NULL, &stats)
          == ST_IMAGE_RUNTIME_OK
          && stats.reclaimed_objects == 1u
          && !st_heap_contains(&heap, garbage)
          && st_heap_contains(&heap, existing)
          && st_heap_contains(&heap, dynamic_root)
          && st_heap_contains(&heap, second_dynamic_root));
    CHECK(st_image_runtime_root_provider_detach(&image, &second_provider));
    CHECK(!st_image_runtime_root_provider_contains(
        &image, &second_provider));
    CHECK(st_image_runtime_root_provider_detach(&image, &provider));

    CHECK(!st_aot_thread_image_detach(&thread,
          (const st_image_runtime_t *)(uintptr_t)8u));
    CHECK(st_aot_thread_image_detach(&thread, &image));
    CHECK(!st_aot_thread_image_detach(&thread, &image));
    result = st_value_nil();
    CHECK(st_image_runtime_global_load(&frame, 0u, &result)
          == ST_IMAGE_RUNTIME_ERR_INVALID_STATE
          && result == ST_VALUE_INVALID);
    st_aot_thread_destroy(&thread);
    st_lookup_context_destroy(&lookup);
    st_image_runtime_destroy(&image);
    CHECK(heap.state != NULL && st_heap_contains(&heap, existing));
    st_heap_destroy(&heap);
}

static void expect_init_failure(
    descriptors_t *descriptors, st_heap_t *heap,
    const st_image_runtime_entry_t *globals, size_t global_count,
    st_image_runtime_status_t expected)
{
    st_image_runtime_t image = {0};
    st_image_runtime_options_t options = options_for(
        descriptors, heap, globals, global_count, NULL, 0u);
    CHECK(st_image_runtime_init(&image, &options) == expected);
    CHECK(!image.initialized && image.state == NULL && image.heap == NULL);
}

static void test_validation_transaction_and_oom(void)
{
    descriptors_t descriptors;
    st_heap_t heap = {0};
    st_value_t value;
    st_image_runtime_entry_t duplicate[2] = {
        { 1u, ST_VALUE_INVALID }, { 1u, st_value_nil() }
    };
    st_image_runtime_entry_t out_of_range[1] = {
        { 2u, st_value_nil() }
    };
    st_image_runtime_entry_t foreign[1] = {
        { 1u, UINT64_C(0x1000) }
    };
    st_image_runtime_entry_t malformed[1] = {
        { 1u, UINT64_C(0x5) }
    };
    descriptors_init(&descriptors);
    CHECK(st_heap_init(&heap, &descriptors.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    expect_init_failure(&descriptors, &heap, duplicate, 2u,
                        ST_IMAGE_RUNTIME_ERR_DUPLICATE_ID);
    expect_init_failure(&descriptors, &heap, out_of_range, 1u,
                        ST_IMAGE_RUNTIME_ERR_ID_OUT_OF_RANGE);
    expect_init_failure(&descriptors, &heap, foreign, 1u,
                        ST_IMAGE_RUNTIME_ERR_NOT_MEMBER);
    expect_init_failure(&descriptors, &heap, malformed, 1u,
                        ST_IMAGE_RUNTIME_ERR_INVALID_VALUE);

    st_image_runtime_entry_t entries[2] = {
        { 1u, ST_VALUE_INVALID }, { 2u, st_value_false() }
    };
    for (size_t failure_call = 1u; failure_call <= 3u; failure_call++) {
        allocation_t allocation = { .fail_on = failure_call };
        st_image_runtime_t image = {0};
        st_image_runtime_options_t options = options_for(
            &descriptors, &heap, entries, 2u, NULL, 0u);
        options.table_allocator = (st_runtime_allocator_t) {
            tracked_allocate, tracked_deallocate, &allocation
        };
        CHECK(st_image_runtime_init(&image, &options)
              == ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY);
        CHECK(allocation.live == 0u && !image.initialized
              && image.state == NULL);
    }

    allocation_t heap_allocation = {0};
    st_image_runtime_t owned = {0};
    st_image_runtime_entry_t owned_globals[1] = {
        { 1u, ST_VALUE_INVALID }
    };
    st_image_runtime_options_t owned_options = options_for(
        &descriptors, NULL, owned_globals, 1u, NULL, 0u);
    owned_options.heap_allocator = (st_runtime_allocator_t) {
        tracked_allocate, tracked_deallocate, &heap_allocation
    };
    CHECK(st_image_runtime_init(&owned, &owned_options)
          == ST_IMAGE_RUNTIME_OK && owned.owns_heap);
    CHECK(st_image_runtime_bootstrap_transcript(&owned, 0u, &value)
          == ST_IMAGE_RUNTIME_OK
          && st_heap_contains(st_image_runtime_heap(&owned), value));
    st_image_runtime_destroy(&owned);
    CHECK(heap_allocation.live == 0u);
    st_heap_destroy(&heap);
}

int main(void)
{
    test_bootstrap_load_roots_and_gc();
    test_validation_transaction_and_oom();
    CHECK(strcmp(st_image_runtime_status_string(
                     ST_IMAGE_RUNTIME_ERR_UNINITIALIZED),
                 "uninitialized slot") == 0);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk image runtime: %u failure(s)\n", failures);
        return 1;
    }
    puts("smalltalk image runtime: PASS (authenticated dense roots, AOT String/Transcript bootstrap, precise GC)");
    return 0;
}
