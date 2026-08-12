#include "st_integer_primitives.h"
#include "st_source_bundle.h"

#include <stdbool.h>
#include <stdalign.h>
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
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_NUMBER = 2,
    CLASS_FLOAT = 3,
    CLASS_BOXED_FLOAT64 = 4,
    CLASS_INTEGER = 5,
    CLASS_LARGE_POSITIVE = 6,
    CLASS_METACLASS = 7,
    CLASS_LARGE_NEGATIVE = 8,
    CLASS_OTHER = 9,
    CLASS_COUNT = 9,
    SHAPE_COUNT = 9
};

typedef struct {
    uint64_t raw_bitmap;
    StClassDescriptor class_storage[CLASS_COUNT];
    StShapeDescriptor shape_storage[SHAPE_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[SHAPE_COUNT];
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_float_primitive_context_t floats;
    st_numeric_context_t numeric;
} fixture_t;

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} fault_allocator_t;

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} heap_fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *allocator = user;
    allocator->calls++;
    if (allocator->fail_at != 0u
            && allocator->calls == allocator->fail_at)
        return NULL;
    void *result = malloc(size);
    if (result != NULL) allocator->live++;
    return result;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *allocator = user;
    if (pointer != NULL) {
        CHECK(allocator->live != 0u);
        allocator->live--;
        free(pointer);
    }
}

static void *heap_fault_allocate(void *user, size_t alignment, size_t size)
{
    heap_fault_allocator_t *allocator = user;
    allocator->calls++;
    if (allocator->fail_at != 0u
            && allocator->calls == allocator->fail_at)
        return NULL;
    void *result = aligned_alloc(alignment, size);
    if (result != NULL) allocator->live++;
    return result;
}

static void heap_fault_deallocate(void *user, void *pointer,
                                  size_t alignment, size_t size)
{
    heap_fault_allocator_t *allocator = user;
    (void)alignment;
    (void)size;
    if (pointer != NULL) {
        CHECK(allocator->live != 0u);
        allocator->live--;
        free(pointer);
    }
}

