#include "st_symbol_intern.h"
#include "st_lookup.h"
#include "st_send_bridge.h"
#include "st_source_bundle.h"
#include "st_string_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                        \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

static bool text_is(st_ast_string_t text, const char *expected)
{
    size_t length = strlen(expected);
    return text.length == length
        && (length == 0u || memcmp(text.data, expected, length) == 0);
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static bool unit_method_is_owned_by(const st_ast_unit_t *unit,
                                    const st_ast_node_t *method,
                                    const char *class_name)
{
    if (unit == NULL || method == NULL) return false;
    for (size_t index = 0u; index < unit->declarations.count; index++) {
        const st_ast_node_t *declaration = unit->declarations.items[index];
        if (declaration == NULL || declaration->kind != ST_AST_CLASS
                || declaration->as.class_decl.name == NULL
                || declaration->as.class_decl.name->kind
                   != ST_AST_VARIABLE
                || !text_is(
                    declaration->as.class_decl.name->as.variable.name,
                    class_name))
            continue;
        for (size_t method_index = 0u;
             method_index < declaration->as.class_decl.methods.count;
             method_index++)
            if (declaration->as.class_decl.methods.items[method_index]
                    == method)
                return true;
    }
    return false;
}

enum {
    CLASS_OBJECT = 1,
    CLASS_STRING,
    CLASS_SYMBOL,
    CLASS_OTHER,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT
};

enum {
    SHAPE_OBJECT = 1,
    SHAPE_STRING8,
    SHAPE_SYMBOL8,
    SHAPE_OTHER,
    SHAPE_NIL,
    SHAPE_FALSE,
    SHAPE_TRUE,
    SHAPE_SMALL_INTEGER,
    SHAPE_CHARACTER,
    SHAPE_METACLASS,
    SHAPE_STRING16,
    SHAPE_STRING32,
    SHAPE_SYMBOL16,
    SHAPE_SYMBOL32,
    SHAPE_COUNT
};

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live;
    bool fail_all;
} allocation_t;

static void *tracked_allocate(void *user, size_t alignment, size_t size)
{
    allocation_t *allocation = user;
    allocation->calls++;
    if (allocation->fail_all
            || (allocation->fail_on != 0u
                && allocation->calls == allocation->fail_on))
        return NULL;
    void *pointer = aligned_alloc(alignment, size);
    if (pointer != NULL) allocation->live++;
    return pointer;
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

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT - 1];
    StShapeDescriptor shape_storage[SHAPE_COUNT - 1];
    const StClassDescriptor *classes[CLASS_COUNT - 1];
    const StShapeDescriptor *shapes[SHAPE_COUNT - 1];
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_image_runtime_t image;
    st_symbol_intern_context_t symbols;
    allocation_t heap_allocation;
    allocation_t symbol_allocation;
} fixture_t;

typedef struct {
    st_heap_t *heap;
    size_t count;
} root_visit_t;

static bool count_authenticated_root(void *user, const st_value_t *root)
{
    root_visit_t *visit = user;
    CHECK(root != NULL && st_heap_contains(visit->heap, *root));
    visit->count++;
    return true;
}

static st_image_runtime_status_t foreign_empty_roots(
    void *owner, const st_value_t **roots_out, size_t *root_count_out)
{
    if (owner == NULL || roots_out == NULL || root_count_out == NULL)
        return ST_IMAGE_RUNTIME_ERR_INVALID_ARGUMENT;
    *roots_out = NULL;
    *root_count_out = 0u;
    return ST_IMAGE_RUNTIME_OK;
}

