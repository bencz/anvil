#include "st_float_primitive_bridge.h"
#include "st_float_primitives.h"
#include "st_integer_primitives.h"
#include "st_send_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_NUMBER,
    CLASS_FLOAT,
    CLASS_BOXED_FLOAT64,
    CLASS_INTEGER,
    CLASS_LARGE_POSITIVE,
    CLASS_LARGE_NEGATIVE,
    CLASS_METACLASS,
    CLASS_COUNT
};

typedef struct {
    uint64_t raw_bitmap;
    StClassDescriptor class_storage[CLASS_COUNT - 1u];
    StShapeDescriptor shape_storage[CLASS_COUNT - 1u];
    const StClassDescriptor *classes[CLASS_COUNT - 1u];
    const StShapeDescriptor *shapes[CLASS_COUNT - 1u];
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_float_primitive_context_t floats;
    st_numeric_context_t numeric;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    StMethodDescriptor method;
    StFrame frame;
} fixture_t;

typedef struct {
    size_t allocation_calls;
    bool fail;
} scratch_fault_t;

static void *scratch_allocate(void *user, size_t size)
{
    scratch_fault_t *fault = user;
    fault->allocation_calls++;
    return fault->fail ? NULL : malloc(size);
}

static void scratch_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void configure_raw_shape(
    fixture_t *fixture, uint32_t id, st_indexed_format_t indexed_format)
{
    StShapeDescriptor *shape = &fixture->shape_storage[id - 1u];
    shape->fixed_word_count = 1u;
    shape->minimum_allocation_size = 32u;
    shape->fixed_pointer_bitmap = &fixture->raw_bitmap;
    shape->fixed_pointer_bitmap_word_count = 1u;
    shape->indexed_format = indexed_format;
}

static bool fixture_init_with_scratch(
    fixture_t *fixture, st_primitive_allocator_t scratch_allocator)
{
    static const char *const names[CLASS_COUNT - 1u] = {
        "Object", "UndefinedObject", "False", "True", "SmallInteger",
        "Character", "Number", "Float", "BoxedFloat64", "Integer",
        "LargePositiveInteger", "LargeNegativeInteger", "Metaclass"
    };
    static const uint32_t superclasses[CLASS_COUNT - 1u] = {
        0u, CLASS_OBJECT, CLASS_OBJECT, CLASS_OBJECT, CLASS_INTEGER,
        CLASS_OBJECT, CLASS_OBJECT, CLASS_NUMBER, CLASS_FLOAT, CLASS_NUMBER,
        CLASS_INTEGER, CLASS_INTEGER, 0u
    };
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };

    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t id = 1u; id < CLASS_COUNT; id++) {
        size_t index = (size_t)id - 1u;
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
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    configure_raw_shape(fixture, CLASS_BOXED_FLOAT64, ST_INDEXED_NONE);
    configure_raw_shape(fixture, CLASS_LARGE_POSITIVE, ST_INDEXED_UINT32);
    configure_raw_shape(fixture, CLASS_LARGE_NEGATIVE, ST_INDEXED_UINT32);
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT - 1u,
        .shapes = fixture->shapes,
        .shape_count = CLASS_COUNT - 1u
    };
    fixture->method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_FLOAT,
        .arity = 1u
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
                != ST_RUNTIME_OK
            || st_heap_init(
                &fixture->heap, &fixture->descriptors,
                (st_runtime_allocator_t) {0}) != ST_HEAP_OK
            || st_float_primitive_context_init(
                &fixture->floats, &(st_float_primitive_options_t) {
                    .heap = &fixture->heap,
                    .boxed_float64_class_id = CLASS_BOXED_FLOAT64,
                    .boxed_float64_shape_id = CLASS_BOXED_FLOAT64
                }) != ST_FLOAT_PRIMITIVE_OK
            || st_numeric_context_init(
                &fixture->numeric, &(st_numeric_options_t) {
                    .heap = &fixture->heap,
                    .large_positive_class_id = CLASS_LARGE_POSITIVE,
                    .large_positive_shape_id = CLASS_LARGE_POSITIVE,
                    .large_negative_class_id = CLASS_LARGE_NEGATIVE,
                    .large_negative_shape_id = CLASS_LARGE_NEGATIVE,
                    .float_primitives = &fixture->floats,
                    .scratch_allocator = scratch_allocator
                }) != ST_INTEGER_PRIMITIVE_OK
            || st_lookup_context_init(
                &fixture->lookup, &fixture->descriptors,
                (st_lookup_allocator_t) {0}) != ST_LOOKUP_FOUND
            || !st_aot_thread_init(
                &fixture->thread, &fixture->lookup, immediate,
                NULL, NULL, NULL, NULL, NULL, NULL, NULL))
        return false;
    fixture->frame = (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->method,
        .argc = 1u
    };
    return true;
}

static bool fixture_init(fixture_t *fixture)
{
    return fixture_init_with_scratch(
        fixture, (st_primitive_allocator_t) {0});
}

static void fixture_destroy(fixture_t *fixture)
{
    if (fixture->thread.numeric != NULL)
        CHECK(st_aot_thread_numeric_detach(
            &fixture->thread, &fixture->numeric));
    st_aot_thread_destroy(&fixture->thread);
    st_lookup_context_destroy(&fixture->lookup);
    st_numeric_context_destroy(&fixture->numeric);
    st_float_primitive_context_destroy(&fixture->floats);
    st_heap_destroy(&fixture->heap);
}