static void descriptor_fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Number", "Float", "BoxedFloat64", "Integer",
        "LargePositiveInteger", "Metaclass", "LargeNegativeInteger",
        "Other"
    };
    static const uint32_t superclasses[CLASS_COUNT] = {
        0u, CLASS_OBJECT, CLASS_NUMBER, CLASS_FLOAT, CLASS_NUMBER,
        CLASS_INTEGER, 0u, CLASS_INTEGER, CLASS_OBJECT
    };

    memset(fixture, 0, sizeof(*fixture));
    for (size_t index = 0u; index < CLASS_COUNT; index++) {
        uint32_t id = (uint32_t)index + 1u;
        fixture->class_storage[index] = (StClassDescriptor) {
            .class_id = id,
            .superclass_id = superclasses[index],
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
        &fixture->raw_bitmap;
    fixture->shape_storage[CLASS_BOXED_FLOAT64 - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->shape_storage[CLASS_LARGE_POSITIVE - 1u].fixed_word_count = 1u;
    fixture->shape_storage[CLASS_LARGE_POSITIVE - 1u]
        .minimum_allocation_size = 32u;
    fixture->shape_storage[CLASS_LARGE_POSITIVE - 1u].indexed_format =
        ST_INDEXED_UINT32;
    fixture->shape_storage[CLASS_LARGE_POSITIVE - 1u]
        .fixed_pointer_bitmap = &fixture->raw_bitmap;
    fixture->shape_storage[CLASS_LARGE_POSITIVE - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->shape_storage[CLASS_LARGE_NEGATIVE - 1u].fixed_word_count = 1u;
    fixture->shape_storage[CLASS_LARGE_NEGATIVE - 1u]
        .minimum_allocation_size = 32u;
    fixture->shape_storage[CLASS_LARGE_NEGATIVE - 1u].indexed_format =
        ST_INDEXED_UINT32;
    fixture->shape_storage[CLASS_LARGE_NEGATIVE - 1u]
        .fixed_pointer_bitmap = &fixture->raw_bitmap;
    fixture->shape_storage[CLASS_LARGE_NEGATIVE - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT,
        .shapes = fixture->shapes,
        .shape_count = SHAPE_COUNT
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static bool fixture_init_full(
    fixture_t *fixture, st_primitive_allocator_t scratch_allocator,
    st_runtime_allocator_t heap_allocator)
{
    st_float_primitive_options_t float_options;
    st_numeric_options_t numeric_options;

    descriptor_fixture_init(fixture);
    if (st_heap_init(
            &fixture->heap, &fixture->descriptors,
            heap_allocator) != ST_HEAP_OK)
        return false;
    float_options = (st_float_primitive_options_t) {
        .heap = &fixture->heap,
        .boxed_float64_class_id = CLASS_BOXED_FLOAT64,
        .boxed_float64_shape_id = CLASS_BOXED_FLOAT64
    };
    if (st_float_primitive_context_init(
            &fixture->floats, &float_options) != ST_FLOAT_PRIMITIVE_OK) {
        st_heap_destroy(&fixture->heap);
        return false;
    }
    numeric_options = (st_numeric_options_t) {
        .heap = &fixture->heap,
        .large_positive_class_id = CLASS_LARGE_POSITIVE,
        .large_positive_shape_id = CLASS_LARGE_POSITIVE,
        .large_negative_class_id = CLASS_LARGE_NEGATIVE,
        .large_negative_shape_id = CLASS_LARGE_NEGATIVE,
        .float_primitives = &fixture->floats,
        .scratch_allocator = scratch_allocator
    };
    if (st_numeric_context_init(
            &fixture->numeric, &numeric_options)
            != ST_INTEGER_PRIMITIVE_OK) {
        st_float_primitive_context_destroy(&fixture->floats);
        st_heap_destroy(&fixture->heap);
        return false;
    }
    return true;
}

static bool fixture_init(fixture_t *fixture,
                         st_primitive_allocator_t scratch_allocator)
{
    return fixture_init_full(
        fixture, scratch_allocator, (st_runtime_allocator_t) {0});
}

static void fixture_destroy(fixture_t *fixture)
{
    st_numeric_context_destroy(&fixture->numeric);
    st_float_primitive_context_destroy(&fixture->floats);
    st_heap_destroy(&fixture->heap);
}

static st_value_t small(int64_t value)
{
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_value_from_small_integer(value, &result));
    return result;
}

static st_value_t integer_from_u64(fixture_t *fixture, bool negative,
                                   uint64_t magnitude)
{
    uint32_t limbs[2] = {
        (uint32_t)magnitude,
        (uint32_t)(magnitude >> 32u)
    };
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_integer_from_sign_magnitude(
        &fixture->numeric, negative, limbs, 2u, &result)
        == ST_INTEGER_PRIMITIVE_OK);
    return result;
}

static bool integer_equals_parts(
    fixture_t *fixture, st_value_t value, bool negative,
    const uint32_t *expected, size_t expected_count)
{
    uint32_t storage[2];
    st_integer_view_t view;
    st_integer_primitive_status_t status = st_integer_view(
        &fixture->numeric, value, storage, &view);

    if (expected == NULL) {
        expected_count = 0u;
    } else {
        while (expected_count != 0u
                && expected[expected_count - 1u] == 0u)
            expected_count--;
    }
    if (status != ST_INTEGER_PRIMITIVE_OK
            || view.negative != (negative && expected_count != 0u)
            || view.limb_count != expected_count)
        return false;
    return expected_count == 0u
        || memcmp(view.limbs, expected,
                  expected_count * sizeof(*expected)) == 0;
}

static bool integer_to_i64(fixture_t *fixture, st_value_t value,
                           int64_t *result_out)
{
    uint32_t storage[2];
    st_integer_view_t view;
    uint64_t magnitude;

    if (st_integer_view(&fixture->numeric, value, storage, &view)
            != ST_INTEGER_PRIMITIVE_OK || view.limb_count > 2u)
        return false;
    magnitude = view.limb_count == 0u ? 0u : view.limbs[0];
    if (view.limb_count == 2u)
        magnitude |= (uint64_t)view.limbs[1] << 32u;
    if (!view.negative) {
        if (magnitude > (uint64_t)INT64_MAX) return false;
        *result_out = (int64_t)magnitude;
    } else {
        if (magnitude > (UINT64_C(1) << 63u)) return false;
        *result_out = magnitude == (UINT64_C(1) << 63u)
            ? INT64_MIN : -(int64_t)magnitude;
    }
    return true;
}

static void test_representation_and_canonicalization(void)
{
    fixture_t fixture;
    st_value_t positive;
    st_value_t negative_small;
    st_value_t negative_large;
    uint32_t two_to_60[2] = { 0u, UINT32_C(0x10000000) };
    uint32_t two_to_60_plus_one[2] = { 1u, UINT32_C(0x10000000) };
    uint32_t with_leading_zeros[4] = {
        0u, UINT32_C(0x10000000), 0u, 0u
    };
    int64_t decoded;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, with_leading_zeros, 4u, &positive)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_kind(positive) == ST_VALUE_OBJECT);
    CHECK(integer_equals_parts(
        &fixture, positive, false, two_to_60, 2u));

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, true, two_to_60, 2u, &negative_small)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(negative_small, &decoded)
          && decoded == ST_SMALL_INTEGER_MIN);

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, true, two_to_60_plus_one, 2u, &negative_large)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_kind(negative_large) == ST_VALUE_OBJECT);
    CHECK(integer_equals_parts(
        &fixture, negative_large, true, two_to_60_plus_one, 2u));
    fixture_destroy(&fixture);
}

