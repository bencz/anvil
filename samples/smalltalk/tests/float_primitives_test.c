#include "st_float_primitives.h"
#include "../src/runtime/primitives/float_primitives_internal.h"
#include "st_core_primitives.h"
#include "st_heap_primitives.h"
#include "st_source_bundle.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_NUMBER = 2,
    CLASS_FLOAT = 3,
    CLASS_BOXED_FLOAT64 = 4,
    CLASS_METACLASS = 5,
    CLASS_OTHER = 6,
    CLASS_COUNT = 6,
    SHAPE_BOXED_FLOAT64_ALTERNATE = 7,
    SHAPE_COUNT = 7
};

typedef struct {
    uint64_t float_bitmap;
    StClassDescriptor class_storage[CLASS_COUNT];
    StShapeDescriptor shape_storage[SHAPE_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[SHAPE_COUNT];
    st_runtime_descriptors_t descriptors;
} descriptor_fixture_t;

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live_blocks;
} fault_allocator_t;

typedef struct {
    descriptor_fixture_t *fixture;
    size_t calls;
} invariant_break_allocator_t;

static void descriptor_fixture_init(descriptor_fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Number", "Float", "BoxedFloat64", "Metaclass", "Other"
    };
    static const uint32_t superclasses[CLASS_COUNT] = {
        0u, CLASS_OBJECT, CLASS_NUMBER, CLASS_FLOAT, 0u, CLASS_OBJECT
    };
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    for (index = 0u; index < CLASS_COUNT; ++index) {
        uint32_t id = (uint32_t)index + 1u;
        fixture->class_storage[index] = (StClassDescriptor){
            .class_id = id,
            .superclass_id = superclasses[index],
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id,
            .flags = id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shape_storage[index] = (StShapeDescriptor){
            .shape_id = id,
            .class_id = id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .fixed_word_count = 0u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u].fixed_word_count = 1u;
    fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u]
        .minimum_allocation_size = 32u;
    fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u].fixed_pointer_bitmap =
        &fixture->float_bitmap;
    fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u]
        .fixed_pointer_bitmap_word_count = 1u;
    fixture->shape_storage[SHAPE_BOXED_FLOAT64_ALTERNATE - 1u] =
        (StShapeDescriptor){
            .shape_id = SHAPE_BOXED_FLOAT64_ALTERNATE,
            .class_id = CLASS_BOXED_FLOAT64,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 32u,
            .fixed_word_count = 1u,
            .indexed_format = ST_INDEXED_NONE,
            .fixed_pointer_bitmap = &fixture->float_bitmap,
            .fixed_pointer_bitmap_word_count = 1u
        };
    fixture->shapes[SHAPE_BOXED_FLOAT64_ALTERNATE - 1u] =
        &fixture->shape_storage[SHAPE_BOXED_FLOAT64_ALTERNATE - 1u];
    fixture->descriptors = (st_runtime_descriptors_t){
        .classes = fixture->classes,
        .class_count = CLASS_COUNT,
        .shapes = fixture->shapes,
        .shape_count = SHAPE_COUNT
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors) ==
          ST_RUNTIME_OK);
}

static void *fault_runtime_allocate(void *user, size_t alignment, size_t size)
{
    fault_allocator_t *fault = user;
    void *pointer;
    size_t call = fault->calls++;
    if (call == fault->fail_at) return NULL;
    pointer = aligned_alloc(alignment, size);
    if (pointer) ++fault->live_blocks;
    return pointer;
}

static void fault_runtime_deallocate(void *user, void *pointer,
                                     size_t alignment, size_t size)
{
    fault_allocator_t *fault = user;
    (void)alignment;
    (void)size;
    if (!pointer) return;
    CHECK(fault->live_blocks != 0u);
    --fault->live_blocks;
    free(pointer);
}

static void *fault_primitive_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    size_t call = fault->calls++;
    void *pointer;
    if (call == fault->fail_at) return NULL;
    pointer = malloc(size);
    if (pointer) ++fault->live_blocks;
    return pointer;
}

static void fault_primitive_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (!pointer) return;
    CHECK(fault->live_blocks != 0u);
    --fault->live_blocks;
    free(pointer);
}

