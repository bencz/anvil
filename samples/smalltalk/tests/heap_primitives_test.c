#include "st_heap_primitives.h"
#include "st_source_bundle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_ARRAY = 2,
    CLASS_METACLASS = 3,
    CLASS_SMALL_INTEGER = 4,
    CLASS_CHARACTER = 5,
    CLASS_NIL = 6,
    CLASS_FALSE = 7,
    CLASS_TRUE = 8,
    CLASS_BYTE_ARRAY = 9,
    CLASS_UINT16_ARRAY = 10,
    CLASS_UINT32_ARRAY = 11,
    CLASS_UINT64_ARRAY = 12,
    CLASS_STRING = 13,
    CLASS_SYMBOL = 14,
    CLASS_CLOSURE_CELL = 15,
    CLASS_COUNT = 15
};

typedef struct {
    uint64_t object_bitmap;
    uint64_t array_bitmap;
    StClassDescriptor classes_storage[CLASS_COUNT];
    StShapeDescriptor shapes_storage[CLASS_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[CLASS_COUNT];
    const char *names[CLASS_COUNT];
    st_heap_indexed_access_t indexed_access[CLASS_COUNT];
    st_runtime_descriptors_t descriptors;
} fixture_t;

static void fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "Array", "Metaclass", "SmallInteger", "Character",
        "UndefinedObject", "False", "True", "ByteArray", "UInt16Array",
        "UInt32Array", "UInt64Array", "String", "Symbol", "ClosureCell"
    };
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    fixture->object_bitmap = UINT64_C(1);
    fixture->array_bitmap = UINT64_C(5);
    for (index = 0u; index < CLASS_COUNT; ++index) {
        uint32_t class_id = (uint32_t)index + 1u;
        fixture->names[index] = names[index];
        fixture->classes_storage[index] = (StClassDescriptor){
            .class_id = class_id,
            .superclass_id = class_id == CLASS_OBJECT ||
                              class_id == CLASS_METACLASS ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS ? ST_CLASS_METACLASS
                   : class_id == CLASS_SYMBOL
                          || class_id == CLASS_CLOSURE_CELL
                       ? ST_CLASS_ABSTRACT : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shapes_storage[index] = (StShapeDescriptor){
            .shape_id = class_id,
            .class_id = class_id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->indexed_access[index] = ST_HEAP_INDEXED_ACCESS_NONE;
        fixture->classes[index] = &fixture->classes_storage[index];
        fixture->shapes[index] = &fixture->shapes_storage[index];
    }
    fixture->shapes_storage[CLASS_OBJECT - 1u].minimum_allocation_size = 32u;
    fixture->shapes_storage[CLASS_OBJECT - 1u].fixed_word_count = 1u;
    fixture->shapes_storage[CLASS_OBJECT - 1u].fixed_pointer_bitmap =
        &fixture->object_bitmap;
    fixture->shapes_storage[CLASS_OBJECT - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->shapes_storage[CLASS_ARRAY - 1u].minimum_allocation_size = 48u;
    fixture->shapes_storage[CLASS_ARRAY - 1u].fixed_word_count = 3u;
    fixture->shapes_storage[CLASS_ARRAY - 1u].indexed_format =
        ST_INDEXED_VALUES;
    fixture->shapes_storage[CLASS_ARRAY - 1u].fixed_pointer_bitmap =
        &fixture->array_bitmap;
    fixture->shapes_storage[CLASS_ARRAY - 1u]
        .fixed_pointer_bitmap_word_count = 1u;
    fixture->indexed_access[CLASS_ARRAY - 1u] =
        ST_HEAP_INDEXED_ACCESS_VALUES;

    fixture->shapes_storage[CLASS_CLOSURE_CELL - 1u]
        .minimum_allocation_size = 32u;
    fixture->shapes_storage[CLASS_CLOSURE_CELL - 1u]
        .fixed_word_count = 1u;
    fixture->shapes_storage[CLASS_CLOSURE_CELL - 1u]
        .fixed_pointer_bitmap = &fixture->object_bitmap;
    fixture->shapes_storage[CLASS_CLOSURE_CELL - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->shapes_storage[CLASS_BYTE_ARRAY - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->indexed_access[CLASS_BYTE_ARRAY - 1u] =
        ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    fixture->shapes_storage[CLASS_UINT16_ARRAY - 1u].indexed_format =
        ST_INDEXED_UINT16;
    fixture->indexed_access[CLASS_UINT16_ARRAY - 1u] =
        ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    fixture->shapes_storage[CLASS_UINT32_ARRAY - 1u].indexed_format =
        ST_INDEXED_UINT32;
    fixture->indexed_access[CLASS_UINT32_ARRAY - 1u] =
        ST_HEAP_INDEXED_ACCESS_CHARACTER;
    fixture->shapes_storage[CLASS_UINT64_ARRAY - 1u].indexed_format =
        ST_INDEXED_UINT64;
    fixture->indexed_access[CLASS_UINT64_ARRAY - 1u] =
        ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    fixture->shapes_storage[CLASS_STRING - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->indexed_access[CLASS_STRING - 1u] =
        ST_HEAP_INDEXED_ACCESS_CHARACTER;

    fixture->descriptors = (st_runtime_descriptors_t){
        .classes = fixture->classes,
        .class_count = CLASS_COUNT,
        .shapes = fixture->shapes,
        .shape_count = CLASS_COUNT
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors) ==
          ST_RUNTIME_OK);
}

static st_value_t small_integer(int64_t integer)
{
    st_value_t result = 0u;
    CHECK(st_value_from_small_integer(integer, &result));
    return result;
}

static int64_t integer_result(st_value_t value)
{
    int64_t result = 0;
    CHECK(st_value_to_small_integer(value, &result));
    return result;
}

static st_value_t heap_allocate(st_heap_t *heap, uint32_t class_id,
                                size_t length, size_t capacity,
                                st_header_flags_t flags)
{
    st_value_t result = 0u;
    CHECK(st_heap_allocate(heap, class_id, class_id, length, capacity, flags,
                           &result) == ST_HEAP_OK);
    return result;
}

static void allocate_class_objects(st_heap_t *heap,
                                   st_value_t class_objects[CLASS_COUNT])
{
    size_t index;
    for (index = 0u; index < CLASS_COUNT; ++index)
        class_objects[index] = heap_allocate(heap, CLASS_METACLASS, 0u, 0u,
                                             ST_HEADER_PINNED);
}

static st_heap_primitive_options_t context_options(
    fixture_t *fixture, st_heap_t *heap,
    st_value_t class_objects[CLASS_COUNT])
{
    return (st_heap_primitive_options_t){
        .heap = heap,
        .immediate_classes = {
            .small_integer_class_id = CLASS_SMALL_INTEGER,
            .character_class_id = CLASS_CHARACTER,
            .nil_class_id = CLASS_NIL,
            .false_class_id = CLASS_FALSE,
            .true_class_id = CLASS_TRUE
        },
        .class_objects = class_objects,
        .class_object_count = CLASS_COUNT,
        .indexed_access = fixture->indexed_access,
        .indexed_access_count = CLASS_COUNT
    };
}

static st_heap_primitive_status_t execute0(
    st_heap_primitive_context_t *context, uint32_t intrinsic,
    st_value_t receiver, st_value_t *result)
{
    return st_heap_primitive_execute(context, intrinsic, receiver, NULL, 0u,
                                     result);
}

static st_heap_primitive_status_t execute1(
    st_heap_primitive_context_t *context, uint32_t intrinsic,
    st_value_t receiver, st_value_t argument, st_value_t *result)
{
    return st_heap_primitive_execute(context, intrinsic, receiver, &argument,
                                     1u, result);
}

static st_heap_primitive_status_t execute2(
    st_heap_primitive_context_t *context, uint32_t intrinsic,
    st_value_t receiver, st_value_t first, st_value_t second,
    st_value_t *result)
{
    st_value_t arguments[2] = { first, second };
    return st_heap_primitive_execute(context, intrinsic, receiver, arguments,
                                     2u, result);
}

static uint64_t hash_result(st_heap_primitive_context_t *context,
                            st_value_t receiver)
{
    st_value_t result = 0u;
    int64_t hash = 0;
    CHECK(execute0(context, ST_INTRINSIC_HASH, receiver, &result) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &hash) && hash >= 0);
    return (uint64_t)hash;
}

static uint64_t string_hash_result(st_heap_primitive_context_t *context,
                                   st_value_t receiver)
{
    st_value_t result = 0u;
    int64_t hash = 0;
    CHECK(execute0(context, ST_INTRINSIC_STRING_HASH, receiver, &result) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_value_to_small_integer(result, &hash) && hash >= 0);
    return (uint64_t)hash;
}

static bool array_equals_result(st_heap_primitive_context_t *context,
                                st_value_t receiver, st_value_t argument)
{
    st_value_t result = 0u;
    bool boolean = false;
    CHECK(execute1(context, ST_INTRINSIC_ARRAY_EQUALS, receiver, argument,
          &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(st_value_to_boolean(result, &boolean));
    return boolean;
}

static void test_context_class_and_allocation(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_heap_primitive_context_t context = {0};
    st_value_t class_objects[CLASS_COUNT];
    st_heap_primitive_options_t options;
    st_value_t result;
    st_value_t character;
    st_object_view_t view;
    size_t root_count = 0u;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    allocate_class_objects(&heap, class_objects);
    options = context_options(&fixture, &heap, class_objects);
    CHECK(st_heap_primitive_context_init(&context, &options) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_heap_primitive_class_roots(&context, &root_count) != NULL &&
          root_count == CLASS_COUNT);

    /* Frozen bit-exact vectors for the immediate identity-hash ABI. */
    CHECK(hash_result(&context, small_integer(ST_SMALL_INTEGER_MIN)) ==
          UINT64_C(0x0c7548805fe0e07b));
    CHECK(hash_result(&context, small_integer(ST_SMALL_INTEGER_MAX)) ==
          UINT64_C(0x0cd54dadc03713a1));
    CHECK(hash_result(&context, small_integer(0)) ==
          UINT64_C(0x06b2c0722ea312df));
    CHECK(st_value_from_character(0u, &character));
    CHECK(hash_result(&context, character) ==
          UINT64_C(0x0f6f3d95ef80a9d5));
    CHECK(st_value_from_character(UINT32_C(0x10ffff), &character));
    CHECK(hash_result(&context, character) ==
          UINT64_C(0x0185f24bf7f35e8f));
    CHECK(hash_result(&context, st_value_nil()) ==
          UINT64_C(0x0b85b66190125042));
    CHECK(hash_result(&context, st_value_false()) ==
          UINT64_C(0x00465bf187cb0e05));
    CHECK(hash_result(&context, st_value_true()) ==
          UINT64_C(0x0dc2977a194e686a));

    CHECK(execute0(&context, ST_INTRINSIC_CLASS, small_integer(1), &result) ==
          ST_HEAP_PRIMITIVE_OK &&
          result == class_objects[CLASS_SMALL_INTEGER - 1u]);
    CHECK(st_value_from_character(UINT32_C(0x1f642), &character));
    CHECK(execute0(&context, ST_INTRINSIC_CLASS, character, &result) ==
          ST_HEAP_PRIMITIVE_OK &&
          result == class_objects[CLASS_CHARACTER - 1u]);
    CHECK(execute0(&context, ST_INTRINSIC_CLASS, st_value_nil(), &result) ==
          ST_HEAP_PRIMITIVE_OK && result == class_objects[CLASS_NIL - 1u]);
    CHECK(execute0(&context, ST_INTRINSIC_CLASS, st_value_false(), &result) ==
          ST_HEAP_PRIMITIVE_OK && result == class_objects[CLASS_FALSE - 1u]);
    CHECK(execute0(&context, ST_INTRINSIC_CLASS, st_value_true(), &result) ==
          ST_HEAP_PRIMITIVE_OK && result == class_objects[CLASS_TRUE - 1u]);

    CHECK(execute0(&context, ST_INTRINSIC_BEHAVIOR_NEW,
          class_objects[CLASS_OBJECT - 1u], &result) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_heap_object_view(&heap, result, &view) == ST_HEAP_OK);
    CHECK(view.class_descriptor->class_id == CLASS_OBJECT &&
          view.shape_descriptor->fixed_word_count == 1u);
    CHECK(execute0(&context, ST_INTRINSIC_CLASS, result, &result) ==
          ST_HEAP_PRIMITIVE_OK && result == class_objects[CLASS_OBJECT - 1u]);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_ARRAY - 1u], small_integer(5), &result) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_heap_object_view(&heap, result, &view) == ST_HEAP_OK);
    CHECK(view.class_descriptor->class_id == CLASS_ARRAY &&
          view.indexed_length == 5u && view.indexed_capacity == 5u);
    CHECK(((st_value_t *)view.fixed_words)[0] == st_value_nil() &&
          ((st_value_t *)view.fixed_words)[2] == st_value_nil());
    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_OBJECT - 1u], small_integer(0), &result) ==
          ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT);
    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_ARRAY - 1u], small_integer(-1), &result) ==
          ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE);
    CHECK(execute0(&context, ST_INTRINSIC_BEHAVIOR_NEW,
          class_objects[CLASS_SYMBOL - 1u], &result) ==
          ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS);
    CHECK(result == 0u);
    CHECK(execute0(&context, ST_INTRINSIC_BEHAVIOR_NEW,
          class_objects[CLASS_CLOSURE_CELL - 1u], &result) ==
          ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS);
    CHECK(result == 0u);

    CHECK(execute0(&context, ST_INTRINSIC_SIZE, small_integer(8), &result) ==
          ST_HEAP_PRIMITIVE_OK && integer_result(result) == 0);
    CHECK(st_heap_primitive_execute(&context, ST_INTRINSIC_AT,
          st_value_nil(), NULL, 0u, &result) ==
          ST_HEAP_PRIMITIVE_ERR_WRONG_ARITY && result == 0u);
    CHECK(st_heap_primitive_execute(&context, UINT32_MAX, st_value_nil(),
          NULL, 0u, &result) == ST_HEAP_PRIMITIVE_ERR_UNKNOWN_INTRINSIC);
    st_heap_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_indexed_access_and_instvars(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_heap_primitive_context_t context = {0};
    st_value_t class_objects[CLASS_COUNT];
    st_heap_primitive_options_t options;
    st_value_t array;
    st_value_t child;
    st_value_t bytes;
    st_value_t words16;
    st_value_t wide_string;
    st_value_t words64;
    st_value_t string;
    st_value_t immutable;
    st_value_t result;
    st_value_t character;
    st_object_view_t view;
    void *foreign;
    st_value_t foreign_value = 0u;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    allocate_class_objects(&heap, class_objects);
    options = context_options(&fixture, &heap, class_objects);
    CHECK(st_heap_primitive_context_init(&context, &options) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_ARRAY - 1u], small_integer(3), &array) ==
          ST_HEAP_PRIMITIVE_OK);
    child = heap_allocate(&heap, CLASS_OBJECT, 0u, 0u, 0u);
    CHECK(execute1(&context, ST_INTRINSIC_AT, array, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_OK && result == st_value_nil());
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, array, small_integer(2),
          child, &result) == ST_HEAP_PRIMITIVE_OK && result == child);
    CHECK(execute1(&context, ST_INTRINSIC_AT, array, small_integer(2),
          &result) == ST_HEAP_PRIMITIVE_OK && result == child);
    CHECK(execute1(&context, ST_INTRINSIC_AT, array, small_integer(0),
          &result) == ST_HEAP_PRIMITIVE_ERR_INDEX_OUT_OF_BOUNDS);
    CHECK(execute1(&context, ST_INTRINSIC_AT, array, small_integer(4),
          &result) == ST_HEAP_PRIMITIVE_ERR_INDEX_OUT_OF_BOUNDS);

    CHECK(execute2(&context, ST_INTRINSIC_INST_VAR_AT_PUT, array,
          small_integer(1), child, &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_INST_VAR_AT, array,
          small_integer(1), &result) == ST_HEAP_PRIMITIVE_OK &&
          result == child);
    CHECK(execute2(&context, ST_INTRINSIC_INST_VAR_AT_PUT, array,
          small_integer(3), st_value_true(), &result) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_INST_VAR_AT, array,
          small_integer(3), &result) == ST_HEAP_PRIMITIVE_OK &&
          result == st_value_true());
    CHECK(execute1(&context, ST_INTRINSIC_INST_VAR_AT, array,
          small_integer(2), &result) == ST_HEAP_PRIMITIVE_ERR_BAD_SLOT_FORMAT);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_BYTE_ARRAY - 1u], small_integer(4), &bytes) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, bytes, small_integer(1),
          small_integer(255), &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_AT, bytes, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_OK && integer_result(result) == 255);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, bytes, small_integer(1),
          small_integer(256), &result) ==
          ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_UINT16_ARRAY - 1u], small_integer(1), &words16)
          == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, words16, small_integer(1),
          small_integer(65535), &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_AT, words16, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_OK &&
          integer_result(result) == 65535);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_UINT32_ARRAY - 1u], small_integer(1),
          &wide_string) == ST_HEAP_PRIMITIVE_OK);
    CHECK(st_value_from_character(UINT32_C(0x1f642), &character));
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, wide_string,
          small_integer(1), character, &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_AT, wide_string, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_OK && result == character);
    view = (st_object_view_t){0};
    CHECK(st_heap_object_view(&heap, wide_string, &view) == ST_HEAP_OK);
    ((uint32_t *)view.indexed_elements)[0] = UINT32_C(0xd800);
    CHECK(execute1(&context, ST_INTRINSIC_AT, wide_string, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_UINT64_ARRAY - 1u], small_integer(1), &words64)
          == ST_HEAP_PRIMITIVE_OK);
    view = (st_object_view_t){0};
    CHECK(st_heap_object_view(&heap, words64, &view) == ST_HEAP_OK);
    ((uint64_t *)view.indexed_elements)[0] = UINT64_MAX;
    CHECK(execute1(&context, ST_INTRINSIC_AT, words64, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_ERR_PROMOTION_REQUIRED);

    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_STRING - 1u], small_integer(1), &string) ==
          ST_HEAP_PRIMITIVE_OK);
    CHECK(st_value_from_character(UINT32_C(0xe9), &character));
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, string, small_integer(1),
          character, &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK(execute1(&context, ST_INTRINSIC_AT, string, small_integer(1),
          &result) == ST_HEAP_PRIMITIVE_OK && result == character);
    CHECK(st_value_from_character(UINT32_C(0x100), &character));
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, string, small_integer(1),
          character, &result) ==
          ST_HEAP_PRIMITIVE_ERR_VALUE_OUT_OF_RANGE);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, string, small_integer(1),
          small_integer(65), &result) == ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH);

    immutable = heap_allocate(&heap, CLASS_ARRAY, 1u, 1u,
                              ST_HEADER_IMMUTABLE);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, immutable, small_integer(1),
          child, &result) == ST_HEAP_PRIMITIVE_ERR_IMMUTABLE);
    CHECK(execute2(&context, ST_INTRINSIC_INST_VAR_AT_PUT, immutable,
          small_integer(1), child, &result) ==
          ST_HEAP_PRIMITIVE_ERR_IMMUTABLE);

    foreign = aligned_alloc(16u, 32u);
    CHECK(foreign != NULL);
    if (foreign) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        CHECK(execute0(&context, ST_INTRINSIC_SIZE, foreign_value, &result) ==
              ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER);
        CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, array,
              small_integer(1), foreign_value, &result) ==
              ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE);
        view = (st_object_view_t){0};
        CHECK(st_heap_object_view(&heap, array, &view) == ST_HEAP_OK);
        ((st_value_t *)view.indexed_elements)[0] = foreign_value;
        CHECK(execute1(&context, ST_INTRINSIC_AT, array, small_integer(1),
              &result) == ST_HEAP_PRIMITIVE_ERR_DANGLING_REFERENCE);
        ((st_value_t *)view.indexed_elements)[0] = st_value_nil();
        free(foreign);
    }
    CHECK(execute0(&context, ST_INTRINSIC_SIZE, array + 8u, &result) ==
          ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER);
    CHECK(execute0(&context, ST_INTRINSIC_SIZE, 0u, &result) ==
          ST_HEAP_PRIMITIVE_ERR_INVALID_VALUE);
    st_heap_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void put_character(st_heap_primitive_context_t *context,
                          st_value_t string, int64_t index,
                          uint32_t code_point)
{
    st_value_t character = 0u;
    st_value_t result = 0u;
    CHECK(st_value_from_character(code_point, &character));
    CHECK(execute2(context, ST_INTRINSIC_AT_PUT, string,
          small_integer(index), character, &result) ==
          ST_HEAP_PRIMITIVE_OK);
}