static void test_add_subtract_multiply_and_compare(void)
{
    fixture_t fixture;
    st_value_t result;
    st_value_t promoted;
    st_value_t maximum = small(ST_SMALL_INTEGER_MAX);
    uint32_t two_to_60[2] = { 0u, UINT32_C(0x10000000) };
    uint32_t two_to_120[4] = { 0u, 0u, 0u, UINT32_C(0x01000000) };
    uint32_t all_64_bits[2] = { UINT32_MAX, UINT32_MAX };
    uint32_t two_to_64[3] = { 0u, 0u, 1u };
    int64_t decoded;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;

    CHECK(st_integer_binary(
        &fixture.numeric, maximum, ST_INTEGER_BINARY_ADD, small(1),
        &promoted) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, promoted, false, two_to_60, 2u));
    CHECK(st_integer_binary(
        &fixture.numeric, promoted, ST_INTEGER_BINARY_SUBTRACT, small(1),
        &result) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded)
          && decoded == ST_SMALL_INTEGER_MAX);

    st_value_t carry = integer_from_u64(&fixture, false, UINT64_MAX);
    CHECK(st_integer_binary(
        &fixture.numeric, carry, ST_INTEGER_BINARY_ADD, small(1),
        &result) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, result, false, two_to_64, 3u));

    CHECK(st_integer_binary(
        &fixture.numeric, promoted, ST_INTEGER_BINARY_MULTIPLY, promoted,
        &result) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, result, false, two_to_120, 4u));

    st_value_t negative = integer_from_u64(
        &fixture, true, (UINT64_C(1) << 60u) + 1u);
    CHECK(st_integer_compare(
        &fixture.numeric, negative, promoted, &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded) && decoded == -1);
    CHECK(st_integer_compare(
        &fixture.numeric, carry,
        integer_from_u64(&fixture, false, UINT64_MAX), &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded) && decoded == 0);

    CHECK(integer_equals_parts(
        &fixture, carry, false, all_64_bits, 2u));
    fixture_destroy(&fixture);
}

static void check_division_case(fixture_t *fixture, int64_t left,
                                int64_t right, int64_t quotient,
                                int64_t remainder)
{
    st_value_t q;
    st_value_t r;
    int64_t actual;

    CHECK(st_integer_binary(
        &fixture->numeric, small(left), ST_INTEGER_BINARY_FLOOR_DIVIDE,
        small(right), &q) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_to_i64(fixture, q, &actual) && actual == quotient);
    CHECK(st_integer_binary(
        &fixture->numeric, small(left), ST_INTEGER_BINARY_MODULO,
        small(right), &r) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_to_i64(fixture, r, &actual) && actual == remainder);
    CHECK(left == quotient * right + remainder);
}

static void test_floor_division_and_modulo(void)
{
    fixture_t fixture;
    st_value_t result = small(123);
    uint32_t dividend_limbs[4] = { 17u, 0u, 0u, UINT32_C(0x01000000) };
    uint32_t divisor_limbs[2] = { 3u, UINT32_C(0x10000000) };
    st_value_t dividend;
    st_value_t divisor;
    st_value_t quotient;
    st_value_t remainder;
    st_value_t product;
    st_value_t rebuilt;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;

    check_division_case(&fixture, 7, 3, 2, 1);
    check_division_case(&fixture, -7, 3, -3, 2);
    check_division_case(&fixture, 7, -3, -3, -2);
    check_division_case(&fixture, -7, -3, 2, -1);
    check_division_case(&fixture, 1, -3, -1, -2);

    CHECK(st_integer_binary(
        &fixture.numeric, small(1), ST_INTEGER_BINARY_FLOOR_DIVIDE,
        small(0), &result) == ST_INTEGER_PRIMITIVE_ERR_DIVISION_BY_ZERO);
    CHECK(result == ST_VALUE_INVALID);

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, dividend_limbs, 4u, &dividend)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, true, divisor_limbs, 2u, &divisor)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_binary(
        &fixture.numeric, dividend, ST_INTEGER_BINARY_FLOOR_DIVIDE,
        divisor, &quotient) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_binary(
        &fixture.numeric, dividend, ST_INTEGER_BINARY_MODULO,
        divisor, &remainder) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_binary(
        &fixture.numeric, quotient, ST_INTEGER_BINARY_MULTIPLY,
        divisor, &product) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_binary(
        &fixture.numeric, product, ST_INTEGER_BINARY_ADD,
        remainder, &rebuilt) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_compare(
        &fixture.numeric, dividend, rebuilt, &result)
        == ST_INTEGER_PRIMITIVE_OK);
    int64_t comparison;
    CHECK(st_value_to_small_integer(result, &comparison) && comparison == 0);
    fixture_destroy(&fixture);
}

static void test_shifts(void)
{
    fixture_t fixture;
    uint32_t two_to_60[2] = { 0u, UINT32_C(0x10000000) };
    uint32_t two_to_124[4] = { 0u, 0u, 0u, UINT32_C(0x10000000) };
    uint32_t huge_count_limbs[3] = { 0u, 0u, 1u };
    st_value_t value;
    st_value_t result;
    st_value_t huge_positive;
    st_value_t huge_negative;
    int64_t decoded;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, two_to_60, 2u, &value)
        == ST_INTEGER_PRIMITIVE_OK);

    CHECK(st_integer_shift(
        &fixture.numeric, value, small(64), &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, result, false, two_to_124, 4u));
    CHECK(st_integer_shift(
        &fixture.numeric, result, small(-64), &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, result, false, two_to_60, 2u));

    st_value_t negative = integer_from_u64(
        &fixture, true, (UINT64_C(1) << 60u) + 1u);
    CHECK(st_integer_shift(
        &fixture.numeric, negative, small(-60), &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded) && decoded == -2);

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, huge_count_limbs, 3u, &huge_positive)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, true, huge_count_limbs, 3u, &huge_negative)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_shift(
        &fixture.numeric, value, huge_positive, &result)
        == ST_INTEGER_PRIMITIVE_ERR_SHIFT_OUT_OF_RANGE);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(st_integer_shift(
        &fixture.numeric, value, huge_negative, &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded) && decoded == 0);
    CHECK(st_integer_shift(
        &fixture.numeric, negative, huge_negative, &result)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &decoded) && decoded == -1);
    fixture_destroy(&fixture);
}

