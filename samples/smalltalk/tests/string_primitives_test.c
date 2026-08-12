#define _POSIX_C_SOURCE 200809L

#include "st_core_primitives.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_source_bundle.h"
#include "st_stream_primitives.h"
#include "st_string_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_STRING = 2,
    CLASS_METACLASS = 3,
    CLASS_OTHER = 4,
    CLASS_COUNT = 4,
    SHAPE_OBJECT = 1,
    SHAPE_STRING8 = 2,
    SHAPE_STRING16 = 3,
    SHAPE_STRING32 = 4,
    SHAPE_METACLASS = 5,
    SHAPE_OTHER8 = 6,
    SHAPE_COUNT = 6
};

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} fault_allocator_t;

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT];
    StShapeDescriptor shape_storage[SHAPE_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[SHAPE_COUNT];
    st_runtime_descriptors_t descriptors;
} fixture_t;

static void *fault_allocate(void *user, size_t alignment, size_t size)
{
    fault_allocator_t *fault = user;
    void *pointer = NULL;
    fault->calls++;
    if (fault->calls == fault->fail_at) return NULL;
    if (posix_memalign(&pointer, alignment, size) != 0) return NULL;
    fault->live++;
    return pointer;
}

static void fault_deallocate(void *user, void *pointer,
                             size_t alignment, size_t size)
{
    fault_allocator_t *fault = user;
    (void)alignment;
    (void)size;
    CHECK(pointer != NULL && fault->live != 0u);
    if (pointer != NULL) {
        fault->live--;
        free(pointer);
    }
}

static void fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "NotNamedString", "Metaclass", "Other"
    };
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    for (index = 0u; index < CLASS_COUNT; index++) {
        uint32_t id = (uint32_t)index + 1u;
        fixture->class_storage[index] = (StClassDescriptor){
            .class_id = id,
            .superclass_id = id == CLASS_OBJECT || id == CLASS_METACLASS
                ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id == CLASS_OBJECT ? SHAPE_OBJECT
                : id == CLASS_STRING ? SHAPE_STRING8
                : id == CLASS_METACLASS ? SHAPE_METACLASS : SHAPE_OTHER8,
            .flags = id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->classes[index] = &fixture->class_storage[index];
    }
    fixture->shape_storage[SHAPE_OBJECT - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_OBJECT, .class_id = CLASS_OBJECT,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_NONE
    };
    fixture->shape_storage[SHAPE_STRING8 - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_STRING8, .class_id = CLASS_STRING,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT8
    };
    fixture->shape_storage[SHAPE_STRING16 - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_STRING16, .class_id = CLASS_STRING,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT16
    };
    fixture->shape_storage[SHAPE_STRING32 - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_STRING32, .class_id = CLASS_STRING,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT32
    };
    fixture->shape_storage[SHAPE_METACLASS - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_METACLASS, .class_id = CLASS_METACLASS,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_NONE
    };
    fixture->shape_storage[SHAPE_OTHER8 - 1u] = (StShapeDescriptor){
        .shape_id = SHAPE_OTHER8, .class_id = CLASS_OTHER,
        .allocation_alignment = 8u, .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT8
    };
    for (index = 0u; index < SHAPE_COUNT; index++)
        fixture->shapes[index] = &fixture->shape_storage[index];
    fixture->descriptors = (st_runtime_descriptors_t){
        .classes = fixture->classes, .class_count = CLASS_COUNT,
        .shapes = fixture->shapes, .shape_count = SHAPE_COUNT
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static st_string_primitive_options_t options(st_heap_t *heap)
{
    return (st_string_primitive_options_t){
        .heap = heap,
        .string_class_id = CLASS_STRING,
        .uint8_shape_id = SHAPE_STRING8,
        .uint16_shape_id = SHAPE_STRING16,
        .uint32_shape_id = SHAPE_STRING32
    };
}

static st_value_t small_integer(int64_t integer)
{
    st_value_t value = ST_VALUE_INVALID;
    CHECK(st_value_from_small_integer(integer, &value));
    return value;
}

static void store_scalar(st_object_view_t *view, size_t index, uint32_t scalar)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        ((uint8_t *)view->indexed_elements)[index] = (uint8_t)scalar;
        break;
    case ST_INDEXED_UINT16:
        ((uint16_t *)view->indexed_elements)[index] = (uint16_t)scalar;
        break;
    case ST_INDEXED_UINT32:
        ((uint32_t *)view->indexed_elements)[index] = scalar;
        break;
    default: CHECK(false); break;
    }
}