static void test_character_equality_and_string_hash(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_heap_primitive_context_t context = {0};
    st_value_t class_objects[CLASS_COUNT];
    st_heap_primitive_options_t options;
    st_value_t narrow;
    st_value_t narrow_copy;
    st_value_t wide;
    st_value_t shorter;
    st_value_t empty;
    st_value_t bytes;
    st_value_t result = 0u;
    st_object_view_t view;
    uint64_t original_hash;
    uint64_t wide_hash;
    void *foreign;
    st_value_t foreign_value = 0u;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    allocate_class_objects(&heap, class_objects);
    options = context_options(&fixture, &heap, class_objects);
    CHECK(st_heap_primitive_context_init(&context, &options) ==
          ST_HEAP_PRIMITIVE_OK);
    narrow = heap_allocate(&heap, CLASS_STRING, 3u, 3u, 0u);
    narrow_copy = heap_allocate(&heap, CLASS_STRING, 3u, 3u, 0u);
    wide = heap_allocate(&heap, CLASS_UINT32_ARRAY, 3u, 3u, 0u);
    shorter = heap_allocate(&heap, CLASS_STRING, 2u, 2u, 0u);
    empty = heap_allocate(&heap, CLASS_STRING, 0u, 0u, 0u);
    bytes = heap_allocate(&heap, CLASS_BYTE_ARRAY, 3u, 3u, 0u);
    put_character(&context, narrow, 1, 'A');
    put_character(&context, narrow, 2, UINT32_C(0xe9));
    put_character(&context, narrow, 3, 'z');
    put_character(&context, narrow_copy, 1, 'A');
    put_character(&context, narrow_copy, 2, UINT32_C(0xe9));
    put_character(&context, narrow_copy, 3, 'z');
    put_character(&context, wide, 1, 'A');
    put_character(&context, wide, 2, UINT32_C(0xe9));
    put_character(&context, wide, 3, 'z');
    put_character(&context, shorter, 1, 'A');
    put_character(&context, shorter, 2, UINT32_C(0xe9));
    CHECK(array_equals_result(&context, narrow, narrow_copy));
    CHECK(array_equals_result(&context, narrow, wide));
    CHECK(!array_equals_result(&context, narrow, shorter));
    CHECK(!array_equals_result(&context, narrow, bytes));
    original_hash = string_hash_result(&context, narrow);
    wide_hash = string_hash_result(&context, wide);
    CHECK(original_hash == UINT64_C(0x0b13e029cfbb0346));
    CHECK(original_hash == wide_hash);
    CHECK(string_hash_result(&context, empty) ==
          UINT64_C(0x013f0174a2367c13));
    put_character(&context, narrow_copy, 3, 'x');
    CHECK(!array_equals_result(&context, narrow, narrow_copy));
    CHECK(string_hash_result(&context, narrow_copy) != original_hash);
    /* No hash cache: mutating a valid logical character changes the answer. */
    put_character(&context, narrow_copy, 3, 'z');
    CHECK(string_hash_result(&context, narrow_copy) == original_hash);

    foreign = aligned_alloc(16u, 32u);
    CHECK(foreign != NULL);
    if (foreign) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        CHECK(execute1(&context, ST_INTRINSIC_ARRAY_EQUALS, narrow,
              foreign_value, &result) == ST_HEAP_PRIMITIVE_ERR_NOT_MEMBER);
        free(foreign);
    }
    CHECK(execute1(&context, ST_INTRINSIC_ARRAY_EQUALS, narrow,
          small_integer(1), &result) == ST_HEAP_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(execute1(&context, ST_INTRINSIC_ARRAY_EQUALS, bytes, narrow,
          &result) == ST_HEAP_PRIMITIVE_ERR_BAD_INDEXED_FORMAT);
    view = (st_object_view_t){0};
    CHECK(st_heap_object_view(&heap, wide, &view) == ST_HEAP_OK);
    ((uint32_t *)view.indexed_elements)[1] = UINT32_C(0xd800);
    CHECK(execute1(&context, ST_INTRINSIC_ARRAY_EQUALS, narrow, wide,
          &result) == ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT);
    CHECK(execute0(&context, ST_INTRINSIC_STRING_HASH, wide, &result) ==
          ST_HEAP_PRIMITIVE_ERR_BAD_OBJECT);
    st_heap_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_write_barrier_and_gc_retention(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_heap_primitive_context_t context = {0};
    st_value_t class_objects[CLASS_COUNT];
    st_heap_primitive_options_t options;
    st_value_t parent;
    st_value_t child;
    st_value_t garbage_before;
    st_value_t garbage_after;
    st_value_t result;
    st_value_t *roots;
    const st_value_t *class_roots;
    size_t class_root_count;
    st_object_view_t parent_view;
    st_gc_generation_t generation;
    uint8_t age;
    st_heap_collection_stats_t stats;
    uint64_t parent_hash;
    uint64_t child_hash;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    allocate_class_objects(&heap, class_objects);
    options = context_options(&fixture, &heap, class_objects);
    CHECK(st_heap_primitive_context_init(&context, &options) ==
          ST_HEAP_PRIMITIVE_OK);
    garbage_before = heap_allocate(&heap, CLASS_OBJECT, 0u, 0u, 0u);
    CHECK(execute1(&context, ST_INTRINSIC_BEHAVIOR_NEW_SIZE,
          class_objects[CLASS_ARRAY - 1u], small_integer(1), &parent) ==
          ST_HEAP_PRIMITIVE_OK);
    child = heap_allocate(&heap, CLASS_OBJECT, 0u, 0u, 0u);
    garbage_after = heap_allocate(&heap, CLASS_OBJECT, 0u, 0u, 0u);
    parent_hash = hash_result(&context, parent);
    child_hash = hash_result(&context, child);
    CHECK(parent_hash == hash_result(&context, parent));
    CHECK(parent_hash != child_hash);
    parent_view = (st_object_view_t){0};
    CHECK(st_heap_object_view(&heap, parent, &parent_view) == ST_HEAP_OK);
    CHECK(st_object_header_survive(&parent_view.object->header, 1u,
          &generation, &age) && generation == ST_GC_SURVIVOR);
    CHECK(st_object_header_survive(&parent_view.object->header, 1u,
          &generation, &age) && generation == ST_GC_OLD);
    CHECK(execute2(&context, ST_INTRINSIC_AT_PUT, parent, small_integer(1),
          child, &result) == ST_HEAP_PRIMITIVE_OK);
    CHECK((st_object_header_flags(st_object_header_load(
          &parent_view.object->header)) & ST_HEADER_REMEMBERED) != 0u);
    class_roots = st_heap_primitive_class_roots(&context, &class_root_count);
    roots = malloc((class_root_count + 1u) * sizeof(*roots));
    CHECK(roots != NULL);
    if (roots) {
        memcpy(roots, class_roots, class_root_count * sizeof(*roots));
        roots[class_root_count] = parent;
        CHECK(st_heap_collect(&heap, NULL, roots, class_root_count + 1u,
              &stats) == ST_HEAP_OK);
        CHECK(st_heap_contains(&heap, parent) && st_heap_contains(&heap, child));
        CHECK(!st_heap_contains(&heap, garbage_before) &&
              !st_heap_contains(&heap, garbage_after));
        CHECK(hash_result(&context, parent) == parent_hash);
        CHECK(hash_result(&context, child) == child_hash);
        parent_view = (st_object_view_t){0};
        CHECK(st_heap_object_view(&heap, parent, &parent_view) == ST_HEAP_OK);
        CHECK((st_object_header_flags(st_object_header_load(
              &parent_view.object->header)) & ST_HEADER_REMEMBERED) == 0u);
        free(roots);
    }
    st_heap_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *result;
    if (fault->calls++ == fault->fail_at) return NULL;
    result = malloc(size);
    if (result) ++fault->live;
    return result;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    CHECK(pointer != NULL && fault->live != 0u);
    if (fault->live != 0u) --fault->live;
    free(pointer);
}

typedef struct {
    size_t calls;
    size_t fail_at;
} heap_fault_t;

static void *heap_fault_allocate(void *user, size_t alignment, size_t size)
{
    heap_fault_t *fault = user;
    if (fault->calls++ == fault->fail_at) return NULL;
    return aligned_alloc(alignment, size);
}

static void heap_fault_deallocate(void *user, void *pointer,
                                  size_t alignment, size_t size)
{
    (void)user;
    (void)alignment;
    (void)size;
    free(pointer);
}

static void test_heap_identity_sequence_overflow_and_fault(void)
{
    fixture_t fixture;
    st_heap_t first = {0};
    st_heap_t second = {0};
    st_value_t first_values[2];
    st_value_t second_values[2];
    uint64_t first_hashes[2] = {0};
    uint64_t second_hashes[2] = {0};
    st_heap_collection_stats_t stats;
    st_object_extent_t extent;
    void *foreign;
    st_value_t foreign_value = 0u;
    size_t index;
    fixture_init(&fixture);
    CHECK(st_heap_init_with_identity_seed(&first, &fixture.descriptors,
          (st_runtime_allocator_t){0}, UINT64_C(42)) == ST_HEAP_OK);
    CHECK(st_heap_init_with_identity_seed(&second, &fixture.descriptors,
          (st_runtime_allocator_t){0}, UINT64_C(42)) == ST_HEAP_OK);
    for (index = 0u; index < 2u; ++index) {
        first_values[index] = heap_allocate(&first, CLASS_OBJECT, 0u, 0u, 0u);
        second_values[index] = heap_allocate(&second, CLASS_OBJECT, 0u, 0u, 0u);
        CHECK(st_heap_identity_hash(&first, first_values[index],
              &first_hashes[index]) == ST_HEAP_OK);
        CHECK(st_heap_identity_hash(&second, second_values[index],
              &second_hashes[index]) == ST_HEAP_OK);
        CHECK(first_hashes[index] == second_hashes[index]);
    }
    CHECK(first_hashes[0] == UINT64_C(0x0dd732262feb6e95));
    CHECK(first_hashes[1] == UINT64_C(0x0a69ec90eb4fef88));
    CHECK(first_hashes[0] != first_hashes[1]);
    CHECK(st_heap_collect(&first, NULL, &first_values[1], 1u, &stats) ==
          ST_HEAP_OK);
    CHECK(!st_heap_contains(&first, first_values[0]) &&
          st_heap_contains(&first, first_values[1]));
    CHECK(st_heap_identity_hash(&first, first_values[1], &second_hashes[0]) ==
          ST_HEAP_OK && second_hashes[0] == first_hashes[1]);
    CHECK(st_heap_identity_hash(&first, first_values[1] + 8u,
          &second_hashes[0]) == ST_HEAP_ERR_NOT_MEMBER);
    CHECK(st_heap_identity_hash(&first, st_value_nil(), &second_hashes[0]) ==
          ST_HEAP_ERR_NOT_OBJECT);
    foreign = aligned_alloc(16u, 32u);
    CHECK(foreign != NULL);
    if (foreign) {
        CHECK(st_value_from_object(foreign, &foreign_value));
        CHECK(st_heap_identity_hash(&first, foreign_value,
              &second_hashes[0]) == ST_HEAP_ERR_NOT_MEMBER);
        free(foreign);
    }
    CHECK(st_heap_authorize(&first, first_values[1], &extent) == ST_HEAP_OK);
    st_heap_destroy(&first);
    st_heap_destroy(&second);

    {
        heap_fault_t fault = { .fail_at = SIZE_MAX };
        st_runtime_allocator_t allocator = {
            heap_fault_allocate, heap_fault_deallocate, &fault
        };
        st_heap_t failed_then_success = {0};
        st_heap_t reference = {0};
        st_value_t failed_value = UINT64_MAX;
        st_value_t retry;
        st_value_t reference_value;
        uint64_t retry_hash = 0u;
        uint64_t reference_hash = 0u;
        CHECK(st_heap_init_with_identity_seed(&failed_then_success,
              &fixture.descriptors, allocator, UINT64_C(100)) == ST_HEAP_OK);
        fault.fail_at = fault.calls;
        CHECK(st_heap_allocate(&failed_then_success, CLASS_OBJECT,
              CLASS_OBJECT, 0u, 0u, 0u, &failed_value) ==
              ST_HEAP_ERR_OUT_OF_MEMORY);
        CHECK(failed_value == 0u &&
              st_heap_object_count(&failed_then_success) == 0u);
        fault.fail_at = SIZE_MAX;
        retry = heap_allocate(&failed_then_success, CLASS_OBJECT, 0u, 0u, 0u);
        CHECK(st_heap_init_with_identity_seed(&reference,
              &fixture.descriptors, (st_runtime_allocator_t){0},
              UINT64_C(100)) == ST_HEAP_OK);
        reference_value = heap_allocate(&reference, CLASS_OBJECT, 0u, 0u, 0u);
        CHECK(st_heap_identity_hash(&failed_then_success, retry, &retry_hash) ==
              ST_HEAP_OK);
        CHECK(st_heap_identity_hash(&reference, reference_value,
              &reference_hash) == ST_HEAP_OK);
        CHECK(retry_hash == reference_hash);
        st_heap_destroy(&failed_then_success);
        st_heap_destroy(&reference);
    }

    {
        heap_fault_t fault = { .fail_at = SIZE_MAX };
        st_runtime_allocator_t allocator = {
            heap_fault_allocate, heap_fault_deallocate, &fault
        };
        st_heap_t exhausted = {0};
        st_value_t last;
        st_value_t rejected = UINT64_MAX;
        uint64_t hash = 0u;
        size_t calls_before;
        CHECK(st_heap_init_with_identity_seed(&exhausted,
              &fixture.descriptors, allocator, UINT64_MAX) == ST_HEAP_OK);
        last = heap_allocate(&exhausted, CLASS_OBJECT, 0u, 0u, 0u);
        CHECK(st_heap_identity_hash(&exhausted, last, &hash) == ST_HEAP_OK);
        CHECK(hash == UINT64_C(0x04d971771b652c20));
        calls_before = fault.calls;
        CHECK(st_heap_allocate(&exhausted, CLASS_OBJECT, CLASS_OBJECT,
              0u, 0u, 0u, &rejected) == ST_HEAP_ERR_OVERFLOW);
        CHECK(rejected == 0u && fault.calls == calls_before &&
              st_heap_object_count(&exhausted) == 1u);
        st_heap_destroy(&exhausted);
    }
}

static void test_context_faults_and_allocation_oom(void)
{
    fixture_t fixture;
    st_heap_t heap = {0};
    st_value_t class_objects[CLASS_COUNT];
    st_heap_primitive_options_t options;
    size_t failure;
    fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    allocate_class_objects(&heap, class_objects);
    options = context_options(&fixture, &heap, class_objects);
    for (failure = 0u; failure < 4u; ++failure) {
        fault_allocator_t fault = { .fail_at = failure };
        st_heap_primitive_context_t context = {0};
        options.allocator = (st_primitive_allocator_t){
            fault_allocate, fault_deallocate, &fault
        };
        CHECK(st_heap_primitive_context_init(&context, &options) ==
              ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(context.state == NULL && fault.live == 0u);
    }
    options.allocator = (st_primitive_allocator_t){0};
    {
        st_heap_primitive_context_t context = {0};
        st_value_t duplicate[CLASS_COUNT];
        memcpy(duplicate, class_objects, sizeof(duplicate));
        duplicate[1] = duplicate[0];
        options.class_objects = duplicate;
        CHECK(st_heap_primitive_context_init(&context, &options) ==
              ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP);
        CHECK(context.state == NULL);
        options.class_objects = class_objects;
        fixture.indexed_access[CLASS_STRING - 1u] =
            ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
        /* This is physically valid and deliberately changes semantics, so it
         * must be accepted: the policy is explicit, not class-name based. */
        CHECK(st_heap_primitive_context_init(&context, &options) ==
              ST_HEAP_PRIMITIVE_OK);
        st_heap_primitive_context_destroy(&context);
        fixture.indexed_access[CLASS_ARRAY - 1u] =
            ST_HEAP_INDEXED_ACCESS_CHARACTER;
        CHECK(st_heap_primitive_context_init(&context, &options) ==
              ST_HEAP_PRIMITIVE_ERR_INVALID_CLASS_MAP);
    }
    st_heap_destroy(&heap);

    {
        heap_fault_t fault = { .fail_at = SIZE_MAX };
        st_runtime_allocator_t allocator = {
            heap_fault_allocate, heap_fault_deallocate, &fault
        };
        st_heap_primitive_context_t context = {0};
        st_value_t result = UINT64_MAX;
        size_t before;
        fixture_init(&fixture);
        CHECK(st_heap_init(&heap, &fixture.descriptors, allocator) ==
              ST_HEAP_OK);
        allocate_class_objects(&heap, class_objects);
        options = context_options(&fixture, &heap, class_objects);
        CHECK(st_heap_primitive_context_init(&context, &options) ==
              ST_HEAP_PRIMITIVE_OK);
        before = st_heap_object_count(&heap);
        fault.fail_at = fault.calls;
        CHECK(execute0(&context, ST_INTRINSIC_BEHAVIOR_NEW,
              class_objects[CLASS_OBJECT - 1u], &result) ==
              ST_HEAP_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(result == 0u && st_heap_object_count(&heap) == before);
        st_heap_primitive_context_destroy(&context);
        st_heap_destroy(&heap);
    }
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static void test_specs_against_real_image(void)
{
    const char *image = first_existing("st-image",
                                       "samples/smalltalk/st-image");
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_ast_unit_t **units;
    const st_primitive_spec_t *specs;
    size_t count;
    size_t index;
    CHECK(image != NULL);
    if (!image) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL) ==
          ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (!units) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    for (index = 0u; index < bundle.count; ++index)
        units[index] = &bundle.files[index].ast;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    specs = st_core_primitive_specs(&count);
    CHECK(count == 19u);
    for (index = 0u; index < count; ++index)
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
    specs = st_heap_primitive_specs(&count);
    CHECK(count == 11u);
    for (index = 0u; index < count; ++index)
        CHECK(st_primitive_catalog_register(&catalog, &specs[index], NULL) ==
              ST_PRIMITIVE_OK);
    CHECK(st_primitive_catalog_count(&catalog) == 30u);
    CHECK(st_primitive_catalog_lookup(&catalog, "HashPrimitive", 13u) != NULL);
    CHECK(st_primitive_catalog_lookup(&catalog, "StringAsSymbolPrimitive",
          23u) == NULL);
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, bundle.count, &catalog, NULL) ==
          ST_PRIMITIVE_OK);
    /* Image contract: 69 pragma uses. The implemented sets cover 34 uses;
     * IntNotEquals is a real core handler not currently named by the image.
     * The remaining 35 uses stay hard missing-implementation diagnostics. */
    CHECK(result.binding_count == 34u);
    CHECK(result.diagnostic_count == 35u);
    CHECK(result.binding_count + result.diagnostic_count == 69u);
    for (index = 0u; index < result.diagnostic_count; ++index)
        CHECK(result.diagnostics[index].code ==
              ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    test_context_class_and_allocation();
    test_indexed_access_and_instvars();
    test_character_equality_and_string_hash();
    test_write_barrier_and_gc_retention();
    test_heap_identity_sequence_overflow_and_fault();
    test_context_faults_and_allocation_oom();
    test_specs_against_real_image();
    if (failures != 0u) {
        fprintf(stderr, "heap primitive regression: %u failure(s)\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("heap primitive regression: PASS");
    return EXIT_SUCCESS;
}