static uint64_t float_bits(fixture_t *fixture, st_value_t value)
{
    uint64_t result = 0u;
    CHECK(st_float_primitive_unbox_bits(
        &fixture->floats, value, &result) == ST_FLOAT_PRIMITIVE_OK);
    return result;
}

static void test_binary64_rounding(void)
{
    fixture_t fixture;
    st_value_t integer;
    st_value_t boxed;
    uint32_t two_to_1024[33] = {0};

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;

    integer = integer_from_u64(
        &fixture, false, (UINT64_C(1) << 53u) + 1u);
    CHECK(st_integer_as_float(&fixture.numeric, integer, &boxed)
          == ST_INTEGER_PRIMITIVE_OK);
    CHECK(float_bits(&fixture, boxed) == UINT64_C(0x4340000000000000));

    integer = integer_from_u64(
        &fixture, false, (UINT64_C(1) << 53u) + 3u);
    CHECK(st_integer_as_float(&fixture.numeric, integer, &boxed)
          == ST_INTEGER_PRIMITIVE_OK);
    CHECK(float_bits(&fixture, boxed) == UINT64_C(0x4340000000000002));

    integer = integer_from_u64(
        &fixture, true, (UINT64_C(1) << 60u) + 1u);
    CHECK(st_integer_as_float(&fixture.numeric, integer, &boxed)
          == ST_INTEGER_PRIMITIVE_OK);
    CHECK(float_bits(&fixture, boxed) == UINT64_C(0xc3b0000000000000));

    two_to_1024[32] = 1u;
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, two_to_1024, 33u, &integer)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_as_float(&fixture.numeric, integer, &boxed)
          == ST_INTEGER_PRIMITIVE_OK);
    CHECK(float_bits(&fixture, boxed) == UINT64_C(0x7ff0000000000000));
    fixture_destroy(&fixture);
}

static void check_binary64_to_i64(
    fixture_t *fixture, uint64_t bits, st_integer_rounding_t rounding,
    int64_t expected)
{
    st_value_t integer;
    int64_t actual;

    CHECK(st_integer_from_binary64_bits(
        &fixture->numeric, bits, rounding, &integer)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_to_i64(fixture, integer, &actual) && actual == expected);
}

static void test_binary64_to_integer(void)
{
    fixture_t fixture;
    st_value_t integer = small(99);
    uint32_t two_to_60[2] = { 0u, UINT32_C(0x10000000) };
    uint32_t maximum_finite[32] = {0};

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;

    check_binary64_to_i64(
        &fixture, UINT64_C(0x0000000000000000),
        ST_INTEGER_ROUND_FLOOR, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x8000000000000000),
        ST_INTEGER_ROUND_CEILING, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x3ff8000000000000),
        ST_INTEGER_ROUND_TOWARD_ZERO, 1);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x3ff8000000000000),
        ST_INTEGER_ROUND_FLOOR, 1);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x3ff8000000000000),
        ST_INTEGER_ROUND_CEILING, 2);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x3ff8000000000000),
        ST_INTEGER_ROUND_NEAREST_TIES_AWAY, 2);
    check_binary64_to_i64(
        &fixture, UINT64_C(0xbff8000000000000),
        ST_INTEGER_ROUND_TOWARD_ZERO, -1);
    check_binary64_to_i64(
        &fixture, UINT64_C(0xbff8000000000000),
        ST_INTEGER_ROUND_FLOOR, -2);
    check_binary64_to_i64(
        &fixture, UINT64_C(0xbff8000000000000),
        ST_INTEGER_ROUND_CEILING, -1);
    check_binary64_to_i64(
        &fixture, UINT64_C(0xbff8000000000000),
        ST_INTEGER_ROUND_NEAREST_TIES_AWAY, -2);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x4004000000000000),
        ST_INTEGER_ROUND_NEAREST_TIES_AWAY, 3);

    check_binary64_to_i64(
        &fixture, UINT64_C(1), ST_INTEGER_ROUND_TOWARD_ZERO, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(1), ST_INTEGER_ROUND_FLOOR, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(1), ST_INTEGER_ROUND_CEILING, 1);
    check_binary64_to_i64(
        &fixture, UINT64_C(1), ST_INTEGER_ROUND_NEAREST_TIES_AWAY, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x8000000000000001),
        ST_INTEGER_ROUND_TOWARD_ZERO, 0);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x8000000000000001),
        ST_INTEGER_ROUND_FLOOR, -1);
    check_binary64_to_i64(
        &fixture, UINT64_C(0x8000000000000001),
        ST_INTEGER_ROUND_CEILING, 0);

    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0x43b0000000000000),
        ST_INTEGER_ROUND_TOWARD_ZERO, &integer)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, integer, false, two_to_60, 2u));
    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0xc3b0000000000000),
        ST_INTEGER_ROUND_TOWARD_ZERO, &integer)
        == ST_INTEGER_PRIMITIVE_OK);
    int64_t decoded;
    CHECK(st_value_to_small_integer(integer, &decoded)
          && decoded == ST_SMALL_INTEGER_MIN);

    maximum_finite[30] = UINT32_C(0xfffff800);
    maximum_finite[31] = UINT32_MAX;
    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0x7fefffffffffffff),
        ST_INTEGER_ROUND_TOWARD_ZERO, &integer)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(integer_equals_parts(
        &fixture, integer, false, maximum_finite, 32u));

    integer = small(99);
    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0x7ff0000000000000),
        ST_INTEGER_ROUND_TOWARD_ZERO, &integer)
        == ST_INTEGER_PRIMITIVE_ERR_NON_FINITE);
    CHECK(integer == ST_VALUE_INVALID);
    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0x7ff8000000000001),
        ST_INTEGER_ROUND_TOWARD_ZERO, &integer)
        == ST_INTEGER_PRIMITIVE_ERR_NON_FINITE);
    CHECK(integer == ST_VALUE_INVALID);
    CHECK(st_integer_from_binary64_bits(
        &fixture.numeric, UINT64_C(0x3ff0000000000000),
        (st_integer_rounding_t)99, &integer)
        == ST_INTEGER_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(integer == ST_VALUE_INVALID);
    fixture_destroy(&fixture);
}