static void *invariant_break_allocate(void *user, size_t alignment,
                                      size_t size)
{
    invariant_break_allocator_t *allocator = user;
    if (allocator->calls++ == 3u) {
        /* Deliberately violate the documented image-lifetime descriptor
           covenant after st_object_allocate validated the shape. */
        allocator->fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u]
            .fixed_word_count = 0u;
    }
    return aligned_alloc(alignment, size);
}

static void invariant_break_deallocate(void *user, void *pointer,
                                       size_t alignment, size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    free(pointer);
}

static st_runtime_allocator_t runtime_allocator(fault_allocator_t *fault)
{
    return (st_runtime_allocator_t){
        fault_runtime_allocate, fault_runtime_deallocate, fault
    };
}

static st_float_primitive_options_t float_options(
    st_heap_t *heap, st_primitive_allocator_t allocator)
{
    return (st_float_primitive_options_t){
        .heap = heap,
        .boxed_float64_class_id = CLASS_BOXED_FLOAT64,
        .boxed_float64_shape_id = CLASS_BOXED_FLOAT64,
        .allocator = allocator
    };
}

static st_float_primitive_status_t init_default_context(
    st_float_primitive_context_t *context, st_heap_t *heap)
{
    st_float_primitive_options_t options = float_options(
        heap, (st_primitive_allocator_t){0});
    return st_float_primitive_context_init(context, &options);
}

