#include "st_string_primitive_bridge.h"

#include "st_lookup.h"
#include "st_send_bridge.h"
#include "st_string_primitives.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                     \
        }                                                                    \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_STRING,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT,
    SHAPE_STRING16 = CLASS_COUNT,
    SHAPE_STRING32,
    SHAPE_COUNT = SHAPE_STRING32
};

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT - 1u];
    StShapeDescriptor shape_storage[SHAPE_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT - 1u];
    const StShapeDescriptor *shapes[SHAPE_COUNT];
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_lookup_context_t lookup;
    st_string_primitive_context_t strings;
    st_aot_thread_t thread;
    StMethodDescriptor method;
    StFrame frame;
    st_value_t left;
    st_value_t right;
} fixture_t;

static bool make_string(
    fixture_t *fixture, uint32_t shape_id, const uint32_t *scalars,
    size_t count, st_value_t *value_out)
{
    st_object_view_t view;

    *value_out = ST_VALUE_INVALID;
    if (st_heap_allocate(
            &fixture->heap, CLASS_STRING, shape_id, count, count,
            ST_HEADER_IMMUTABLE, value_out) != ST_HEAP_OK
            || st_heap_object_view(&fixture->heap, *value_out, &view)
                != ST_HEAP_OK) {
        return false;
    }
    for (size_t index = 0u; index < count; index++) {
        switch (view.shape_descriptor->indexed_format) {
        case ST_INDEXED_UINT8:
            ((uint8_t *)view.indexed_elements)[index] =
                (uint8_t)scalars[index];
            break;
        case ST_INDEXED_UINT16:
            ((uint16_t *)view.indexed_elements)[index] =
                (uint16_t)scalars[index];
            break;
        case ST_INDEXED_UINT32:
            ((uint32_t *)view.indexed_elements)[index] = scalars[index];
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1u] = {
        "Object", "String", "UndefinedObject", "False", "True",
        "SmallInteger", "Character", "Metaclass"
    };
    static const uint32_t left_scalars[] = {'A', UINT32_C(0x100)};
    static const uint32_t right_scalars[] = {'A', UINT32_C(0x101)};
    const uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };

    memset(fixture, 0, sizeof(*fixture));
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
            .indexed_format = id == CLASS_STRING
                ? ST_INDEXED_UINT8 : ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->shape_storage[SHAPE_STRING16 - 1u] = (StShapeDescriptor) {
        .shape_id = SHAPE_STRING16,
        .class_id = CLASS_STRING,
        .allocation_alignment = 8u,
        .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT16
    };
    fixture->shape_storage[SHAPE_STRING32 - 1u] = (StShapeDescriptor) {
        .shape_id = SHAPE_STRING32,
        .class_id = CLASS_STRING,
        .allocation_alignment = 8u,
        .minimum_allocation_size = 24u,
        .indexed_format = ST_INDEXED_UINT32
    };
    fixture->shapes[SHAPE_STRING16 - 1u] =
        &fixture->shape_storage[SHAPE_STRING16 - 1u];
    fixture->shapes[SHAPE_STRING32 - 1u] =
        &fixture->shape_storage[SHAPE_STRING32 - 1u];
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT - 1u,
        .shapes = fixture->shapes,
        .shape_count = SHAPE_COUNT
    };
    fixture->method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_OBJECT
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK
            || st_heap_init(
                &fixture->heap, &fixture->descriptors,
                (st_runtime_allocator_t) {0}) != ST_HEAP_OK
            || st_string_primitive_context_init(
                &fixture->strings, &(st_string_primitive_options_t) {
                    .heap = &fixture->heap,
                    .string_class_id = CLASS_STRING,
                    .uint8_shape_id = CLASS_STRING,
                    .uint16_shape_id = SHAPE_STRING16,
                    .uint32_shape_id = SHAPE_STRING32
                }) != ST_STRING_PRIMITIVE_OK
            || st_lookup_context_init(
                &fixture->lookup, &fixture->descriptors,
                (st_lookup_allocator_t) {0}) != ST_LOOKUP_FOUND
            || !st_aot_thread_init(
                &fixture->thread, &fixture->lookup, immediate,
                NULL, NULL, NULL, NULL, NULL, NULL, NULL)
            || !make_string(
                fixture, SHAPE_STRING16, left_scalars,
                sizeof(left_scalars) / sizeof(left_scalars[0]),
                &fixture->left)
            || !make_string(
                fixture, SHAPE_STRING16, right_scalars,
                sizeof(right_scalars) / sizeof(right_scalars[0]),
                &fixture->right)) {
        return false;
    }
    fixture->frame = (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->method,
        .receiver = fixture->left
    };
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    if (fixture->thread.strings != NULL) {
        CHECK(st_aot_thread_strings_detach(
            &fixture->thread, &fixture->strings));
    }
    st_aot_thread_destroy(&fixture->thread);
    st_lookup_context_destroy(&fixture->lookup);
    st_string_primitive_context_destroy(&fixture->strings);
    st_heap_destroy(&fixture->heap);
}

static void test_bridge(void)
{
    fixture_t fixture;
    st_value_t result = st_value_nil();
    uint32_t detail = 99u;
    int64_t comparison = 0;
    st_object_view_t concatenated;

    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) {
        return;
    }
    CHECK(st_aot_string_compare_primitive_execute(
              &fixture.frame, fixture.left, &fixture.right, 1u,
              &result, &detail)
          == (uint32_t)ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_strings_attach(&fixture.thread, &fixture.strings));
    CHECK(!st_aot_thread_strings_attach(&fixture.thread, &fixture.strings));
    CHECK(st_aot_string_compare_primitive_execute(
              &fixture.frame, fixture.left, &fixture.right, 1u,
              &result, &detail) == (uint32_t)ST_STRING_PRIMITIVE_OK);
    CHECK(detail == 0u && st_value_to_small_integer(result, &comparison)
          && comparison == -1);
    CHECK(st_aot_string_concat_primitive_execute(
              &fixture.frame, fixture.left, &fixture.right, 1u,
              &result, &detail) == (uint32_t)ST_STRING_PRIMITIVE_OK);
    CHECK(detail == 0u
          && st_heap_object_view(&fixture.heap, result, &concatenated)
              == ST_HEAP_OK
          && concatenated.indexed_capacity == 4u
          && concatenated.shape_descriptor->shape_id == SHAPE_STRING16);
    result = st_value_true();
    detail = 77u;
    CHECK(st_aot_string_compare_primitive_execute(
              &fixture.frame, fixture.left, NULL, 0u,
              &result, &detail)
          == (uint32_t)ST_STRING_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);
    CHECK(!st_aot_thread_strings_detach(
        &fixture.thread,
        (const st_string_primitive_context_t *)(uintptr_t)8u));
    fixture_destroy(&fixture);
}

int main(void)
{
    test_bridge();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk String primitive bridge: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk String primitive bridge: PASS");
    return 0;
}