static int64_t integer_hash_value(fixture_t *fixture, st_value_t value)
{
    st_value_t hash = ST_VALUE_INVALID;
    int64_t decoded = -1;

    CHECK(st_integer_hash(&fixture->numeric, value, &hash)
          == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(hash, &decoded));
    CHECK(decoded >= 0 && decoded <= ST_SMALL_INTEGER_MAX);
    return decoded;
}

static void test_integer_hash(void)
{
    fixture_t fixture;
    uint32_t magnitude[3] = {
        UINT32_C(0x89abcdef), UINT32_C(0x01234567), UINT32_C(0x55aa55aa)
    };
    uint32_t small_maximum[2] = {
        UINT32_MAX, UINT32_C(0x0fffffff)
    };
    st_value_t first;
    st_value_t second;
    st_value_t negative;
    st_value_t demoted;
    st_value_t roots[3];
    st_heap_collection_stats_t stats;
    int64_t first_hash;
    int64_t second_hash;
    int64_t negative_hash;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, magnitude, 3u, &first)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, magnitude, 3u, &second)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, true, magnitude, 3u, &negative)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(first != second);

    first_hash = integer_hash_value(&fixture, first);
    second_hash = integer_hash_value(&fixture, second);
    negative_hash = integer_hash_value(&fixture, negative);
    CHECK(first_hash == second_hash);
    CHECK(first_hash != negative_hash);

    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, small_maximum, 2u, &demoted)
        == ST_INTEGER_PRIMITIVE_OK);
    CHECK(demoted == small(ST_SMALL_INTEGER_MAX));
    CHECK(integer_hash_value(&fixture, demoted)
          == integer_hash_value(&fixture, small(ST_SMALL_INTEGER_MAX)));
    CHECK(integer_hash_value(&fixture, small(0))
          == integer_hash_value(&fixture, small(0)));

    roots[0] = first;
    roots[1] = second;
    roots[2] = negative;
    CHECK(st_heap_collect(
        &fixture.heap, NULL, roots, 3u, &stats) == ST_HEAP_OK);
    CHECK(integer_hash_value(&fixture, roots[0]) == first_hash);
    CHECK(integer_hash_value(&fixture, roots[1]) == second_hash);
    CHECK(integer_hash_value(&fixture, roots[2]) == negative_hash);
    fixture_destroy(&fixture);
}

static uint64_t random_state = UINT64_C(0x6a09e667f3bcc909);

static uint64_t random_u64(void)
{
    random_state ^= random_state << 13u;
    random_state ^= random_state >> 7u;
    random_state ^= random_state << 17u;
    return random_state;
}

static int64_t floor_divide_i64(int64_t left, int64_t right)
{
    int64_t quotient = left / right;
    int64_t remainder = left % right;
    if (remainder != 0 && ((remainder < 0) != (right < 0))) quotient--;
    return quotient;
}

static int64_t modulo_i64(int64_t left, int64_t right)
{
    int64_t remainder = left % right;
    if (remainder != 0 && ((remainder < 0) != (right < 0)))
        remainder += right;
    return remainder;
}