static void fixture_descriptors(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1] = {
        "Object", "String", "Symbol", "Other", "UndefinedObject",
        "False", "True", "SmallInteger", "Character", "Metaclass"
    };
    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t id = 1u; id < CLASS_COUNT; id++) {
        size_t index = (size_t)id - 1u;
        fixture->class_storage[index] = (StClassDescriptor) {
            .class_id = id,
            .superclass_id = id == CLASS_OBJECT || id == CLASS_METACLASS
                ? 0u : id == CLASS_SYMBOL ? CLASS_STRING : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id,
            .flags = id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->classes[index] = &fixture->class_storage[index];
        if (id == CLASS_SYMBOL)
            fixture->class_storage[index].flags |= ST_CLASS_ABSTRACT;
    }
    for (uint32_t id = 1u; id < SHAPE_COUNT; id++) {
        uint32_t class_id = id < CLASS_COUNT ? id : CLASS_STRING;
        if (id == SHAPE_SYMBOL16 || id == SHAPE_SYMBOL32)
            class_id = CLASS_SYMBOL;
        fixture->shape_storage[id - 1u] = (StShapeDescriptor) {
            .shape_id = id,
            .class_id = class_id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->shapes[id - 1u] = &fixture->shape_storage[id - 1u];
    }
    fixture->shape_storage[SHAPE_STRING8 - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->shape_storage[SHAPE_STRING16 - 1u].indexed_format =
        ST_INDEXED_UINT16;
    fixture->shape_storage[SHAPE_STRING32 - 1u].indexed_format =
        ST_INDEXED_UINT32;
    fixture->shape_storage[SHAPE_SYMBOL8 - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->shape_storage[SHAPE_SYMBOL16 - 1u].indexed_format =
        ST_INDEXED_UINT16;
    fixture->shape_storage[SHAPE_SYMBOL32 - 1u].indexed_format =
        ST_INDEXED_UINT32;
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, CLASS_COUNT - 1u,
        fixture->shapes, SHAPE_COUNT - 1u
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static uint64_t collide_hash(void *user,
                             const st_object_view_t *authenticated_string)
{
    CHECK(user == (void *)(uintptr_t)UINT32_C(0x51));
    CHECK(authenticated_string != NULL);
    return UINT64_C(0xfeedface);
}

static st_symbol_intern_options_t symbol_options(fixture_t *fixture)
{
    return (st_symbol_intern_options_t) {
        .image = &fixture->image,
        .string_class_id = CLASS_STRING,
        .string_uint8_shape_id = SHAPE_STRING8,
        .string_uint16_shape_id = SHAPE_STRING16,
        .string_uint32_shape_id = SHAPE_STRING32,
        .symbol_class_id = CLASS_SYMBOL,
        .symbol_uint8_shape_id = SHAPE_SYMBOL8,
        .symbol_uint16_shape_id = SHAPE_SYMBOL16,
        .symbol_uint32_shape_id = SHAPE_SYMBOL32,
        .initial_table_capacity = 4u,
        .allocator = {
            tracked_allocate, tracked_deallocate,
            &fixture->symbol_allocation
        },
        .hash = collide_hash,
        .hash_user = (void *)(uintptr_t)UINT32_C(0x51)
    };
}

static bool fixture_init(fixture_t *fixture)
{
    st_image_runtime_options_t image_options;
    st_symbol_intern_options_t intern_options;
    fixture_descriptors(fixture);
    if (st_heap_init(&fixture->heap, &fixture->descriptors,
            (st_runtime_allocator_t) {
                tracked_allocate, tracked_deallocate,
                &fixture->heap_allocation
            }) != ST_HEAP_OK)
        return false;
    image_options = (st_image_runtime_options_t) {
        .descriptors = &fixture->descriptors,
        .borrowed_heap = &fixture->heap,
        .string_layout = { CLASS_STRING, SHAPE_STRING8 }
    };
    if (st_image_runtime_init(&fixture->image, &image_options)
            != ST_IMAGE_RUNTIME_OK)
        return false;
    intern_options = symbol_options(fixture);
    return st_symbol_intern_context_init(&fixture->symbols, &intern_options)
        == ST_SYMBOL_INTERN_OK;
}

static bool fixture_init_default_hash(fixture_t *fixture)
{
    st_image_runtime_options_t image_options;
    st_symbol_intern_options_t intern_options;
    fixture_descriptors(fixture);
    if (st_heap_init(&fixture->heap, &fixture->descriptors,
            (st_runtime_allocator_t) {
                tracked_allocate, tracked_deallocate,
                &fixture->heap_allocation
            }) != ST_HEAP_OK)
        return false;
    image_options = (st_image_runtime_options_t) {
        .descriptors = &fixture->descriptors,
        .borrowed_heap = &fixture->heap,
        .string_layout = { CLASS_STRING, SHAPE_STRING8 }
    };
    if (st_image_runtime_init(&fixture->image, &image_options)
            != ST_IMAGE_RUNTIME_OK)
        return false;
    intern_options = symbol_options(fixture);
    intern_options.hash = NULL;
    intern_options.hash_user = NULL;
    return st_symbol_intern_context_init(&fixture->symbols, &intern_options)
        == ST_SYMBOL_INTERN_OK;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_symbol_intern_context_destroy(&fixture->symbols);
    st_image_runtime_destroy(&fixture->image);
    st_heap_destroy(&fixture->heap);
    CHECK(fixture->symbol_allocation.live == 0u);
    CHECK(fixture->heap_allocation.live == 0u);
}

static st_value_t make_sequence(fixture_t *fixture, uint32_t class_id,
                                uint32_t shape_id,
                                const uint32_t *code_points, size_t count,
                                st_header_flags_t flags)
{
    st_value_t value = ST_VALUE_INVALID;
    st_object_view_t view;
    CHECK(st_heap_allocate(&fixture->heap, class_id, shape_id,
                           count, count, flags, &value) == ST_HEAP_OK);
    CHECK(st_heap_object_view(&fixture->heap, value, &view) == ST_HEAP_OK);
    if (view.shape_descriptor->indexed_format == ST_INDEXED_UINT8) {
        for (size_t index = 0u; index < count; index++)
            ((uint8_t *)view.indexed_elements)[index] =
                (uint8_t)code_points[index];
    } else if (view.shape_descriptor->indexed_format == ST_INDEXED_UINT16) {
        for (size_t index = 0u; index < count; index++)
            ((uint16_t *)view.indexed_elements)[index] =
                (uint16_t)code_points[index];
    } else {
        for (size_t index = 0u; index < count; index++)
            ((uint32_t *)view.indexed_elements)[index] = code_points[index];
    }
    return value;
}

static void check_symbol_shape(fixture_t *fixture, st_value_t value,
                               uint32_t expected_shape)
{
    st_object_view_t view;
    CHECK(st_heap_object_view(&fixture->heap, value, &view) == ST_HEAP_OK);
    CHECK(view.class_descriptor->class_id == CLASS_SYMBOL
          && view.shape_descriptor->shape_id == expected_shape
          && (st_object_header_flags(
              st_object_header_load(&view.object->header))
              & ST_HEADER_IMMUTABLE) != 0u);
}

static void test_identity_width_canonicalization_and_growth(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    if (!fixture.symbols.initialized) return;
    const uint32_t ascii[] = { 'a', 'l', 'p', 'h', 'a' };
    st_value_t string8 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING8, ascii, 5u, 0u);
    st_value_t string16 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING16, ascii, 5u, 0u);
    st_value_t string32 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, ascii, 5u, 0u);
    st_value_t first = ST_VALUE_INVALID;
    st_value_t second = ST_VALUE_INVALID;
    st_value_t third = ST_VALUE_INVALID;
    CHECK(st_symbol_intern(&fixture.symbols, string8, &first)
          == ST_SYMBOL_INTERN_OK);
    CHECK(st_symbol_intern(&fixture.symbols, string8, &second)
          == ST_SYMBOL_INTERN_OK && second == first);
    CHECK(st_symbol_intern(&fixture.symbols, string16, &second)
          == ST_SYMBOL_INTERN_OK && second == first);
    CHECK(st_symbol_intern(&fixture.symbols, string32, &third)
          == ST_SYMBOL_INTERN_OK && third == first);
    check_symbol_shape(&fixture, first, SHAPE_SYMBOL8);
    CHECK(st_symbol_intern(&fixture.symbols, first, &second)
          == ST_SYMBOL_INTERN_OK && second == first);

    const uint32_t medium[] = { UINT32_C(0x100), 'x' };
    st_value_t medium16 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING16, medium, 2u, 0u);
    st_value_t medium32 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, medium, 2u, 0u);
    st_value_t medium_symbol;
    CHECK(st_symbol_intern(&fixture.symbols, medium16, &medium_symbol)
          == ST_SYMBOL_INTERN_OK);
    CHECK(st_symbol_intern(&fixture.symbols, medium32, &second)
          == ST_SYMBOL_INTERN_OK && second == medium_symbol);
    check_symbol_shape(&fixture, medium_symbol, SHAPE_SYMBOL16);

    const uint32_t wide[] = { UINT32_C(0x1f642) };
    st_value_t wide_string = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, wide, 1u, 0u);
    st_value_t wide_symbol;
    CHECK(st_symbol_intern(&fixture.symbols, wide_string, &wide_symbol)
          == ST_SYMBOL_INTERN_OK);
    check_symbol_shape(&fixture, wide_symbol, SHAPE_SYMBOL32);
    CHECK(st_symbol_intern_count(&fixture.symbols) == 3u);

    for (uint32_t integer = 0u; integer < 80u; integer++) {
        uint32_t sequence[] = { 'k', integer };
        st_value_t input = make_sequence(
            &fixture, CLASS_STRING, SHAPE_STRING8, sequence, 2u, 0u);
        st_value_t symbol;
        CHECK(st_symbol_intern(&fixture.symbols, input, &symbol)
              == ST_SYMBOL_INTERN_OK);
        CHECK(st_symbol_intern(&fixture.symbols, input, &second)
              == ST_SYMBOL_INTERN_OK && second == symbol);
    }
    size_t capacity = st_symbol_intern_table_capacity(&fixture.symbols);
    CHECK(st_symbol_intern_count(&fixture.symbols) == 83u
          && capacity > 4u && (capacity & (capacity - 1u)) == 0u
          && st_symbol_intern_count(&fixture.symbols) * 4u
             <= capacity * 3u);
    fixture_destroy(&fixture);
}