static uint32_t load_scalar(const st_object_view_t *view, size_t index)
{
    switch (view->shape_descriptor->indexed_format) {
    case ST_INDEXED_UINT8:
        return ((const uint8_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT16:
        return ((const uint16_t *)view->indexed_elements)[index];
    case ST_INDEXED_UINT32:
        return ((const uint32_t *)view->indexed_elements)[index];
    default: CHECK(false); return UINT32_MAX;
    }
}

static st_value_t make_string(st_heap_t *heap, uint32_t shape_id,
                              const uint32_t *scalars, size_t count,
                              st_header_flags_t flags)
{
    st_value_t value = ST_VALUE_INVALID;
    st_object_view_t view;
    size_t index;
    CHECK(st_heap_allocate(heap, CLASS_STRING, shape_id, count, count, flags,
                           &value) == ST_HEAP_OK);
    CHECK(st_heap_object_view(heap, value, &view) == ST_HEAP_OK);
    for (index = 0u; index < count; index++)
        store_scalar(&view, index, scalars[index]);
    return value;
}

static st_string_primitive_status_t execute1(
    st_string_primitive_context_t *context, st_string_operation_t operation,
    st_value_t receiver, st_value_t argument, st_value_t *result)
{
    return st_string_primitive_execute(context, operation, receiver,
                                       &argument, 1u, result);
}

static int64_t comparison(st_string_primitive_context_t *context,
                          st_value_t left, st_value_t right)
{
    st_value_t result = ST_VALUE_INVALID;
    int64_t integer = 99;
    CHECK(execute1(context, ST_STRING_OPERATION_COMPARE, left, right,
                   &result) == ST_STRING_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &integer));
    return integer;
}

static void test_context_and_contracts(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_string_primitive_context_t context = {0};
    st_string_primitive_options_t config;
    st_value_t result = 123u;
    size_t length = 9u;
    size_t count;
    const st_primitive_spec_t *specs;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    config = options(&heap);
    CHECK(st_string_primitive_context_init(&context, &config)
          == ST_STRING_PRIMITIVE_OK);
    CHECK(st_string_primitive_combined_length(10u, 20u, &length)
          && length == 30u);
    CHECK(!st_string_primitive_combined_length(SIZE_MAX, 1u, &length)
          && length == 0u);
    CHECK(!st_string_primitive_combined_length(0u, 0u, NULL));
    specs = st_string_primitive_specs(&count);
    CHECK(specs != NULL && count == 3u);
    CHECK(specs[0].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID
          && specs[1].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID
          && specs[2].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
    CHECK(specs[0].failure_policy == ST_PRIMITIVE_FALL_THROUGH
          && specs[1].failure_policy == ST_PRIMITIVE_FALL_THROUGH
          && specs[2].failure_policy == ST_PRIMITIVE_FALL_THROUGH
          && specs[0].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL
          && specs[1].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL
          && specs[2].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL
          && specs[0].runtime_symbol_length
             == sizeof("st_aot_string_compare_primitive_execute") - 1u
          && memcmp(specs[0].runtime_symbol,
                    "st_aot_string_compare_primitive_execute",
                    specs[0].runtime_symbol_length) == 0
          && specs[1].runtime_symbol_length
             == sizeof("st_aot_string_concat_primitive_execute") - 1u
          && memcmp(specs[1].runtime_symbol,
                    "st_aot_string_concat_primitive_execute",
                    specs[1].runtime_symbol_length) == 0
          && specs[2].runtime_symbol_length
             == sizeof("st_aot_string_as_symbol_primitive_execute") - 1u
          && memcmp(specs[2].runtime_symbol,
                    "st_aot_string_as_symbol_primitive_execute",
                    specs[2].runtime_symbol_length) == 0);
    CHECK(st_string_primitive_execute(&context, UINT32_MAX, st_value_nil(),
          NULL, 0u, &result) == ST_STRING_PRIMITIVE_ERR_UNKNOWN_INTRINSIC
          && result == ST_VALUE_INVALID);
    CHECK(st_string_primitive_execute(&context, ST_STRING_OPERATION_COMPARE,
          st_value_nil(), NULL, 0u, &result)
          == ST_STRING_PRIMITIVE_ERR_WRONG_ARITY);
    config.uint16_shape_id = SHAPE_STRING8;
    CHECK(st_string_primitive_context_init(&context, &config)
          == ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(st_string_primitive_execute(&context, ST_STRING_OPERATION_COMPARE,
          st_value_nil(), NULL, 0u, &result)
          == ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT);
    config = options(&heap);
    config.uint16_shape_id = SHAPE_STRING32;
    config.uint32_shape_id = SHAPE_STRING16;
    CHECK(st_string_primitive_context_init(&context, &config)
          == ST_STRING_PRIMITIVE_ERR_INVALID_DESCRIPTOR);
    st_string_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_compare_and_validation(void)
{
    static const uint32_t ascii[] = {'A', 'b', 'c'};
    static const uint32_t ascii_greater[] = {'A', 'b', 'd'};
    static const uint32_t prefix[] = {'A', 'b'};
    static const uint32_t bmp[] = {'A', UINT32_C(0x100), 'c'};
    static const uint32_t astral[] = {'A', UINT32_C(0x1f642), 'c'};
    fixture_t fixture;
    st_heap_t heap = {0};
    st_string_primitive_context_t context = {0};
    st_string_primitive_options_t config;
    st_value_t s8;
    st_value_t s16_ascii;
    st_value_t s32_ascii;
    st_value_t greater;
    st_value_t shorter;
    st_value_t wide16;
    st_value_t wide32;
    st_value_t other;
    st_value_t result = 1u;
    st_object_view_t view;
    void *foreign = NULL;
    st_value_t foreign_value = ST_VALUE_INVALID;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    config = options(&heap);
    CHECK(st_string_primitive_context_init(&context, &config)
          == ST_STRING_PRIMITIVE_OK);
    s8 = make_string(&heap, SHAPE_STRING8, ascii, 3u, ST_HEADER_IMMUTABLE);
    s16_ascii = make_string(&heap, SHAPE_STRING16, ascii, 3u,
                            ST_HEADER_IMMUTABLE);
    s32_ascii = make_string(&heap, SHAPE_STRING32, ascii, 3u,
                            ST_HEADER_IMMUTABLE);
    greater = make_string(&heap, SHAPE_STRING8, ascii_greater, 3u,
                          ST_HEADER_IMMUTABLE);
    shorter = make_string(&heap, SHAPE_STRING8, prefix, 2u,
                          ST_HEADER_IMMUTABLE);
    wide16 = make_string(&heap, SHAPE_STRING16, bmp, 3u,
                         ST_HEADER_IMMUTABLE);
    wide32 = make_string(&heap, SHAPE_STRING32, astral, 3u,
                         ST_HEADER_IMMUTABLE);
    CHECK(comparison(&context, s8, s8) == 0);
    CHECK(comparison(&context, s8, s16_ascii) == 0);
    CHECK(comparison(&context, s8, s32_ascii) == 0);
    CHECK(comparison(&context, s8, greater) == -1);
    CHECK(comparison(&context, greater, s8) == 1);
    CHECK(comparison(&context, shorter, s8) == -1);
    CHECK(comparison(&context, s8, shorter) == 1);
    CHECK(comparison(&context, s8, wide16) == -1);
    CHECK(comparison(&context, wide32, wide16) == 1);
    CHECK(comparison(&context, s8, s8) == 0); /* exact alias */

    CHECK(st_heap_allocate(&heap, CLASS_OTHER, SHAPE_OTHER8, 1u, 1u, 0u,
                           &other) == ST_HEAP_OK);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8, other, &result)
          == ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH
          && result == ST_VALUE_INVALID);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8,
                   small_integer(1), &result)
          == ST_STRING_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8, s8 + 8u,
                   &result) == ST_STRING_PRIMITIVE_ERR_NOT_MEMBER);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8,
                   ST_VALUE_INVALID, &result)
          == ST_STRING_PRIMITIVE_ERR_INVALID_VALUE);
    CHECK(posix_memalign(&foreign, 8u, 32u) == 0);
    if (foreign != NULL) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8,
                       foreign_value, &result)
              == ST_STRING_PRIMITIVE_ERR_NOT_MEMBER);
        free(foreign);
    }
    CHECK(st_heap_object_view(&heap, wide32, &view) == ST_HEAP_OK);
    ((uint32_t *)view.indexed_elements)[1] = UINT32_C(0xd800);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, s8, wide32,
                   &result) == ST_STRING_PRIMITIVE_ERR_BAD_OBJECT);
    ((uint32_t *)view.indexed_elements)[1] = UINT32_C(0x110000);
    CHECK(execute1(&context, ST_STRING_OPERATION_COMPARE, wide32, s8,
                   &result) == ST_STRING_PRIMITIVE_ERR_BAD_OBJECT);
    st_string_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void check_concat(st_string_primitive_context_t *context,
                         st_heap_t *heap, st_value_t left, st_value_t right,
                         uint32_t expected_shape, const uint32_t *expected,
                         size_t expected_count)
{
    st_value_t result = ST_VALUE_INVALID;
    st_object_view_t view;
    size_t index;
    CHECK(execute1(context, ST_STRING_OPERATION_CONCAT, left, right, &result)
          == ST_STRING_PRIMITIVE_OK);
    CHECK(result != left && result != right);
    CHECK(st_heap_object_view(heap, result, &view) == ST_HEAP_OK);
    CHECK(view.class_descriptor->class_id == CLASS_STRING);
    CHECK(view.shape_descriptor->shape_id == expected_shape);
    CHECK(view.indexed_length == expected_count
          && view.indexed_capacity == expected_count);
    CHECK((st_object_header_flags(st_object_header_load(&view.object->header))
           & ST_HEADER_IMMUTABLE) != 0u);
    for (index = 0u; index < expected_count; index++)
        CHECK(load_scalar(&view, index) == expected[index]);
}