static uint64_t bits_of_double(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static st_value_t box(st_float_primitive_context_t *context, uint64_t bits)
{
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_float_primitive_box_bits(context, bits, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(result != ST_VALUE_INVALID);
    return result;
}

static uint64_t unbox(st_float_primitive_context_t *context, st_value_t value)
{
    uint64_t result = 0u;
    CHECK(st_float_primitive_unbox_bits(context, value, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    return result;
}

static st_float_primitive_status_t execute0(
    st_float_primitive_context_t *context, st_float_operation_t operation,
    st_value_t receiver, st_value_t *result_out)
{
    return st_float_primitive_execute_internal(
        context, operation, receiver, NULL, 0u, result_out);
}

static st_float_primitive_status_t execute1(
    st_float_primitive_context_t *context, st_float_operation_t operation,
    st_value_t receiver, st_value_t argument, st_value_t *result_out)
{
    return st_float_primitive_execute_internal(
        context, operation, receiver, &argument, 1u, result_out);
}

static int64_t integer_result(st_value_t value)
{
    int64_t integer = 0;
    CHECK(st_value_to_small_integer(value, &integer));
    return integer;
}

static bool boolean_result(st_value_t value)
{
    bool boolean = false;
    CHECK(st_value_to_boolean(value, &boolean));
    return boolean;
}

static void test_context_and_exact_representation(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_heap_t foreign_heap = {0};
    st_float_primitive_context_t context = {0};
    st_float_primitive_context_t foreign_context = {0};
    static const uint64_t vectors[] = {
        UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
        UINT64_C(0x0000000000000001), UINT64_C(0x8000000000000001),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000001234), UINT64_C(0xfff0000000005678),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)
    };
    size_t index;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_heap_init(&foreign_heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_float_primitive_context_init(
              &context, &(st_float_primitive_options_t){
                  .heap = &heap,
                  .boxed_float64_class_id = CLASS_BOXED_FLOAT64,
                  .boxed_float64_shape_id = CLASS_BOXED_FLOAT64
              }) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(st_float_primitive_context_init(
              &foreign_context, &(st_float_primitive_options_t){
                  .heap = &foreign_heap,
                  .boxed_float64_class_id = CLASS_BOXED_FLOAT64,
                  .boxed_float64_shape_id = CLASS_BOXED_FLOAT64
              }) == ST_FLOAT_PRIMITIVE_OK);
    for (index = 0u; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        size_t objects_before = st_heap_object_count(&heap);
        st_value_t value = box(&context, vectors[index]);
        CHECK(st_heap_object_count(&heap) == objects_before + 1u);
        CHECK(unbox(&context, value) == vectors[index]);
        st_object_view_t view;
        CHECK(st_heap_object_view(&heap, value, &view) == ST_HEAP_OK);
        CHECK(view.class_descriptor->class_id == CLASS_BOXED_FLOAT64);
        CHECK(view.shape_descriptor->shape_id == CLASS_BOXED_FLOAT64);
        CHECK((st_object_header_flags(
                  st_object_header_load(&view.object->header)) &
               ST_HEADER_IMMUTABLE) != 0u);
    }
    {
        st_value_t other = ST_VALUE_INVALID;
        st_value_t alternate = ST_VALUE_INVALID;
        st_value_t foreign = box(&foreign_context,
                                 UINT64_C(0x3ff0000000000000));
        uint64_t bits = UINT64_MAX;
        CHECK(st_heap_allocate(&heap, CLASS_OTHER, CLASS_OTHER, 0u, 0u, 0u,
                               &other) == ST_HEAP_OK);
        CHECK(st_heap_allocate(&heap, CLASS_BOXED_FLOAT64,
                               SHAPE_BOXED_FLOAT64_ALTERNATE, 0u, 0u, 0u,
                               &alternate) == ST_HEAP_OK);
        CHECK(st_float_primitive_unbox_bits(&context, st_value_true(), &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH && bits == 0u);
        CHECK(st_float_primitive_unbox_bits(&context, other, &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH && bits == 0u);
        CHECK(st_float_primitive_unbox_bits(&context, alternate, &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH && bits == 0u);
        CHECK(st_float_primitive_unbox_bits(&context, foreign, &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER && bits == 0u);
        CHECK(st_float_primitive_unbox_bits(&context, foreign + 8u, &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER && bits == 0u);
        CHECK(st_float_primitive_unbox_bits(&context, ST_VALUE_INVALID, &bits) ==
              ST_FLOAT_PRIMITIVE_ERR_INVALID_VALUE && bits == 0u);
        {
            st_value_t stale = box(&context,
                                   UINT64_C(0x400921fb54442d18));
            st_heap_collection_stats_t stats;
            CHECK(st_heap_collect(&heap, NULL, NULL, 0u, &stats) == ST_HEAP_OK);
            CHECK(stats.reclaimed_objects != 0u);
            CHECK(st_float_primitive_unbox_bits(&context, stale, &bits) ==
                  ST_FLOAT_PRIMITIVE_ERR_NOT_MEMBER && bits == 0u);
        }
    }
    st_float_primitive_context_destroy(&foreign_context);
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&foreign_heap);
    st_heap_destroy(&heap);
}

static void test_arithmetic_nan_zero_and_limits(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    st_value_t result;
    st_value_t zero;
    st_value_t negative_zero;
    st_value_t one;
    st_value_t two;
    st_value_t infinity;
    st_value_t qnan;
    st_value_t snan;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    zero = box(&context, UINT64_C(0x0000000000000000));
    negative_zero = box(&context, UINT64_C(0x8000000000000000));
    one = box(&context, UINT64_C(0x3ff0000000000000));
    two = box(&context, UINT64_C(0x4000000000000000));
    infinity = box(&context, UINT64_C(0x7ff0000000000000));
    qnan = box(&context, UINT64_C(0xfff8000000001234));
    snan = box(&context, UINT64_C(0x7ff0000000005678));

    CHECK(execute1(&context, ST_FLOAT_OPERATION_ADD, one, two, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x4008000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_SUBTRACT, one, two, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0xbff0000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_MULTIPLY, two, negative_zero,
                   &result) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x8000000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_DIVIDE, one, zero, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x7ff0000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_DIVIDE, zero, zero, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x7ff8000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_SUBTRACT, infinity, infinity,
                   &result) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x7ff8000000000000));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_ADD, qnan, one, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0xfff8000000001234));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_ADD, one, snan, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x7ff8000000005678));
    CHECK(execute0(&context, ST_FLOAT_OPERATION_NEGATE, qnan, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x7ff8000000001234));
    CHECK(execute0(&context, ST_FLOAT_OPERATION_NEGATE, zero, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(unbox(&context, result) == UINT64_C(0x8000000000000000));
    {
        st_value_t subnormal = box(&context, UINT64_C(1));
        st_value_t maximum = box(&context, UINT64_C(0x7fefffffffffffff));
        CHECK(execute1(&context, ST_FLOAT_OPERATION_ADD, subnormal, zero,
                       &result) == ST_FLOAT_PRIMITIVE_OK);
        CHECK(unbox(&context, result) == UINT64_C(1));
        CHECK(execute1(&context, ST_FLOAT_OPERATION_MULTIPLY, maximum, two,
                       &result) == ST_FLOAT_PRIMITIVE_OK);
        CHECK(unbox(&context, result) == UINT64_C(0x7ff0000000000000));
    }
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_comparisons(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    st_value_t result = ST_VALUE_INVALID;
    st_value_t positive_zero;
    st_value_t negative_zero;
    st_value_t one;
    st_value_t infinity;
    st_value_t nan;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    positive_zero = box(&context, UINT64_C(0));
    negative_zero = box(&context, UINT64_C(0x8000000000000000));
    one = box(&context, UINT64_C(0x3ff0000000000000));
    infinity = box(&context, UINT64_C(0x7ff0000000000000));
    nan = box(&context, UINT64_C(0x7ff0000000000042));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_EQUALS, positive_zero,
                   negative_zero, &result) == ST_FLOAT_PRIMITIVE_OK &&
          boolean_result(result));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_LESS_THAN, positive_zero,
                   negative_zero, &result) == ST_FLOAT_PRIMITIVE_OK &&
          !boolean_result(result));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_LESS_EQUALS, positive_zero,
                   negative_zero, &result) == ST_FLOAT_PRIMITIVE_OK &&
          boolean_result(result));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_GREATER_THAN, infinity,
                   one, &result) == ST_FLOAT_PRIMITIVE_OK &&
          boolean_result(result));
    CHECK(execute1(&context, ST_FLOAT_OPERATION_GREATER_EQUALS, one,
                   one, &result) == ST_FLOAT_PRIMITIVE_OK &&
          boolean_result(result));
    {
        static const st_float_operation_t comparisons[] = {
            ST_FLOAT_OPERATION_EQUALS, ST_FLOAT_OPERATION_LESS_THAN,
            ST_FLOAT_OPERATION_GREATER_THAN, ST_FLOAT_OPERATION_LESS_EQUALS,
            ST_FLOAT_OPERATION_GREATER_EQUALS
        };
        size_t index;
        for (index = 0u;
             index < sizeof(comparisons) / sizeof(comparisons[0]); ++index) {
            CHECK(execute1(&context, comparisons[index], nan, one, &result) ==
                  ST_FLOAT_PRIMITIVE_OK);
            CHECK(!boolean_result(result));
        }
    }
    CHECK(execute1(&context, ST_FLOAT_OPERATION_EQUALS, one, st_value_true(),
                   &result) == ST_FLOAT_PRIMITIVE_OK &&
          !boolean_result(result));
    result = UINT64_MAX;
    CHECK(execute1(&context, ST_FLOAT_OPERATION_LESS_THAN, one,
                   st_value_true(), &result) ==
          ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH &&
          result == ST_VALUE_INVALID);
    result = UINT64_MAX;
    CHECK(execute1(&context, ST_FLOAT_OPERATION_EQUALS, st_value_nil(), one,
                   &result) == ST_FLOAT_PRIMITIVE_ERR_TYPE_MISMATCH &&
          result == ST_VALUE_INVALID);
    {
        st_value_t positive_zero_hash;
        st_value_t negative_zero_hash;
        st_value_t one_hash;
        st_value_t another_one = box(
            &context, UINT64_C(0x3ff0000000000000));
        st_value_t another_one_hash;

        CHECK(execute0(&context, ST_FLOAT_OPERATION_HASH, positive_zero,
                       &positive_zero_hash) == ST_FLOAT_PRIMITIVE_OK);
        CHECK(execute0(&context, ST_FLOAT_OPERATION_HASH, negative_zero,
                       &negative_zero_hash) == ST_FLOAT_PRIMITIVE_OK);
        CHECK(positive_zero_hash == negative_zero_hash);
        CHECK(execute0(&context, ST_FLOAT_OPERATION_HASH, one, &one_hash) ==
              ST_FLOAT_PRIMITIVE_OK);
        CHECK(execute0(&context, ST_FLOAT_OPERATION_HASH, another_one,
                       &another_one_hash) == ST_FLOAT_PRIMITIVE_OK);
        CHECK(one_hash == another_one_hash);
    }
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void check_conversion(st_float_primitive_context_t *context,
                             st_float_operation_t operation, double input,
                             int64_t expected)
{
    st_value_t result = ST_VALUE_INVALID;
    st_value_t receiver = box(context, bits_of_double(input));
    CHECK(execute0(context, operation, receiver, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(integer_result(result) == expected);
}

static void test_integer_conversions(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    st_value_t result;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED, 1.9, 1);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED, -1.9, -1);
    check_conversion(&context, ST_FLOAT_OPERATION_FLOOR, -1.1, -2);
    check_conversion(&context, ST_FLOAT_OPERATION_CEILING, -1.1, -1);
    check_conversion(&context, ST_FLOAT_OPERATION_ROUNDED, 1.5, 2);
    check_conversion(&context, ST_FLOAT_OPERATION_ROUNDED, -1.5, -2);
    check_conversion(&context, ST_FLOAT_OPERATION_ROUNDED, 2.5, 3);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED, -0.0, 0);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED,
                     double_from_bits(UINT64_C(1)), 0);
    check_conversion(&context, ST_FLOAT_OPERATION_FLOOR,
                     double_from_bits(UINT64_C(0x8000000000000001)), -1);
    check_conversion(&context, ST_FLOAT_OPERATION_CEILING,
                     double_from_bits(UINT64_C(1)), 1);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED, -0x1p60,
                     ST_SMALL_INTEGER_MIN);
    check_conversion(&context, ST_FLOAT_OPERATION_TRUNCATED,
                     nextafter(0x1p60, 0.0),
                     INT64_C(1152921504606846848));
    {
        static const uint64_t non_finite[] = {
            UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
            UINT64_C(0x7ff8000000000001)
        };
        size_t index;
        for (index = 0u;
             index < sizeof(non_finite) / sizeof(non_finite[0]); ++index) {
            st_value_t receiver = box(&context, non_finite[index]);
            result = UINT64_MAX;
            CHECK(execute0(&context, ST_FLOAT_OPERATION_TRUNCATED,
                           receiver, &result) ==
                  ST_FLOAT_PRIMITIVE_ERR_NON_FINITE);
            CHECK(result == ST_VALUE_INVALID);
        }
    }
    {
        st_value_t too_large = box(&context, bits_of_double(0x1p60));
        st_value_t too_small = box(&context,
                                   bits_of_double(-0x1.0000000000001p60));
        result = UINT64_MAX;
        CHECK(execute0(&context, ST_FLOAT_OPERATION_TRUNCATED,
                       too_large, &result) ==
              ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED);
        CHECK(result == ST_VALUE_INVALID);
        result = UINT64_MAX;
        CHECK(execute0(&context, ST_FLOAT_OPERATION_FLOOR,
                       too_small, &result) ==
              ST_FLOAT_PRIMITIVE_ERR_PROMOTION_REQUIRED);
        CHECK(result == ST_VALUE_INVALID);
    }
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_rounding_environment_isolated(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    fenv_t original;
    static const int ambient_modes[] = { FE_UPWARD, FE_DOWNWARD };
    struct arithmetic_case {
        st_float_operation_t operation;
        uint64_t left;
        uint64_t right;
        uint64_t expected;
    } cases[] = {
        { ST_FLOAT_OPERATION_ADD, UINT64_C(0x3ff0000000000000),
          bits_of_double(0x1p-53), UINT64_C(0x3ff0000000000000) },
        { ST_FLOAT_OPERATION_SUBTRACT, UINT64_C(0x3ff0000000000000),
          bits_of_double(0x1p-54), UINT64_C(0x3ff0000000000000) },
        { ST_FLOAT_OPERATION_MULTIPLY, UINT64_C(0x3ff0000000000001),
          UINT64_C(0x3ff0000000000001), UINT64_C(0x3ff0000000000002) },
        { ST_FLOAT_OPERATION_DIVIDE, UINT64_C(0x3ff0000000000000),
          UINT64_C(0x4024000000000000), UINT64_C(0x3fb999999999999a) },
        { ST_FLOAT_OPERATION_DIVIDE, UINT64_C(0x3ff0000000000000),
          UINT64_C(0), UINT64_C(0x7ff0000000000000) },
        { ST_FLOAT_OPERATION_MULTIPLY, UINT64_C(0x7fefffffffffffff),
          UINT64_C(0x4000000000000000), UINT64_C(0x7ff0000000000000) }
    };
    size_t mode_index;
    CHECK(fegetenv(&original) == 0);
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    for (mode_index = 0u;
         mode_index < sizeof(ambient_modes) / sizeof(ambient_modes[0]);
         ++mode_index) {
        size_t case_index;
        CHECK(fesetround(ambient_modes[mode_index]) == 0);
        CHECK(feclearexcept(FE_ALL_EXCEPT) == 0);
        CHECK(feraiseexcept(FE_INVALID | FE_OVERFLOW) == 0);
        for (case_index = 0u;
             case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
            st_value_t left = box(&context, cases[case_index].left);
            st_value_t right = box(&context, cases[case_index].right);
            st_value_t result = ST_VALUE_INVALID;
            int flags_before = fetestexcept(FE_ALL_EXCEPT);
            CHECK(execute1(&context, cases[case_index].operation,
                           left, right, &result) == ST_FLOAT_PRIMITIVE_OK);
            CHECK(unbox(&context, result) == cases[case_index].expected);
            CHECK(fegetround() == ambient_modes[mode_index]);
            CHECK(fetestexcept(FE_ALL_EXCEPT) == flags_before);
        }
    }
    CHECK(fesetenv(&original) == 0);
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_status_arity_and_oom(void)
{
    descriptor_fixture_t fixture;
    fault_allocator_t context_fault = { .fail_at = 0u };
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    st_float_primitive_options_t options;
    st_value_t result = UINT64_MAX;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    {
        st_float_primitive_options_t bad = float_options(
            &heap, (st_primitive_allocator_t){0});
        bad.boxed_float64_shape_id = SHAPE_BOXED_FLOAT64_ALTERNATE;
        CHECK(st_float_primitive_context_init(&context, &bad) ==
              ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR);
        CHECK(context.state == NULL);
        bad = float_options(&heap, (st_primitive_allocator_t){0});
        bad.boxed_float64_class_id = CLASS_OTHER;
        bad.boxed_float64_shape_id = CLASS_OTHER;
        CHECK(st_float_primitive_context_init(&context, &bad) ==
              ST_FLOAT_PRIMITIVE_ERR_INVALID_DESCRIPTOR);
        CHECK(context.state == NULL);
    }
    options = float_options(&heap, (st_primitive_allocator_t){
        fault_primitive_allocate, fault_primitive_deallocate, &context_fault
    });
    CHECK(st_float_primitive_context_init(&context, &options) ==
          ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(context.state == NULL && context_fault.live_blocks == 0u);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(st_float_primitive_execute_internal(&context, UINT32_MAX, st_value_nil(),
                                     NULL, 0u, &result) ==
          ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(st_float_primitive_execute_internal(&context, ST_FLOAT_OPERATION_NEGATE,
                                     st_value_nil(), NULL, 1u, &result) ==
          ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID);
    {
        st_value_t one = box(&context, bits_of_double(1.0));
        CHECK(st_float_primitive_execute_internal(&context, ST_FLOAT_OPERATION_NEGATE,
                                         one, &one, 1u, &result) ==
              ST_FLOAT_PRIMITIVE_ERR_WRONG_ARITY);
        CHECK(result == ST_VALUE_INVALID);
    }
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);

    for (size_t fail_offset = 0u; fail_offset < 3u; ++fail_offset) {
        fault_allocator_t heap_fault = { .fail_at = SIZE_MAX };
        size_t calls_before;
        heap = (st_heap_t){0};
        context = (st_float_primitive_context_t){0};
        CHECK(st_heap_init(&heap, &fixture.descriptors,
                           runtime_allocator(&heap_fault)) == ST_HEAP_OK);
        CHECK(init_default_context(&context, &heap) ==
              ST_FLOAT_PRIMITIVE_OK);
        calls_before = heap_fault.calls;
        heap_fault.fail_at = calls_before + fail_offset;
        result = UINT64_MAX;
        CHECK(st_float_primitive_box_bits(&context,
                  UINT64_C(0x3ff0000000000000), &result) ==
              ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(result == ST_VALUE_INVALID);
        CHECK(st_heap_object_count(&heap) == 0u);
        CHECK(st_heap_allocated_bytes(&heap) == 0u);
        st_float_primitive_context_destroy(&context);
        st_heap_destroy(&heap);
        CHECK(heap_fault.live_blocks == 0u);
    }

    {
        fault_allocator_t heap_fault = { .fail_at = SIZE_MAX };
        st_value_t left;
        st_value_t right;
        size_t objects_before;
        heap = (st_heap_t){0};
        context = (st_float_primitive_context_t){0};
        CHECK(st_heap_init(&heap, &fixture.descriptors,
                           runtime_allocator(&heap_fault)) == ST_HEAP_OK);
        CHECK(init_default_context(&context, &heap) ==
              ST_FLOAT_PRIMITIVE_OK);
        left = box(&context, bits_of_double(1.0));
        right = box(&context, bits_of_double(2.0));
        objects_before = st_heap_object_count(&heap);
        heap_fault.fail_at = heap_fault.calls;
        result = UINT64_MAX;
        CHECK(execute1(&context, ST_FLOAT_OPERATION_ADD,
                       left, right, &result) ==
              ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(result == ST_VALUE_INVALID);
        CHECK(st_heap_object_count(&heap) == objects_before);
        st_float_primitive_context_destroy(&context);
        st_heap_destroy(&heap);
        CHECK(heap_fault.live_blocks == 0u);
    }
    {
        st_heap_t exhausted_heap = {0};
        st_float_primitive_context_t exhausted_context = {0};
        st_value_t first = ST_VALUE_INVALID;
        CHECK(st_heap_init_with_identity_seed(
                  &exhausted_heap, &fixture.descriptors,
                  (st_runtime_allocator_t){0}, UINT64_MAX) == ST_HEAP_OK);
        CHECK(init_default_context(&exhausted_context, &exhausted_heap) ==
              ST_FLOAT_PRIMITIVE_OK);
        CHECK(st_float_primitive_box_bits(
                  &exhausted_context, bits_of_double(1.0), &first) ==
              ST_FLOAT_PRIMITIVE_OK);
        result = UINT64_MAX;
        CHECK(st_float_primitive_box_bits(
                  &exhausted_context, bits_of_double(2.0), &result) ==
              ST_FLOAT_PRIMITIVE_ERR_OVERFLOW);
        CHECK(result == ST_VALUE_INVALID);
        CHECK(st_heap_object_count(&exhausted_heap) == 1u);
        st_float_primitive_context_destroy(&exhausted_context);
        st_heap_destroy(&exhausted_heap);
    }
}

static void test_post_allocation_invariant_is_fatal(void)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        descriptor_fixture_t fixture;
        invariant_break_allocator_t allocator;
        st_heap_t heap = {0};
        st_float_primitive_context_t context = {0};
        st_value_t result = ST_VALUE_INVALID;
        descriptor_fixture_init(&fixture);
        allocator = (invariant_break_allocator_t){ &fixture, 0u };
        if (st_heap_init(&heap, &fixture.descriptors,
                         (st_runtime_allocator_t){
                             invariant_break_allocate,
                             invariant_break_deallocate,
                             &allocator
                         }) != ST_HEAP_OK)
            _Exit(10);
        if (init_default_context(&context, &heap) != ST_FLOAT_PRIMITIVE_OK)
            _Exit(11);
        (void)st_float_primitive_box_bits(
            &context, UINT64_C(0x3ff0000000000000), &result);
        _Exit(12);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}

static const char *image_directory(void)
{
    if (access("st-image", R_OK) == 0) return "st-image";
    if (access("samples/smalltalk/st-image", R_OK) == 0)
        return "samples/smalltalk/st-image";
    return NULL;
}

static void test_catalog_and_real_image_contract(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_float_primitive_context_t context = {0};
    st_primitive_catalog_t catalog = {0};
    st_source_bundle_t bundle;
    st_primitive_result_t resolution;
    const st_ast_unit_t **units;
    const st_primitive_spec_t *specs;
    const char *image = image_directory();
    size_t count = 0u;
    size_t index;
    size_t float_missing_fallback = 0u;
    size_t remaining_missing_implementation = 0u;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(init_default_context(&context, &heap) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(st_primitive_catalog_init(&catalog,
                                    (st_primitive_allocator_t){0}));
    specs = st_core_primitive_specs(&count);
    CHECK(specs != NULL && count == 19u);
    for (index = 0u; index < count; ++index) {
        CHECK(specs[index].intrinsic_id == ST_INTRINSIC_IDENTITY + index);
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
    }
    specs = st_heap_primitive_specs(&count);
    CHECK(specs != NULL && count == 11u);
    for (index = 0u; index < count; ++index) {
        CHECK(specs[index].intrinsic_id == ST_INTRINSIC_SIZE + index);
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
    }
    specs = st_float_primitive_specs(&count);
    CHECK(specs != NULL && count == 15u);
    for (index = 0u; index < count; ++index) {
        st_value_t receiver = box(&context, bits_of_double(1.0));
        st_value_t argument = box(&context, bits_of_double(2.0));
        st_value_t result = ST_VALUE_INVALID;
        CHECK(specs[index].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL);
        CHECK(specs[index].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
        CHECK(specs[index].runtime_symbol != NULL &&
              specs[index].runtime_symbol_length != 0u);
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
        CHECK(st_float_primitive_execute_internal(
                  &context,
                  (st_float_operation_t)(ST_FLOAT_OPERATION_EQUALS + index),
                  receiver,
                  specs[index].method_arity ? &argument : NULL,
                  specs[index].method_arity, &result) !=
              ST_FLOAT_PRIMITIVE_ERR_UNKNOWN_OPERATION);
    }
    CHECK(specs[0].failure_policy == ST_PRIMITIVE_CANNOT_FAIL);
    for (index = 1u; index + 1u < count; ++index)
        CHECK(specs[index].failure_policy == ST_PRIMITIVE_FALL_THROUGH);
    CHECK(specs[count - 1u].failure_policy == ST_PRIMITIVE_CANNOT_FAIL);
    CHECK(st_primitive_catalog_count(&catalog) == 45u);

    CHECK(image != NULL);
    if (!image) goto done;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL) ==
          ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) goto done;
    CHECK(bundle.count == 50u);
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (!units) {
        st_source_bundle_destroy(&bundle);
        goto done;
    }
    for (index = 0u; index < bundle.count; ++index)
        units[index] = &bundle.files[index].ast;
    st_primitive_result_init(&resolution);
    CHECK(st_primitive_resolve(&resolution, units, bundle.count,
                               &catalog, NULL) == ST_PRIMITIVE_OK);
    CHECK(resolution.binding_count != 0u);
    for (index = 0u; index < resolution.diagnostic_count; ++index) {
        const st_primitive_diagnostic_t *diagnostic =
            &resolution.diagnostics[index];
        bool is_float = diagnostic->requested_name.length >= 5u &&
            memcmp(diagnostic->requested_name.data, "Float", 5u) == 0;
        if (diagnostic->code == ST_PRIMITIVE_DIAG_MISSING_FALLBACK) {
            CHECK(is_float);
            ++float_missing_fallback;
        } else if (diagnostic->code ==
                   ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION) {
            CHECK(!is_float);
            ++remaining_missing_implementation;
        } else {
            CHECK(false);
        }
    }
    CHECK(float_missing_fallback == 0u);
    CHECK(remaining_missing_implementation == resolution.diagnostic_count);
    st_primitive_result_destroy(&resolution);
    free(units);
    st_source_bundle_destroy(&bundle);
done:
    st_primitive_catalog_destroy(&catalog);
    st_float_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

int main(void)
{
    test_context_and_exact_representation();
    test_arithmetic_nan_zero_and_limits();
    test_comparisons();
    test_integer_conversions();
    test_rounding_environment_isolated();
    test_status_arity_and_oom();
    test_post_allocation_invariant_is_fatal();
    test_catalog_and_real_image_contract();
    if (failures != 0u) {
        fprintf(stderr, "Float primitive regression: %u failure(s)\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("Float primitive regression: PASS");
    return EXIT_SUCCESS;
}