static void test_differential_i64_oracle(void)
{
    fixture_t fixture;

    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie == 0u) return;
    for (size_t iteration = 0u; iteration < 10000u; iteration++) {
        int64_t left = (int64_t)(random_u64() & ((UINT64_C(1) << 60u) - 1u));
        int64_t right = (int64_t)(random_u64() & ((UINT64_C(1) << 60u) - 1u));
        if ((random_u64() & 1u) != 0u) left = -left;
        if ((random_u64() & 1u) != 0u) right = -right;
        st_value_t result;
        int64_t actual;

        CHECK(st_integer_binary(
            &fixture.numeric, small(left), ST_INTEGER_BINARY_ADD,
            small(right), &result) == ST_INTEGER_PRIMITIVE_OK);
        CHECK(integer_to_i64(&fixture, result, &actual)
              && actual == left + right);
        CHECK(st_integer_binary(
            &fixture.numeric, small(left), ST_INTEGER_BINARY_SUBTRACT,
            small(right), &result) == ST_INTEGER_PRIMITIVE_OK);
        CHECK(integer_to_i64(&fixture, result, &actual)
              && actual == left - right);
        if (right != 0) {
            CHECK(st_integer_binary(
                &fixture.numeric, small(left),
                ST_INTEGER_BINARY_FLOOR_DIVIDE, small(right), &result)
                == ST_INTEGER_PRIMITIVE_OK);
            CHECK(integer_to_i64(&fixture, result, &actual)
                  && actual == floor_divide_i64(left, right));
            CHECK(st_integer_binary(
                &fixture.numeric, small(left), ST_INTEGER_BINARY_MODULO,
                small(right), &result) == ST_INTEGER_PRIMITIVE_OK);
            CHECK(integer_to_i64(&fixture, result, &actual)
                  && actual == modulo_i64(left, right));
        }
    }
    fixture_destroy(&fixture);
}