static void test_production_hash_is_width_independent(void)
{
    fixture_t fixture;
    CHECK(fixture_init_default_hash(&fixture));
    if (!fixture.symbols.initialized) return;
    const uint32_t text[] = {
        'h', UINT32_C(0xe9), UINT32_C(0x20ac), UINT32_C(0x1f642)
    };
    st_value_t string16 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING16, text, 3u, 0u);
    st_value_t string32_prefix = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, text, 3u, 0u);
    st_value_t symbol16;
    st_value_t same;
    CHECK(st_symbol_intern(&fixture.symbols, string16, &symbol16)
          == ST_SYMBOL_INTERN_OK);
    CHECK(st_symbol_intern(&fixture.symbols, string32_prefix, &same)
          == ST_SYMBOL_INTERN_OK && same == symbol16);
    check_symbol_shape(&fixture, symbol16, SHAPE_SYMBOL16);
    st_value_t string32 = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, text, 4u, 0u);
    st_value_t wide;
    CHECK(st_symbol_intern(&fixture.symbols, string32, &wide)
          == ST_SYMBOL_INTERN_OK && wide != symbol16);
    check_symbol_shape(&fixture, wide, SHAPE_SYMBOL32);
    fixture_destroy(&fixture);
}

static void test_validation_oom_transaction_and_gc_roots(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    if (!fixture.symbols.initialized) return;
    st_value_t result = st_value_nil();
    const uint32_t bad_unicode[] = { UINT32_C(0xd800) };
    st_value_t invalid = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING32, bad_unicode, 1u, 0u);
    CHECK(st_symbol_intern(&fixture.symbols, invalid, &result)
          == ST_SYMBOL_INTERN_ERR_BAD_OBJECT
          && result == ST_VALUE_INVALID);
    uint32_t other_data[] = { 1u };
    st_value_t other = make_sequence(
        &fixture, CLASS_OTHER, SHAPE_OTHER, other_data, 0u, 0u);
    CHECK(st_symbol_intern(&fixture.symbols, other, &result)
          == ST_SYMBOL_INTERN_ERR_TYPE_MISMATCH
          && result == ST_VALUE_INVALID);
    CHECK(st_symbol_intern(&fixture.symbols, UINT64_C(0x1000), &result)
          == ST_SYMBOL_INTERN_ERR_NOT_MEMBER
          && result == ST_VALUE_INVALID);
    CHECK(st_symbol_intern(&fixture.symbols, UINT64_C(5), &result)
          == ST_SYMBOL_INTERN_ERR_INVALID_VALUE
          && result == ST_VALUE_INVALID);
    const uint32_t mutable_data[] = { 'm' };
    st_value_t mutable_symbol = make_sequence(
        &fixture, CLASS_SYMBOL, SHAPE_SYMBOL8, mutable_data, 1u, 0u);
    CHECK(st_symbol_intern(&fixture.symbols, mutable_symbol, &result)
          == ST_SYMBOL_INTERN_ERR_BAD_OBJECT);

    for (uint32_t value = 1u; value <= 3u; value++) {
        uint32_t sequence[] = { value };
        st_value_t input = make_sequence(
            &fixture, CLASS_STRING, SHAPE_STRING8, sequence, 1u, 0u);
        CHECK(st_symbol_intern(&fixture.symbols, input, &result)
              == ST_SYMBOL_INTERN_OK);
    }
    CHECK(st_symbol_intern_count(&fixture.symbols) == 3u
          && st_symbol_intern_table_capacity(&fixture.symbols) == 4u);
    uint32_t grow_data[] = { 4u };
    st_value_t grow_input = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING8, grow_data, 1u, 0u);
    size_t baseline_live = fixture.symbol_allocation.live;
    fixture.symbol_allocation.fail_on = fixture.symbol_allocation.calls + 1u;
    CHECK(st_symbol_intern(&fixture.symbols, grow_input, &result)
          == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY
          && result == ST_VALUE_INVALID
          && st_symbol_intern_count(&fixture.symbols) == 3u
          && st_symbol_intern_table_capacity(&fixture.symbols) == 4u
          && fixture.symbol_allocation.live == baseline_live);
    fixture.symbol_allocation.fail_on = fixture.symbol_allocation.calls + 2u;
    CHECK(st_symbol_intern(&fixture.symbols, grow_input, &result)
          == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY
          && st_symbol_intern_count(&fixture.symbols) == 3u
          && st_symbol_intern_table_capacity(&fixture.symbols) == 4u
          && fixture.symbol_allocation.live == baseline_live);
    fixture.symbol_allocation.fail_on = 0u;
    fixture.heap_allocation.fail_all = true;
    CHECK(st_symbol_intern(&fixture.symbols, grow_input, &result)
          == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY
          && st_symbol_intern_count(&fixture.symbols) == 3u
          && st_symbol_intern_table_capacity(&fixture.symbols) == 4u
          && fixture.symbol_allocation.live == baseline_live);
    fixture.heap_allocation.fail_all = false;
    CHECK(st_symbol_intern(&fixture.symbols, grow_input, &result)
          == ST_SYMBOL_INTERN_OK
          && st_symbol_intern_count(&fixture.symbols) == 4u
          && st_symbol_intern_table_capacity(&fixture.symbols) == 8u);
    st_value_t retained_symbol = result;
    st_heap_collection_stats_t stats;
    CHECK(st_image_runtime_collect(&fixture.image, NULL, &stats)
          == ST_IMAGE_RUNTIME_OK);
    CHECK(st_heap_contains(&fixture.heap, retained_symbol)
          && !st_heap_contains(&fixture.heap, grow_input));
    size_t visited = 0u;
    root_visit_t visit = { &fixture.heap, 0u };
    CHECK(st_image_runtime_visit_roots(
              &fixture.image, count_authenticated_root, &visit, &visited)
          == ST_IMAGE_RUNTIME_OK
          && visited == st_symbol_intern_count(&fixture.symbols)
          && visit.count == visited);
    fixture_destroy(&fixture);
}