static st_value_t box(fixture_t *fixture, uint64_t bits)
{
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_float_primitive_box_bits(&fixture->floats, bits, &result) ==
          ST_FLOAT_PRIMITIVE_OK);
    return result;
}

static void test_arithmetic_hash_and_validation(void)
{
    fixture_t fixture;
    st_value_t one;
    st_value_t two;
    st_value_t result = st_value_true();
    st_value_t argument;
    uint64_t bits = 0u;
    uint32_t detail = 99u;

    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;
    one = box(&fixture, UINT64_C(0x3ff0000000000000));
    two = box(&fixture, UINT64_C(0x4000000000000000));
    argument = two;
    fixture.frame.receiver = one;
    fixture.frame.argv = &argument;

    CHECK(st_aot_float_add_primitive_execute(
        &fixture.frame, one, &argument, 1u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_numeric_attach(&fixture.thread, &fixture.numeric));
    CHECK(st_aot_float_add_primitive_execute(
        &fixture.frame, one, &argument, 1u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_OK);
    CHECK(detail == 0u);
    CHECK(st_float_primitive_unbox_bits(&fixture.floats, result, &bits) ==
          ST_FLOAT_PRIMITIVE_OK);
    CHECK(bits == UINT64_C(0x4008000000000000));

    result = st_value_true();
    CHECK(st_aot_float_add_primitive_execute(
        &fixture.frame, one, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);

    fixture.method.arity = 0u;
    fixture.frame.argc = 0u;
    fixture.frame.argv = NULL;
    CHECK(st_aot_float_hash_primitive_execute(
        &fixture.frame, box(&fixture, 0u), NULL, 0u,
        &result, &detail) == ST_FLOAT_PRIMITIVE_OK);
    st_value_t positive_zero_hash = result;
    CHECK(st_aot_float_hash_primitive_execute(
        &fixture.frame, box(&fixture, UINT64_C(0x8000000000000000)),
        NULL, 0u, &result, &detail) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(result == positive_zero_hash);

    fixture_destroy(&fixture);
}

static void test_promoted_conversions(void)
{
    fixture_t fixture;
    st_value_t receiver;
    st_value_t result = ST_VALUE_INVALID;
    st_integer_view_t view;
    uint32_t storage[2];
    uint32_t detail = 0u;
    int64_t small_result;

    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;
    CHECK(st_aot_thread_numeric_attach(&fixture.thread, &fixture.numeric));
    fixture.method.arity = 0u;
    fixture.frame.argc = 0u;
    fixture.frame.argv = NULL;

    receiver = box(&fixture, UINT64_C(0x43b0000000000000)); /* 2^60 */
    fixture.frame.receiver = receiver;
    CHECK(st_aot_float_truncated_primitive_execute(
        &fixture.frame, receiver, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_OK);
    CHECK(detail == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_integer_view(&fixture.numeric, result, storage, &view) ==
          ST_INTEGER_PRIMITIVE_OK);
    CHECK(!view.is_small_integer && !view.negative && view.limb_count == 2u);
    CHECK(view.limbs[0] == 0u && view.limbs[1] == UINT32_C(0x10000000));

    receiver = box(&fixture, UINT64_C(0xc004000000000000)); /* -2.5 */
    fixture.frame.receiver = receiver;
    CHECK(st_aot_float_rounded_primitive_execute(
        &fixture.frame, receiver, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &small_result));
    CHECK(small_result == -3);

    receiver = box(&fixture, UINT64_C(0x7fefffffffffffff));
    fixture.frame.receiver = receiver;
    CHECK(st_aot_float_floor_primitive_execute(
        &fixture.frame, receiver, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_OK);
    CHECK(st_integer_view(&fixture.numeric, result, storage, &view) ==
          ST_INTEGER_PRIMITIVE_OK);
    CHECK(!view.is_small_integer && !view.negative && view.limb_count == 32u);

    receiver = box(&fixture, UINT64_C(0x7ff0000000000000));
    fixture.frame.receiver = receiver;
    result = st_value_true();
    detail = 17u;
    CHECK(st_aot_float_ceiling_primitive_execute(
        &fixture.frame, receiver, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_ERR_NON_FINITE);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);

    fixture_destroy(&fixture);
}

static void test_promoted_conversion_oom_is_not_retried(void)
{
    fixture_t fixture;
    scratch_fault_t fault = {0};
    st_primitive_allocator_t allocator = {
        .allocate = scratch_allocate,
        .deallocate = scratch_deallocate,
        .user = &fault
    };
    st_value_t receiver;
    st_value_t result = st_value_true();
    uint32_t detail = 0u;

    CHECK(fixture_init_with_scratch(&fixture, allocator));
    if (!fixture.thread.initialized) return;
    CHECK(st_aot_thread_numeric_attach(&fixture.thread, &fixture.numeric));
    fixture.method.arity = 0u;
    fixture.frame.argc = 0u;
    fixture.frame.argv = NULL;
    receiver = box(&fixture, UINT64_C(0x43b0000000000000));
    fixture.frame.receiver = receiver;
    fault.fail = true;
    CHECK(st_aot_float_truncated_primitive_execute(
        &fixture.frame, receiver, NULL, 0u, &result, &detail) ==
        ST_FLOAT_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(detail == ST_INTEGER_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(fault.allocation_calls == 1u);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_arithmetic_hash_and_validation();
    test_promoted_conversions();
    test_promoted_conversion_oom_is_not_retried();
    if (failures != 0u) {
        fprintf(stderr, "Float primitive bridge: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("Float primitive bridge: PASS");
    return EXIT_SUCCESS;
}
