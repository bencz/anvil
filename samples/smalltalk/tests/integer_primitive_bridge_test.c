#include "st_integer_primitives.h"
#include "st_send_bridge.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
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

static void configure_raw_shape(fixture_t *fixture, uint32_t id,
                                st_indexed_format_t indexed_format)
{
    StShapeDescriptor *shape = &fixture->shape_storage[id - 1u];
    shape->fixed_word_count = 1u;
    shape->minimum_allocation_size = 32u;
    shape->fixed_pointer_bitmap = &fixture->raw_bitmap;
    shape->fixed_pointer_bitmap_word_count = 1u;
    shape->indexed_format = indexed_format;
}

static bool fixture_init(fixture_t *fixture)
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
        .owner_class_id = CLASS_INTEGER,
        .arity = 2u,
        .code_size = 0u
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
                    .float_primitives = &fixture->floats
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
        .receiver = st_value_nil(),
        .argc = 2u
    };
    return true;
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

static st_value_t small(int64_t integer)
{
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_value_from_small_integer(integer, &result));
    return result;
}

static void test_bridge(void)
{
    fixture_t fixture;
    st_value_t arguments[2] = { small(ST_INTEGER_BINARY_ADD), small(1) };
    st_value_t result = st_value_true();
    uint32_t detail = 99u;
    uint32_t limbs[2] = { 0u, UINT32_C(0x10000000) };
    st_value_t large;
    int64_t comparison;
    uint64_t float_bits;

    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;
    fixture.frame.argv = arguments;
    fixture.frame.receiver = small(ST_SMALL_INTEGER_MAX);

    CHECK(st_aot_large_integer_binary_primitive_execute(
        &fixture.frame, fixture.frame.receiver, arguments, 2u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_ERR_INVALID_STATE);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_numeric_attach(
        &fixture.thread, &fixture.numeric));
    CHECK(!st_aot_thread_numeric_attach(
        &fixture.thread, &fixture.numeric));

    CHECK(st_aot_large_integer_binary_primitive_execute(
        &fixture.frame, fixture.frame.receiver, arguments, 2u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(detail == 0u);
    CHECK(st_integer_view(
        &fixture.numeric, result, limbs, &(st_integer_view_t) {0})
        == ST_INTEGER_PRIMITIVE_OK);
    large = result;

    fixture.method.arity = 1u;
    fixture.frame.argc = 1u;
    fixture.frame.argv = &arguments[1];
    CHECK(st_aot_large_integer_compare_primitive_execute(
        &fixture.frame, large, &arguments[1], 1u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &comparison)
          && comparison == 1);

    arguments[1] = small(-60);
    CHECK(st_aot_large_integer_shift_primitive_execute(
        &fixture.frame, large, &arguments[1], 1u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &comparison)
          && comparison == 1);

    fixture.method.arity = 0u;
    fixture.frame.argc = 0u;
    fixture.frame.argv = NULL;
    CHECK(st_aot_large_integer_as_float_primitive_execute(
        &fixture.frame, large, NULL, 0u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_float_primitive_unbox_bits(
        &fixture.floats, result, &float_bits) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(float_bits == UINT64_C(0x43b0000000000000));
    CHECK(st_aot_small_integer_as_float_primitive_execute(
        &fixture.frame, small(-7), NULL, 0u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_float_primitive_unbox_bits(
        &fixture.floats, result, &float_bits) == ST_FLOAT_PRIMITIVE_OK);
    CHECK(float_bits == UINT64_C(0xc01c000000000000));
    CHECK(st_aot_integer_hash_primitive_execute(
        &fixture.frame, large, NULL, 0u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &comparison)
          && comparison >= 0);
    CHECK(st_aot_integer_hash_primitive_execute(
        &fixture.frame, small(17), NULL, 0u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &comparison)
          && comparison >= 0);

    arguments[0] = small(99);
    arguments[1] = small(1);
    fixture.method.arity = 2u;
    fixture.frame.argc = 2u;
    fixture.frame.argv = arguments;
    CHECK(st_aot_large_integer_binary_primitive_execute(
        &fixture.frame, large, arguments, 2u,
        &result, &detail) == ST_INTEGER_PRIMITIVE_ERR_UNKNOWN_OPERATION);
    CHECK(result == ST_VALUE_INVALID && detail == 99u);
    CHECK(!st_aot_thread_numeric_detach(
        &fixture.thread,
        (const st_numeric_context_t *)(uintptr_t)8u));
    fixture_destroy(&fixture);
}

int main(void)
{
    test_bridge();
    if (failures != 0u) {
        fprintf(stderr, "integer primitive bridge: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("integer primitive bridge: PASS (authenticated numeric sidecar)");
    return 0;
}