static void test_batch_prepare_commit_abort_and_utf8(void)
{
    fixture_t fixture;
    st_symbol_intern_batch_t batch = {0};
    const st_symbol_utf8_t spellings[] = {
        { "alpha", 5u },
        { "alpha", 5u },
        { "\xe2\x82\xac", 3u },
        { "\xf0\x9f\x99\x82", 4u }
    };
    size_t count = 99u;
    CHECK(fixture_init(&fixture));
    if (!fixture.symbols.initialized) return;
    CHECK(st_symbol_intern_batch_prepare_utf8(
              &batch, &fixture.symbols, spellings,
              sizeof(spellings) / sizeof(spellings[0]))
          == ST_SYMBOL_INTERN_OK);
    const st_value_t *values = st_symbol_intern_batch_values(&batch, &count);
    CHECK(values != NULL && count == 4u && values[0] == values[1]
          && values[0] != values[2] && values[2] != values[3]
          && st_symbol_intern_count(&fixture.symbols) == 0u);
    check_symbol_shape(&fixture, values[0], SHAPE_SYMBOL8);
    check_symbol_shape(&fixture, values[2], SHAPE_SYMBOL16);
    check_symbol_shape(&fixture, values[3], SHAPE_SYMBOL32);
    fixture.symbol_allocation.fail_all = true;
    fixture.heap_allocation.fail_all = true;
    CHECK(st_symbol_intern_batch_commit(&batch) == ST_SYMBOL_INTERN_OK
          && st_symbol_intern_count(&fixture.symbols) == 3u);
    fixture.symbol_allocation.fail_all = false;
    fixture.heap_allocation.fail_all = false;
    st_value_t alpha = values[0];
    st_symbol_intern_batch_destroy(&batch);

    const uint32_t alpha_code_points[] = { 'a', 'l', 'p', 'h', 'a' };
    st_value_t alpha_string = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING8, alpha_code_points, 5u, 0u);
    st_value_t same = ST_VALUE_INVALID;
    CHECK(st_symbol_intern(&fixture.symbols, alpha_string, &same)
          == ST_SYMBOL_INTERN_OK && same == alpha);

    const st_symbol_utf8_t abort_spelling = { "discard", 7u };
    size_t objects_before = st_heap_object_count(&fixture.heap);
    CHECK(st_symbol_intern_batch_prepare_utf8(
              &batch, &fixture.symbols, &abort_spelling, 1u)
          == ST_SYMBOL_INTERN_OK
          && st_symbol_intern_count(&fixture.symbols) == 3u
          && st_heap_object_count(&fixture.heap) > objects_before);
    st_symbol_intern_batch_destroy(&batch);
    st_heap_collection_stats_t stats;
    CHECK(st_image_runtime_collect(&fixture.image, NULL, &stats)
          == ST_IMAGE_RUNTIME_OK
          && st_symbol_intern_count(&fixture.symbols) == 3u
          && st_heap_contains(&fixture.heap, alpha));

    const st_symbol_utf8_t malformed = { "\xc0\x80", 2u };
    CHECK(st_symbol_intern_batch_prepare_utf8(
              &batch, &fixture.symbols, &malformed, 1u)
          == ST_SYMBOL_INTERN_ERR_INVALID_VALUE
          && batch.state == NULL
          && st_symbol_intern_count(&fixture.symbols) == 3u);
    fixture_destroy(&fixture);
}

