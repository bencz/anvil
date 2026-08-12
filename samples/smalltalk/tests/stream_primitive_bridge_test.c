#include "st_stream_primitive_bridge.h"

#include <errno.h>
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
    st_heap_t heap;
    st_lookup_context_t lookup;
    st_stream_primitive_context_t streams;
    st_aot_thread_t thread;
    StMethodDescriptor method;
    StFrame frame;
    st_value_t string;
    unsigned char written[16];
    size_t written_count;
    int error;
} fixture_t;

static int64_t write_bytes(void *user, int descriptor, const void *bytes,
                           size_t byte_count, int *os_error_out)
{
    fixture_t *fixture = user;
    CHECK(descriptor == 1);
    if (fixture->error != 0) {
        *os_error_out = fixture->error;
        return -1;
    }
    if (byte_count > sizeof(fixture->written) - fixture->written_count) {
        *os_error_out = EOVERFLOW;
        return -1;
    }
    memcpy(fixture->written + fixture->written_count, bytes, byte_count);
    fixture->written_count += byte_count;
    *os_error_out = 0;
    return (int64_t)byte_count;
}

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1] = {
        "Object", "ByteString", "UndefinedObject", "False", "True",
        "SmallInteger", "Character", "Metaclass"
    };
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    st_object_view_t view;
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
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->shape_storage[CLASS_STRING - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, CLASS_COUNT - 1u,
        fixture->shapes, CLASS_COUNT - 1u
    };
    fixture->method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_OBJECT,
        .code_size = 0u
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK
            || st_heap_init(&fixture->heap, &fixture->descriptors,
                            (st_runtime_allocator_t){0}) != ST_HEAP_OK
            || st_heap_allocate(&fixture->heap, CLASS_STRING, CLASS_STRING,
                                3u, 3u, ST_HEADER_IMMUTABLE,
                                &fixture->string) != ST_HEAP_OK
            || st_heap_object_view(&fixture->heap, fixture->string, &view)
                != ST_HEAP_OK)
        return false;
    memcpy(view.indexed_elements, "A\0Z", 3u);
    if (st_stream_primitive_context_init(
            &fixture->streams, &(st_stream_primitive_options_t) {
                .heap = &fixture->heap,
                .string_class_id = CLASS_STRING,
                .string_shape_id = CLASS_STRING,
                .write_bytes = write_bytes,
                .write_user = fixture
            }) != ST_STREAM_PRIMITIVE_OK
            || st_lookup_context_init(&fixture->lookup,
                                      &fixture->descriptors,
                                      (st_lookup_allocator_t){0})
                != ST_LOOKUP_FOUND
            || !st_aot_thread_init(&fixture->thread, &fixture->lookup,
                                   immediate, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL))
        return false;
    fixture->frame = (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->method,
        .receiver = st_value_nil()
    };
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    if (fixture->thread.streams != NULL)
        CHECK(st_aot_thread_streams_detach(&fixture->thread,
                                           &fixture->streams));
    st_aot_thread_destroy(&fixture->thread);
    st_lookup_context_destroy(&fixture->lookup);
    st_stream_primitive_context_destroy(&fixture->streams);
    st_heap_destroy(&fixture->heap);
}

static void test_bridge_context_status_and_detail(void)
{
    fixture_t fixture;
    st_value_t arguments[3];
    st_value_t result = st_value_nil();
    uint32_t detail = 99u;
    int64_t written = -1;
    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;
    CHECK(st_value_from_small_integer(1, &arguments[0]));
    CHECK(st_value_from_small_integer(3, &arguments[1]));
    arguments[2] = fixture.string;

    CHECK(st_aot_stream_write_primitive_execute(
              &fixture.frame, fixture.frame.receiver, arguments, 3u,
              &result, &detail)
          == (uint32_t)ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT
          && result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_streams_attach(&fixture.thread, &fixture.streams));
    CHECK(!st_aot_thread_streams_attach(&fixture.thread, &fixture.streams));
    CHECK(st_aot_stream_write_primitive_execute(
              &fixture.frame, fixture.frame.receiver, arguments, 3u,
              &result, &detail) == (uint32_t)ST_STREAM_PRIMITIVE_OK
          && detail == 0u && st_value_to_small_integer(result, &written)
          && written == 3 && fixture.written_count == 3u
          && memcmp(fixture.written, "A\0Z", 3u) == 0);

    fixture.error = EPIPE;
    result = st_value_true();
    detail = 0u;
    CHECK(st_aot_stream_write_primitive_execute(
              &fixture.frame, fixture.frame.receiver, arguments, 3u,
              &result, &detail)
          == (uint32_t)ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED
          && result == ST_VALUE_INVALID && detail == (uint32_t)EPIPE);
    fixture.error = 0;
    fixture.frame.argc = 1u;
    result = st_value_true();
    detail = 88u;
    CHECK(st_aot_stream_write_primitive_execute(
              &fixture.frame, fixture.frame.receiver, arguments, 3u,
              &result, &detail)
          == (uint32_t)ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT
          && result == ST_VALUE_INVALID && detail == 0u);
    fixture.frame.argc = 0u;
    CHECK(!st_aot_thread_streams_detach(
        &fixture.thread,
        (const st_stream_primitive_context_t *)(uintptr_t)8u));
    fixture_destroy(&fixture);
}

int main(void)
{
    test_bridge_context_status_and_detail();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk stream primitive bridge: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk stream primitive bridge: PASS "
         "(authenticated frame/context, separate OS detail)");
    return 0;
}