static void test_oom_malformed_and_gc(void)
{
    fixture_t fixture;
    fault_allocator_t allocator = {0};
    st_primitive_allocator_t scratch = {
        .allocate = fault_allocate,
        .deallocate = fault_deallocate,
        .user = &allocator
    };
    st_value_t value;
    st_value_t result = small(7);
    st_object_view_t object;
    uint64_t metadata;
    st_heap_collection_stats_t stats;
    uint32_t magnitude[2] = { 1u, UINT32_C(0x10000000) };
    alignas(8) unsigned char foreign_storage[32] = {0};
    st_value_t foreign;
    st_value_t interior;
    st_value_t other;

    CHECK(fixture_init(&fixture, scratch));
    if (fixture.numeric.abi_cookie == 0u) return;
    CHECK(st_integer_from_sign_magnitude(
        &fixture.numeric, false, magnitude, 2u, &value)
        == ST_INTEGER_PRIMITIVE_OK);

    allocator.fail_at = allocator.calls + 1u;
    CHECK(st_integer_binary(
        &fixture.numeric, value, ST_INTEGER_BINARY_ADD, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID && allocator.live == 0u);
    allocator.fail_at = 0u;

    CHECK(st_heap_object_view(&fixture.heap, value, &object) == ST_HEAP_OK);
    CHECK(st_value_from_object(foreign_storage, &foreign));
    CHECK(st_integer_compare(
        &fixture.numeric, foreign, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(st_value_from_object(
        (unsigned char *)object.object + 8u, &interior));
    CHECK(st_integer_compare(
        &fixture.numeric, interior, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER);
    CHECK(st_heap_allocate(
        &fixture.heap, CLASS_OTHER, CLASS_OTHER, 0u, 0u, 0u, &other)
        == ST_HEAP_OK);
    CHECK(st_integer_compare(
        &fixture.numeric, other, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_TYPE_MISMATCH);
    memcpy(&metadata, object.fixed_words, sizeof(metadata));
    memset(object.fixed_words, 0, sizeof(metadata));
    CHECK(st_integer_compare(
        &fixture.numeric, value, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_BAD_OBJECT);
    CHECK(result == ST_VALUE_INVALID);
    memcpy(object.fixed_words, &metadata, sizeof(metadata));

    uint32_t saved_high = ((uint32_t *)object.indexed_elements)[1];
    ((uint32_t *)object.indexed_elements)[1] = 0u;
    CHECK(st_integer_compare(
        &fixture.numeric, value, value, &result)
        == ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL);
    ((uint32_t *)object.indexed_elements)[1] = saved_high;

    CHECK(st_heap_collect(
        &fixture.heap, NULL, &value, 1u, &stats) == ST_HEAP_OK);
    CHECK(st_heap_contains(&fixture.heap, value));
    CHECK(st_heap_collect(
        &fixture.heap, NULL, NULL, 0u, &stats) == ST_HEAP_OK);
    CHECK(!st_heap_contains(&fixture.heap, value));
    CHECK(st_integer_compare(
        &fixture.numeric, value, small(0), &result)
        == ST_INTEGER_PRIMITIVE_ERR_NOT_MEMBER);
    CHECK(allocator.live == 0u);
    fixture_destroy(&fixture);
}

static void test_heap_oom_transaction(void)
{
    fixture_t fixture;
    heap_fault_allocator_t allocator = {0};
    st_runtime_allocator_t heap_allocator = {
        .allocate = heap_fault_allocate,
        .deallocate = heap_fault_deallocate,
        .user = &allocator
    };
    st_value_t operand;
    st_value_t result = small(99);
    size_t objects_before;

    CHECK(fixture_init_full(
        &fixture, (st_primitive_allocator_t) {0}, heap_allocator));
    if (fixture.numeric.abi_cookie == 0u) return;
    operand = integer_from_u64(
        &fixture, false, UINT64_C(1) << 60u);
    objects_before = st_heap_object_count(&fixture.heap);
    allocator.fail_at = allocator.calls + 1u;
    CHECK(st_integer_binary(
        &fixture.numeric, operand, ST_INTEGER_BINARY_ADD,
        operand, &result) == ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(st_heap_object_count(&fixture.heap) == objects_before);
    allocator.fail_at = 0u;
    fixture_destroy(&fixture);
    CHECK(allocator.live == 0u);
}

static void test_specs_and_context_validation(void)
{
    fixture_t fixture;
    size_t count = 0u;
    const st_primitive_spec_t *specs = st_integer_primitive_specs(&count);

    CHECK(count == 6u && specs != NULL);
    for (size_t index = 0u; index < count; index++) {
        CHECK(specs[index].implementation_kind
              == ST_PRIMITIVE_RUNTIME_SYMBOL);
        CHECK(specs[index].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
        CHECK(specs[index].failure_policy
              == (index == count - 1u
                  ? ST_PRIMITIVE_CANNOT_FAIL
                  : ST_PRIMITIVE_FALL_THROUGH));
        CHECK(specs[index].runtime_symbol != NULL
              && specs[index].runtime_symbol_length != 0u);
    }
    CHECK(fixture_init(&fixture, (st_primitive_allocator_t) {0}));
    if (fixture.numeric.abi_cookie != 0u) {
        CHECK(st_float_primitive_context_heap(&fixture.floats)
              == &fixture.heap);
        CHECK(strcmp(st_integer_primitive_status_string(
            ST_INTEGER_PRIMITIVE_ERR_NON_CANONICAL),
            "non-canonical LargeInteger") == 0);
        fixture_destroy(&fixture);
    }
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static void test_real_image_bindings(void)
{
    const char *image = first_existing(
        "st-image", "samples/smalltalk/st-image");
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_ast_unit_t **units;
    size_t spec_count;
    const st_primitive_spec_t *specs;

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
    for (size_t index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;
    CHECK(st_primitive_catalog_init(
        &catalog, (st_primitive_allocator_t) {0}));
    specs = st_integer_primitive_specs(&spec_count);
    for (size_t index = 0u; index < spec_count; index++)
        CHECK(st_primitive_catalog_register(
            &catalog, &specs[index], NULL) == ST_PRIMITIVE_OK);
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(
        &result, units, bundle.count, &catalog, NULL) == ST_PRIMITIVE_OK);
    CHECK(result.binding_count == 6u);
    for (size_t index = 0u; index < result.binding_count; index++) {
        CHECK(result.bindings[index].primitive != NULL);
        CHECK(result.bindings[index].primitive->implementation_kind
              == ST_PRIMITIVE_RUNTIME_SYMBOL);
    }
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

static bool parse_hex_integer(fixture_t *fixture, const char *sign_text,
                              const char *hex, st_value_t *value_out)
{
    size_t length = strlen(hex);
    size_t count = (length + 7u) / 8u;
    uint32_t *limbs = count == 0u ? NULL : calloc(count, sizeof(*limbs));
    bool negative = strcmp(sign_text, "-") == 0;
    bool ok = count == 0u || limbs != NULL;

    for (size_t index = 0u; ok && index < count; index++) {
        size_t end = length - index * 8u;
        size_t start = end > 8u ? end - 8u : 0u;
        size_t digits = end - start;
        char chunk[9];
        char *tail;

        memcpy(chunk, hex + start, digits);
        chunk[digits] = '\0';
        unsigned long parsed = strtoul(chunk, &tail, 16);
        ok = *tail == '\0' && parsed <= UINT32_MAX;
        if (ok) limbs[index] = (uint32_t)parsed;
    }
    if (ok) {
        ok = st_integer_from_sign_magnitude(
            &fixture->numeric, negative, limbs, count, value_out)
            == ST_INTEGER_PRIMITIVE_OK;
    }
    free(limbs);
    return ok;
}

static bool print_integer(fixture_t *fixture, st_value_t value)
{
    uint32_t small_storage[2];
    st_integer_view_t view;

    if (st_integer_view(
            &fixture->numeric, value, small_storage, &view)
            != ST_INTEGER_PRIMITIVE_OK)
        return false;
    if (printf("ok\t%s\t", view.negative ? "-" : "+") < 0)
        return false;
    if (view.limb_count == 0u) {
        if (putchar('0') == EOF) return false;
    } else {
        if (printf("%x", view.limbs[view.limb_count - 1u]) < 0)
            return false;
        for (size_t index = view.limb_count - 1u; index != 0u; index--)
            if (printf("%08x", view.limbs[index - 1u]) < 0)
                return false;
    }
    return putchar('\n') != EOF && fflush(stdout) == 0;
}

static bool parse_u64_hex(const char *text, uint64_t *value_out)
{
    uint64_t value = 0u;
    size_t length;

    if (text == NULL || value_out == NULL
            || (length = strlen(text)) == 0u || length > 16u)
        return false;
    for (size_t index = 0u; index < length; index++) {
        unsigned digit;
        unsigned char character = (unsigned char)text[index];
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10u;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10u;
        } else {
            return false;
        }
        value = (value << 4u) | digit;
    }
    *value_out = value;
    return true;
}

static int run_oracle_protocol(void)
{
    fixture_t fixture;
    char line[5000];

    if (!fixture_init(&fixture, (st_primitive_allocator_t) {0})) return 2;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *operation = strtok(line, "\t\r\n");
        char *left_sign = strtok(NULL, "\t\r\n");
        char *left_hex = strtok(NULL, "\t\r\n");
        char *right_sign = strtok(NULL, "\t\r\n");
        char *right_hex = strtok(NULL, "\t\r\n");
        st_value_t left;
        st_value_t right;
        st_value_t result = ST_VALUE_INVALID;
        st_integer_primitive_status_t status;

        if (operation != NULL
                && (strcmp(operation, "ftrunc") == 0
                    || strcmp(operation, "ffloor") == 0
                    || strcmp(operation, "fceil") == 0
                    || strcmp(operation, "fround") == 0)) {
            uint64_t bits;
            st_integer_rounding_t rounding = ST_INTEGER_ROUND_TOWARD_ZERO;

            if (left_sign == NULL || left_hex == NULL
                    || strcmp(left_sign, "+") != 0
                    || !parse_u64_hex(left_hex, &bits)) {
                fixture_destroy(&fixture);
                return 3;
            }
            if (strcmp(operation, "ffloor") == 0)
                rounding = ST_INTEGER_ROUND_FLOOR;
            else if (strcmp(operation, "fceil") == 0)
                rounding = ST_INTEGER_ROUND_CEILING;
            else if (strcmp(operation, "fround") == 0)
                rounding = ST_INTEGER_ROUND_NEAREST_TIES_AWAY;
            status = st_integer_from_binary64_bits(
                &fixture.numeric, bits, rounding, &result);
            if (status != ST_INTEGER_PRIMITIVE_OK
                    || !print_integer(&fixture, result)) {
                fixture_destroy(&fixture);
                return 5;
            }
            continue;
        }

        if (operation == NULL || left_sign == NULL || left_hex == NULL
                || right_sign == NULL || right_hex == NULL
                || !parse_hex_integer(
                    &fixture, left_sign, left_hex, &left)
                || !parse_hex_integer(
                    &fixture, right_sign, right_hex, &right)) {
            fixture_destroy(&fixture);
            return 3;
        }
        if (strcmp(operation, "add") == 0) {
            status = st_integer_binary(
                &fixture.numeric, left, ST_INTEGER_BINARY_ADD,
                right, &result);
        } else if (strcmp(operation, "sub") == 0) {
            status = st_integer_binary(
                &fixture.numeric, left, ST_INTEGER_BINARY_SUBTRACT,
                right, &result);
        } else if (strcmp(operation, "mul") == 0) {
            status = st_integer_binary(
                &fixture.numeric, left, ST_INTEGER_BINARY_MULTIPLY,
                right, &result);
        } else if (strcmp(operation, "div") == 0) {
            status = st_integer_binary(
                &fixture.numeric, left, ST_INTEGER_BINARY_FLOOR_DIVIDE,
                right, &result);
        } else if (strcmp(operation, "mod") == 0) {
            status = st_integer_binary(
                &fixture.numeric, left, ST_INTEGER_BINARY_MODULO,
                right, &result);
        } else if (strcmp(operation, "cmp") == 0) {
            status = st_integer_compare(
                &fixture.numeric, left, right, &result);
        } else if (strcmp(operation, "shift") == 0) {
            status = st_integer_shift(
                &fixture.numeric, left, right, &result);
        } else if (strcmp(operation, "asfloat") == 0) {
            uint64_t bits;
            status = st_integer_as_float(
                &fixture.numeric, left, &result);
            if (status != ST_INTEGER_PRIMITIVE_OK
                    || st_float_primitive_unbox_bits(
                        &fixture.floats, result, &bits)
                        != ST_FLOAT_PRIMITIVE_OK
                    || printf("bits\t%016llx\n",
                              (unsigned long long)bits) < 0
                    || fflush(stdout) != 0) {
                fixture_destroy(&fixture);
                return 5;
            }
            continue;
        } else {
            fixture_destroy(&fixture);
            return 4;
        }
        if (status != ST_INTEGER_PRIMITIVE_OK
                || !print_integer(&fixture, result)) {
            fixture_destroy(&fixture);
            return 5;
        }
    }
    fixture_destroy(&fixture);
    return ferror(stdin) ? 6 : 0;
}

static void test_python_big_integer_oracle(const char *executable)
{
    const char *script = first_existing(
        "tests/integer_differential_oracle.py",
        "samples/smalltalk/tests/integer_differential_oracle.py");
    pid_t child;
    int status;

    CHECK(script != NULL && executable != NULL);
    if (script == NULL || executable == NULL) return;
    child = fork();
    CHECK(child >= 0);
    if (child < 0) return;
    if (child == 0) {
        execlp("python3", "python3", script, executable, (char *)NULL);
        _exit(127);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 && strcmp(arguments[1], "--oracle") == 0)
        return run_oracle_protocol();
    if (argument_count != 1) return 2;
    test_representation_and_canonicalization();
    test_add_subtract_multiply_and_compare();
    test_floor_division_and_modulo();
    test_shifts();
    test_binary64_rounding();
    test_binary64_to_integer();
    test_integer_hash();
    test_differential_i64_oracle();
    test_oom_malformed_and_gc();
    test_heap_oom_transaction();
    test_specs_and_context_validation();
    test_real_image_bindings();
    test_python_big_integer_oracle(arguments[0]);

    if (failures != 0u) {
        fprintf(stderr, "%u integer primitive checks failed\n", failures);
        return 1;
    }
    puts("integer primitive tests passed");
    return 0;
}