static void test_concat_shapes_gc_and_oom(void)
{
    static const uint32_t left8_data[] = {'A', UINT32_C(0xff)};
    static const uint32_t right8_data[] = {'z'};
    static const uint32_t right16_data[] = {UINT32_C(0x100)};
    static const uint32_t right32_data[] = {UINT32_C(0x1f642)};
    static const uint32_t max16_data[] = {UINT32_C(0xffff)};
    static const uint32_t min32_data[] = {UINT32_C(0x10000)};
    static const uint32_t max_scalar_data[] = {UINT32_C(0x10ffff)};
    static const uint32_t expected8[] = {'A', UINT32_C(0xff), 'z'};
    static const uint32_t expected16[] = {'A', UINT32_C(0xff),
                                          UINT32_C(0x100)};
    static const uint32_t expected32[] = {'A', UINT32_C(0xff),
                                          UINT32_C(0x1f642)};
    fixture_t fixture;
    fault_allocator_t fault = {0};
    st_runtime_allocator_t allocator = {
        fault_allocate, fault_deallocate, &fault
    };
    st_heap_t heap = {0};
    st_string_primitive_context_t context = {0};
    st_string_primitive_options_t config;
    st_value_t left8;
    st_value_t right8;
    st_value_t right16;
    st_value_t right32;
    st_value_t max16;
    st_value_t min32;
    st_value_t max_scalar;
    st_value_t empty;
    st_value_t result = ST_VALUE_INVALID;
    st_value_t root;
    st_heap_collection_stats_t stats;
    size_t before;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors, allocator) == ST_HEAP_OK);
    config = options(&heap);
    CHECK(st_string_primitive_context_init(&context, &config)
          == ST_STRING_PRIMITIVE_OK);
    left8 = make_string(&heap, SHAPE_STRING8, left8_data, 2u,
                        ST_HEADER_IMMUTABLE);
    right8 = make_string(&heap, SHAPE_STRING8, right8_data, 1u,
                         ST_HEADER_IMMUTABLE);
    right16 = make_string(&heap, SHAPE_STRING16, right16_data, 1u,
                          ST_HEADER_IMMUTABLE);
    right32 = make_string(&heap, SHAPE_STRING32, right32_data, 1u,
                          ST_HEADER_IMMUTABLE);
    max16 = make_string(&heap, SHAPE_STRING16, max16_data, 1u,
                        ST_HEADER_IMMUTABLE);
    min32 = make_string(&heap, SHAPE_STRING32, min32_data, 1u,
                        ST_HEADER_IMMUTABLE);
    max_scalar = make_string(&heap, SHAPE_STRING32, max_scalar_data, 1u,
                             ST_HEADER_IMMUTABLE);
    empty = make_string(&heap, SHAPE_STRING8, NULL, 0u,
                        ST_HEADER_IMMUTABLE);
    check_concat(&context, &heap, left8, right8, SHAPE_STRING8,
                 expected8, 3u);
    check_concat(&context, &heap, left8, right16, SHAPE_STRING16,
                 expected16, 3u);
    check_concat(&context, &heap, left8, right32, SHAPE_STRING32,
                 expected32, 3u);
    check_concat(&context, &heap, left8, left8, SHAPE_STRING8,
                 (const uint32_t[]){'A', UINT32_C(0xff),
                                    'A', UINT32_C(0xff)}, 4u);
    check_concat(&context, &heap, empty, empty, SHAPE_STRING8, NULL, 0u);
    check_concat(&context, &heap, empty, max16, SHAPE_STRING16,
                 max16_data, 1u);
    check_concat(&context, &heap, empty, min32, SHAPE_STRING32,
                 min32_data, 1u);
    check_concat(&context, &heap, empty, max_scalar, SHAPE_STRING32,
                 max_scalar_data, 1u);

    before = st_heap_object_count(&heap);
    fault.fail_at = fault.calls + 1u;
    CHECK(execute1(&context, ST_STRING_OPERATION_CONCAT, left8, right8,
                   &result) == ST_STRING_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID && st_heap_object_count(&heap) == before);
    fault.fail_at = 0u;
    CHECK(execute1(&context, ST_STRING_OPERATION_CONCAT, left8, right32,
                   &result) == ST_STRING_PRIMITIVE_OK);
    root = result;
    CHECK(st_heap_collect(&heap, NULL, &root, 1u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_contains(&heap, result));
    CHECK(!st_heap_contains(&heap, left8)
          && !st_heap_contains(&heap, right32));
    CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(!st_heap_contains(&heap, result));
    st_string_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
    CHECK(fault.live == 0u);
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static bool register_specs(st_primitive_catalog_t *catalog,
                           const st_primitive_spec_t *specs, size_t count)
{
    size_t index;
    for (index = 0u; index < count; index++)
        if (st_primitive_catalog_register(catalog, &specs[index], NULL)
                != ST_PRIMITIVE_OK) return false;
    return true;
}

static void test_combined_image_catalog(void)
{
    const char *image = first_existing("st-image",
                                       "samples/smalltalk/st-image");
    st_source_bundle_t bundle;
    const st_ast_unit_t **units;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_primitive_spec_t *specs;
    size_t count;
    size_t index;
    CHECK(image != NULL);
    if (image == NULL) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    for (index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;
    CHECK(st_primitive_catalog_init(&catalog, (st_primitive_allocator_t){0}));
    specs = st_core_primitive_specs(&count);
    CHECK(register_specs(&catalog, specs, count));
    specs = st_heap_primitive_specs(&count);
    CHECK(register_specs(&catalog, specs, count));
    specs = st_float_primitive_specs(&count);
    CHECK(register_specs(&catalog, specs, count));
    specs = st_stream_primitive_specs(&count);
    CHECK(register_specs(&catalog, specs, count));
    specs = st_string_primitive_specs(&count);
    CHECK(register_specs(&catalog, specs, count));
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, bundle.count, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(result.binding_count == 53u);
    CHECK(result.diagnostic_count == 16u);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    test_context_and_contracts();
    test_compare_and_validation();
    test_concat_shapes_gc_and_oom();
    test_combined_image_catalog();
    if (failures != 0u) {
        fprintf(stderr, "String primitives: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("String primitives: PASS");
    return EXIT_SUCCESS;
}