static void test_init_oom_and_aot_bridge(void)
{
    for (size_t fail = 1u; fail <= 3u; fail++) {
        fixture_t fixture;
        fixture_descriptors(&fixture);
        CHECK(st_heap_init(&fixture.heap, &fixture.descriptors,
            (st_runtime_allocator_t) {
                tracked_allocate, tracked_deallocate,
                &fixture.heap_allocation
            }) == ST_HEAP_OK);
        st_image_runtime_options_t image_options = {
            .descriptors = &fixture.descriptors,
            .borrowed_heap = &fixture.heap
        };
        CHECK(st_image_runtime_init(&fixture.image, &image_options)
              == ST_IMAGE_RUNTIME_OK);
        fixture.symbol_allocation.fail_on = fail;
        st_symbol_intern_options_t options = symbol_options(&fixture);
        CHECK(st_symbol_intern_context_init(&fixture.symbols, &options)
              == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY);
        CHECK(!fixture.symbols.initialized
              && st_image_runtime_root_provider(&fixture.image) == NULL
              && fixture.symbol_allocation.live == 0u);
        st_image_runtime_destroy(&fixture.image);
        st_heap_destroy(&fixture.heap);
        CHECK(fixture.heap_allocation.live == 0u);
    }

    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    if (!fixture.symbols.initialized) return;
    st_lookup_context_t lookup = {0};
    st_aot_thread_t thread = {0};
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    StMethodDescriptor method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_STRING
    };
    const uint32_t word[] = { 'b', 'r', 'i', 'd', 'g', 'e' };
    st_value_t input = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING8, word, 6u, 0u);
    st_value_t result = st_value_true();
    uint32_t detail = 99u;
    CHECK(st_lookup_context_init(&lookup, &fixture.descriptors,
                                 (st_lookup_allocator_t){0})
          == ST_LOOKUP_FOUND);
    CHECK(st_aot_thread_init(&thread, &lookup, immediate,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    StFrame frame = {
        .thread = &thread,
        .method = &method,
        .receiver = input
    };
    CHECK(st_aot_string_as_symbol_primitive_execute(
              &frame, input, NULL, 0u, &result, &detail)
          == (uint32_t)ST_SYMBOL_INTERN_ERR_INVALID_STATE
          && result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_image_attach(&thread, &fixture.image));
    CHECK(st_aot_string_as_symbol_primitive_execute(
              &frame, input, NULL, 0u, &result, &detail)
          == (uint32_t)ST_SYMBOL_INTERN_OK
          && result != ST_VALUE_INVALID && detail == 0u);
    st_value_t same = ST_VALUE_INVALID;
    CHECK(st_aot_string_as_symbol_primitive_execute(
              &frame, input, NULL, 0u, &same, &detail)
          == (uint32_t)ST_SYMBOL_INTERN_OK && same == result);
    CHECK(st_aot_thread_image_detach(&thread, &fixture.image));
    st_aot_thread_destroy(&thread);
    st_lookup_context_destroy(&lookup);
    fixture_destroy(&fixture);
}

static void test_foreign_provider_conflict_and_bridge_rejection(void)
{
    fixture_t fixture;
    fixture_descriptors(&fixture);
    CHECK(st_heap_init(&fixture.heap, &fixture.descriptors,
        (st_runtime_allocator_t) {
            tracked_allocate, tracked_deallocate, &fixture.heap_allocation
        }) == ST_HEAP_OK);
    st_image_runtime_options_t image_options = {
        .descriptors = &fixture.descriptors,
        .borrowed_heap = &fixture.heap
    };
    CHECK(st_image_runtime_init(&fixture.image, &image_options)
          == ST_IMAGE_RUNTIME_OK);
    st_image_root_provider_t foreign_provider = {
        ST_IMAGE_ROOT_PROVIDER_ABI_VERSION,
        &fixture,
        foreign_empty_roots
    };
    CHECK(st_image_runtime_root_provider_attach(
              &fixture.image, &foreign_provider));
    st_symbol_intern_options_t options = symbol_options(&fixture);
    CHECK(st_symbol_intern_context_init(&fixture.symbols, &options)
          == ST_SYMBOL_INTERN_OK);
    CHECK(fixture.symbols.initialized
          && st_image_runtime_root_provider_contains(
              &fixture.image, &foreign_provider)
          && st_image_runtime_root_provider_contains(
              &fixture.image, &fixture.symbols.root_provider));

    st_lookup_context_t lookup = {0};
    st_aot_thread_t thread = {0};
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    StMethodDescriptor method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = 1u,
        .owner_class_id = CLASS_STRING
    };
    const uint32_t text[] = { 'x' };
    st_value_t input = make_sequence(
        &fixture, CLASS_STRING, SHAPE_STRING8, text, 1u, 0u);
    st_value_t result = st_value_true();
    uint32_t detail = 99u;
    CHECK(st_lookup_context_init(&lookup, &fixture.descriptors,
                                 (st_lookup_allocator_t){0})
          == ST_LOOKUP_FOUND);
    CHECK(st_aot_thread_init(&thread, &lookup, immediate,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    CHECK(st_aot_thread_image_attach(&thread, &fixture.image));
    StFrame frame = {
        .thread = &thread,
        .method = &method,
        .receiver = input
    };
    CHECK(st_aot_string_as_symbol_primitive_execute(
              &frame, input, NULL, 0u, &result, &detail)
          == (uint32_t)ST_SYMBOL_INTERN_OK
          && st_heap_contains(&fixture.heap, result) && detail == 0u);
    CHECK(st_aot_thread_image_detach(&thread, &fixture.image));
    st_aot_thread_destroy(&thread);
    st_lookup_context_destroy(&lookup);
    st_symbol_intern_context_destroy(&fixture.symbols);
    CHECK(st_image_runtime_root_provider_detach(
              &fixture.image, &foreign_provider));
    st_image_runtime_destroy(&fixture.image);
    st_heap_destroy(&fixture.heap);
    CHECK(fixture.symbol_allocation.live == 0u
          && fixture.heap_allocation.live == 0u);
}

static void test_real_image_catalog_binding(void)
{
    const char *image = first_existing(
        "st-image", "samples/smalltalk/st-image");
    st_source_bundle_t bundle;
    const st_ast_unit_t **units = NULL;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_primitive_spec_t *specs;
    const st_primitive_t *registered;
    size_t spec_count = 0u;
    size_t binding_count = 0u;
    size_t relevant_diagnostics = 0u;
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
    CHECK(st_primitive_catalog_init(&catalog,
                                    (st_primitive_allocator_t){0}));
    specs = st_string_primitive_specs(&spec_count);
    CHECK(specs != NULL && spec_count == 3u);
    for (size_t index = 0u; index < spec_count; index++)
        CHECK(st_primitive_catalog_register(
                  &catalog, &specs[index], NULL) == ST_PRIMITIVE_OK);
    registered = st_primitive_catalog_lookup(
        &catalog, "StringAsSymbolPrimitive",
        sizeof("StringAsSymbolPrimitive") - 1u);
    CHECK(registered != NULL
          && registered->implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL
          && registered->intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID
          && registered->failure_policy == ST_PRIMITIVE_FALL_THROUGH
          && text_is(registered->runtime_symbol,
                     "st_aot_string_as_symbol_primitive_execute"));
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, bundle.count, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    for (size_t index = 0u; index < result.binding_count; index++) {
        const st_primitive_binding_t *binding = &result.bindings[index];
        if (binding->primitive != registered) continue;
        binding_count++;
        CHECK(binding->method != NULL
              && binding->method->kind == ST_AST_METHOD
              && binding->unit_index < bundle.count
              && unit_method_is_owned_by(units[binding->unit_index],
                                         binding->method, "String")
              && text_is(binding->method->as.method.selector, "asSymbol")
              && !binding->method->as.method.class_side
              && binding->method->as.method.body != NULL
              && binding->method->as.method.body->kind == ST_AST_BLOCK
              && binding->method->as.method.body->as.block.expressions.count
                 != 0u);
    }
    for (size_t index = 0u; index < result.diagnostic_count; index++) {
        const st_primitive_diagnostic_t *diagnostic =
            &result.diagnostics[index];
        if (text_is(diagnostic->requested_name,
                    "StringAsSymbolPrimitive")) {
            CHECK(diagnostic->code != ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION
                  && diagnostic->code
                     != ST_PRIMITIVE_DIAG_MISSING_FALLBACK);
            relevant_diagnostics++;
        }
    }
    CHECK(binding_count == 1u && relevant_diagnostics == 0u);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    test_identity_width_canonicalization_and_growth();
    test_production_hash_is_width_independent();
    test_validation_oom_transaction_and_gc_roots();
    test_batch_prepare_commit_abort_and_utf8();
    test_init_oom_and_aot_bridge();
    test_foreign_provider_conflict_and_bridge_rejection();
    test_real_image_catalog_binding();
    CHECK(strcmp(st_symbol_intern_status_string(
                     ST_SYMBOL_INTERN_ERR_BAD_OBJECT),
                 "malformed String/Symbol or Unicode scalar") == 0);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk Symbol interner: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk Symbol interner: PASS (Robin Hood identity, cross-width Unicode, transactional OOM, precise roots)");
    return 0;
}
