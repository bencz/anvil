#include "st_stream_primitives.h"
#include "st_core_primitives.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_source_bundle.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    CLASS_STRING = 2,
    CLASS_BYTES = 3,
    CLASS_METACLASS = 4,
    CLASS_COUNT = 4
};

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT];
    StShapeDescriptor shape_storage[CLASS_COUNT];
    const StClassDescriptor *classes[CLASS_COUNT];
    const StShapeDescriptor *shapes[CLASS_COUNT];
    st_runtime_descriptors_t descriptors;
} descriptor_fixture_t;

typedef struct {
    unsigned calls;
    unsigned mode;
    unsigned char bytes[64];
    size_t length;
} write_fixture_t;

static void descriptor_fixture_init(descriptor_fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT] = {
        "Object", "String", "ByteArray", "Metaclass"
    };
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    for (index = 0u; index < CLASS_COUNT; ++index) {
        uint32_t id = (uint32_t)index + 1u;
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
            .indexed_format = id == CLASS_STRING || id == CLASS_BYTES
                ? ST_INDEXED_UINT8 : ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT,
        .shapes = fixture->shapes,
        .shape_count = CLASS_COUNT
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static st_value_t small_integer(int64_t value)
{
    st_value_t result = ST_VALUE_INVALID;
    CHECK(st_value_from_small_integer(value, &result));
    return result;
}

static st_value_t allocate_bytes(st_heap_t *heap, uint32_t class_id,
                                 const void *bytes, size_t length)
{
    st_value_t value = ST_VALUE_INVALID;
    st_object_view_t view;
    CHECK(st_heap_allocate(heap, class_id, class_id, length, length,
                           ST_HEADER_IMMUTABLE, &value) == ST_HEAP_OK);
    CHECK(st_heap_object_view(heap, value, &view) == ST_HEAP_OK);
    if (length != 0u) memcpy(view.indexed_elements, bytes, length);
    return value;
}

static st_stream_primitive_status_t execute(
    st_stream_primitive_context_t *context, st_value_t descriptor,
    st_value_t count, st_value_t string, st_value_t *result_out,
    int *os_error_out)
{
    st_value_t arguments[3] = { descriptor, count, string };
    return st_stream_write_primitive_execute(
        context, st_value_nil(), arguments, 3u, result_out, os_error_out);
}

static int64_t scripted_write(void *user, int descriptor, const void *bytes,
                              size_t count, int *os_error_out)
{
    write_fixture_t *fixture = user;
    size_t amount;
    (void)descriptor;
    ++fixture->calls;
    if (fixture->mode == 1u && fixture->calls == 1u) {
        *os_error_out = EINTR;
        return -1;
    }
    if (fixture->mode == 2u) return 0;
    if (fixture->mode == 3u) return (int64_t)count + 1;
    amount = count > 2u ? 2u : count;
    CHECK(amount <= sizeof(fixture->bytes) - fixture->length);
    memcpy(fixture->bytes + fixture->length, bytes, amount);
    fixture->length += amount;
    *os_error_out = 0;
    return (int64_t)amount;
}

static void test_real_pipe_binary_and_utf8(void)
{
    static const unsigned char payload[] = {
        'A', 0u, 0xc3u, 0xa9u, '\n'
    };
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_stream_primitive_context_t context = {0};
    st_value_t string;
    st_value_t result = ST_VALUE_INVALID;
    int os_error = -1;
    int descriptors[2] = { -1, -1 };
    unsigned char observed[sizeof(payload)] = {0};
    ssize_t amount;
    int64_t result_count = -1;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_stream_primitive_context_init(
              &context, &(st_stream_primitive_options_t) {
                  .heap = &heap,
                  .string_class_id = CLASS_STRING,
                  .string_shape_id = CLASS_STRING
              }) == ST_STREAM_PRIMITIVE_OK);
    string = allocate_bytes(&heap, CLASS_STRING, payload, sizeof(payload));
    CHECK(pipe(descriptors) == 0);
    CHECK(execute(&context, small_integer(descriptors[1]),
                  small_integer((int64_t)sizeof(payload)), string,
                  &result, &os_error) == ST_STREAM_PRIMITIVE_OK);
    CHECK(os_error == 0
          && st_value_to_small_integer(result, &result_count)
          && result_count == (int64_t)sizeof(payload));
    CHECK(close(descriptors[1]) == 0);
    descriptors[1] = -1;
    amount = read(descriptors[0], observed, sizeof(observed));
    CHECK(amount == (ssize_t)sizeof(payload));
    CHECK(memcmp(payload, observed, sizeof(payload)) == 0);
    CHECK(close(descriptors[0]) == 0);
    st_stream_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_eintr_short_write_and_callback_contract(void)
{
    static const unsigned char payload[] = "abcdefg";
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_stream_primitive_context_t context = {0};
    write_fixture_t writer = {0};
    st_value_t string;
    st_value_t result = ST_VALUE_INVALID;
    int os_error = -1;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    writer.mode = 1u;
    CHECK(st_stream_primitive_context_init(
              &context, &(st_stream_primitive_options_t) {
                  .heap = &heap,
                  .string_class_id = CLASS_STRING,
                  .string_shape_id = CLASS_STRING,
                  .write_bytes = scripted_write,
                  .write_user = &writer
              }) == ST_STREAM_PRIMITIVE_OK);
    string = allocate_bytes(&heap, CLASS_STRING, payload,
                            sizeof(payload) - 1u);
    CHECK(execute(&context, small_integer(7), small_integer(7), string,
                  &result, &os_error) == ST_STREAM_PRIMITIVE_OK);
    CHECK(writer.calls == 5u && writer.length == 7u
          && memcmp(writer.bytes, payload, 7u) == 0 && os_error == 0);

    writer = (write_fixture_t){ .mode = 3u };
    context.write_user = &writer;
    CHECK(execute(&context, small_integer(7), small_integer(0), string,
                  &result, &os_error) == ST_STREAM_PRIMITIVE_OK);
    CHECK(writer.calls == 0u && result == small_integer(0) && os_error == 0);

    writer = (write_fixture_t){ .mode = 2u };
    context.write_user = &writer;
    result = st_value_true();
    os_error = -1;
    CHECK(execute(&context, small_integer(7), small_integer(1), string,
                  &result, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_ZERO_PROGRESS);
    CHECK(result == ST_VALUE_INVALID && os_error == 0);

    writer = (write_fixture_t){ .mode = 3u };
    context.write_user = &writer;
    result = st_value_true();
    CHECK(execute(&context, small_integer(7), small_integer(1), string,
                  &result, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_WRITE_CONTRACT);
    CHECK(result == ST_VALUE_INVALID && os_error == 0);
    st_stream_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_errors_types_provenance_and_bounds(void)
{
    static const unsigned char payload[] = "xy";
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_stream_primitive_context_t context = {0};
    st_value_t string;
    st_value_t bytes;
    st_value_t result = st_value_true();
    st_value_t arguments[3];
    st_value_t foreign_value = ST_VALUE_INVALID;
    void *foreign = aligned_alloc(8u, 24u);
    int os_error = -1;
    int descriptors[2] = { -1, -1 };
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    CHECK(st_stream_primitive_context_init(
              &context, &(st_stream_primitive_options_t) {
                  .heap = &heap,
                  .string_class_id = CLASS_STRING,
                  .string_shape_id = CLASS_STRING
              }) == ST_STREAM_PRIMITIVE_OK);
    string = allocate_bytes(&heap, CLASS_STRING, payload, 2u);
    bytes = allocate_bytes(&heap, CLASS_BYTES, payload, 2u);

    CHECK(pipe(descriptors) == 0);
    CHECK(close(descriptors[1]) == 0);
    result = st_value_true();
    CHECK(execute(&context, small_integer(descriptors[1]), small_integer(2),
                  string, &result, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_WRITE_FAILED);
    CHECK(result == ST_VALUE_INVALID && os_error == EBADF);
    CHECK(close(descriptors[0]) == 0);

#define EXPECT_FAILURE(descriptor_, count_, string_, expected_)             \
    do {                                                                     \
        result = st_value_true();                                            \
        os_error = -1;                                                       \
        CHECK(execute(&context, (descriptor_), (count_), (string_),          \
                      &result, &os_error) == (expected_));                    \
        CHECK(result == ST_VALUE_INVALID && os_error == 0);                  \
    } while (0)

    EXPECT_FAILURE(st_value_true(), small_integer(1), string,
                   ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH);
    EXPECT_FAILURE(small_integer(1), st_value_false(), string,
                   ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH);
    EXPECT_FAILURE(small_integer(-1), small_integer(1), string,
                   ST_STREAM_PRIMITIVE_ERR_DESCRIPTOR_OUT_OF_RANGE);
    EXPECT_FAILURE(small_integer((int64_t)INT_MAX + 1), small_integer(1),
                   string, ST_STREAM_PRIMITIVE_ERR_DESCRIPTOR_OUT_OF_RANGE);
    EXPECT_FAILURE(small_integer(1), small_integer(-1), string,
                   ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE);
    EXPECT_FAILURE(small_integer(1), small_integer(3), string,
                   ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE);
    EXPECT_FAILURE(small_integer(1), small_integer(ST_SMALL_INTEGER_MAX),
                   string, ST_STREAM_PRIMITIVE_ERR_COUNT_OUT_OF_RANGE);
    EXPECT_FAILURE(small_integer(1), small_integer(1), bytes,
                   ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH);
    EXPECT_FAILURE(small_integer(1), small_integer(1), st_value_nil(),
                   ST_STREAM_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(foreign != NULL && st_value_from_object(foreign, &foreign_value));
    if (foreign != NULL && foreign_value != ST_VALUE_INVALID)
        EXPECT_FAILURE(small_integer(1), small_integer(1), foreign_value,
                       ST_STREAM_PRIMITIVE_ERR_NOT_MEMBER);
    free(foreign);
#undef EXPECT_FAILURE

    arguments[0] = small_integer(1);
    arguments[1] = small_integer(0);
    arguments[2] = string;
    result = st_value_true();
    os_error = -1;
    CHECK(st_stream_write_primitive_execute(
              &context, st_value_nil(), arguments, 2u, &result, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_WRONG_ARITY);
    CHECK(result == ST_VALUE_INVALID && os_error == 0);
    arguments[0] = ST_VALUE_INVALID;
    result = st_value_true();
    CHECK(st_stream_write_primitive_execute(
              &context, st_value_nil(), arguments, 3u, &result, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_INVALID_VALUE);
    CHECK(result == ST_VALUE_INVALID && os_error == 0);
    result = st_value_true();
    os_error = -1;
    CHECK(st_stream_write_primitive_execute(
              &context, st_value_nil(), arguments, 3u, NULL, &os_error)
          == ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(os_error == 0);
    CHECK(st_stream_write_primitive_execute(
              &context, st_value_nil(), arguments, 3u, &result, NULL)
          == ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(result == ST_VALUE_INVALID);
    st_stream_primitive_context_destroy(&context);
    st_heap_destroy(&heap);
}

static void test_context_and_catalog_contract(void)
{
    descriptor_fixture_t fixture;
    st_heap_t heap = {0};
    st_stream_primitive_context_t context = {0};
    st_stream_primitive_options_t options;
    const st_primitive_spec_t *specs;
    size_t count = 0u;
    descriptor_fixture_init(&fixture);
    CHECK(st_heap_init(&heap, &fixture.descriptors,
                       (st_runtime_allocator_t){0}) == ST_HEAP_OK);
    options = (st_stream_primitive_options_t) {
        .heap = &heap,
        .string_class_id = CLASS_STRING,
        .string_shape_id = CLASS_STRING
    };
    CHECK(st_stream_primitive_context_init(&context, &options)
          == ST_STREAM_PRIMITIVE_OK);
    CHECK(st_stream_primitive_context_init(&context, &options)
          == ST_STREAM_PRIMITIVE_ERR_INVALID_ARGUMENT);
    st_stream_primitive_context_destroy(&context);
    fixture.shape_storage[CLASS_STRING - 1u].indexed_format = ST_INDEXED_UINT16;
    CHECK(st_stream_primitive_context_init(&context, &options)
          == ST_STREAM_PRIMITIVE_ERR_INVALID_DESCRIPTOR);
    fixture.shape_storage[CLASS_STRING - 1u].indexed_format = ST_INDEXED_UINT8;

    specs = st_stream_primitive_specs(&count);
    CHECK(specs != NULL && count == 1u);
    CHECK(specs[0].name_length == sizeof("StreamWritePrimitive") - 1u
          && memcmp(specs[0].name, "StreamWritePrimitive",
                    specs[0].name_length) == 0);
    CHECK(specs[0].method_arity == 3u
          && specs[0].receiver_policy == ST_PRIMITIVE_CLASS_ONLY
          && specs[0].failure_policy == ST_PRIMITIVE_FALL_THROUGH
          && specs[0].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL
          && specs[0].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID
          && specs[0].runtime_symbol_length
             == sizeof("st_aot_stream_write_primitive_execute") - 1u
          && memcmp(specs[0].runtime_symbol,
                    "st_aot_stream_write_primitive_execute",
                    sizeof("st_aot_stream_write_primitive_execute") - 1u)
             == 0);
    st_heap_destroy(&heap);
}

static const char *image_directory(void)
{
    if (access("st-image", R_OK) == 0) return "st-image";
    if (access("samples/smalltalk/st-image", R_OK) == 0)
        return "samples/smalltalk/st-image";
    return NULL;
}

static void register_specs(st_primitive_catalog_t *catalog,
                           const st_primitive_spec_t *specs, size_t count)
{
    size_t index;
    for (index = 0u; index < count; ++index)
        CHECK(st_primitive_catalog_register(catalog, &specs[index], NULL)
              == ST_PRIMITIVE_OK);
}

static void test_complete_image_resolution_counts(void)
{
    st_primitive_catalog_t catalog = {0};
    st_source_bundle_t bundle;
    st_primitive_result_t resolution;
    const st_ast_unit_t **units = NULL;
    const st_primitive_spec_t *specs;
    const char *image = image_directory();
    size_t count;
    size_t index;
    size_t float_fallbacks = 0u;
    size_t missing_implementations = 0u;
    CHECK(image != NULL);
    if (image == NULL) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) return;
    CHECK(bundle.count == 50u);
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) goto done;
    for (index = 0u; index < bundle.count; ++index)
        units[index] = &bundle.files[index].ast;
    CHECK(st_primitive_catalog_init(&catalog,
                                    (st_primitive_allocator_t){0}));
    specs = st_core_primitive_specs(&count);
    register_specs(&catalog, specs, count);
    specs = st_heap_primitive_specs(&count);
    register_specs(&catalog, specs, count);
    specs = st_float_primitive_specs(&count);
    register_specs(&catalog, specs, count);
    specs = st_stream_primitive_specs(&count);
    register_specs(&catalog, specs, count);
    CHECK(st_primitive_catalog_count(&catalog) == 46u);
    st_primitive_result_init(&resolution);
    CHECK(st_primitive_resolve(&resolution, units, bundle.count,
                               &catalog, NULL) == ST_PRIMITIVE_OK);
    CHECK(resolution.binding_count == 50u);
    CHECK(resolution.diagnostic_count == 19u);
    CHECK(resolution.binding_count + resolution.diagnostic_count == 69u);
    for (index = 0u; index < resolution.diagnostic_count; ++index) {
        const st_primitive_diagnostic_t *diagnostic
            = &resolution.diagnostics[index];
        if (diagnostic->code == ST_PRIMITIVE_DIAG_MISSING_FALLBACK)
            ++float_fallbacks;
        else if (diagnostic->code
                 == ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION)
            ++missing_implementations;
        else
            CHECK(false);
    }
    CHECK(float_fallbacks == 0u && missing_implementations == 19u);
    st_primitive_result_destroy(&resolution);
    st_primitive_catalog_destroy(&catalog);
done:
    free(units);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    test_real_pipe_binary_and_utf8();
    test_eintr_short_write_and_callback_contract();
    test_errors_types_provenance_and_bounds();
    test_context_and_catalog_contract();
    test_complete_image_resolution_counts();
    if (failures != 0u) {
        fprintf(stderr, "stream primitive regression: %u failure(s)\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("stream primitive regression: PASS");
    return EXIT_SUCCESS;
}
