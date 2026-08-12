#include "st_image_layout.h"
#include "st_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct {
    st_ast_unit_t unit;
    st_parser_t parser;
    st_class_graph_result_t graph;
    const st_ast_unit_t *units[1];
} fixture_t;

typedef struct allocation_header {
    size_t size;
} allocation_header_t;

typedef struct {
    size_t fail_after;
    size_t successes;
    size_t live;
} failing_allocator_t;

static bool text_is(st_ast_string_t text, const char *expected)
{
    size_t length = strlen(expected);
    return text.length == length
        && (length == 0u || memcmp(text.data, expected, length) == 0);
}

static bool fixture_init(fixture_t *fixture, const char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    if (!st_ast_unit_init(&fixture->unit, "layout.st")
            || !st_parser_init_cstr(&fixture->parser, &fixture->unit, source)
            || !st_parse_compilation_unit(&fixture->parser)
            || !st_parser_at_end(&fixture->parser)) {
        return false;
    }

    fixture->units[0] = &fixture->unit;
    st_class_graph_result_init(&fixture->graph);
    return st_class_graph_build(&fixture->graph, fixture->units, 1u, NULL)
               == ST_CLASS_GRAPH_OK
        && st_class_graph_succeeded(&fixture->graph);
}

static void fixture_destroy(fixture_t *fixture)
{
    st_class_graph_result_destroy(&fixture->graph);
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
    memset(fixture, 0, sizeof(*fixture));
}

static const st_class_graph_entity_t *find_entity(
    const st_class_graph_result_t *graph, st_class_graph_entity_kind_t kind,
    const char *name)
{
    for (size_t index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];

        if (entity->kind == kind && text_is(entity->name, name)) {
            return entity;
        }
    }
    return NULL;
}

static void *failing_allocate(void *user, size_t size)
{
    failing_allocator_t *state = user;
    allocation_header_t *header;

    if (state->successes == state->fail_after
            || size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }

    header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }

    header->size = size;
    state->successes++;
    state->live++;
    return header + 1;
}

static void failing_deallocate(void *user, void *pointer)
{
    failing_allocator_t *state = user;

    if (pointer == NULL) {
        return;
    }

    CHECK(state->live != 0u);
    state->live--;
    free((allocation_header_t *)pointer - 1);
}

static void test_complete_contract(void)
{
    static const char source[] =
        "Object := nil [ ]\n"
        "Class := Object [ <classObjectLayout: true> | name | ]\n"
        "AbstractBase := Object [ <abstract: true> ]\n"
        "Concrete := AbstractBase [ ]\n"
        "String := Object [ <shape: indexedUInt8 default: true> "
        "<shape: indexedUInt16> <shape: indexedUInt32> ]\n"
        "Symbol := String [ ]\n"
        "Block := Object [ <shape: closure> <abstract: true> ]\n"
        "ClosureCell := Object [ <shape: cell> <abstract: true> ]\n"
        "LargeInteger := Object [ <shape: largeInteger> <abstract: true> ]\n"
        "LargePositiveInteger := LargeInteger [ ]\n"
        "Fraction := Object [ | numerator denominator | ]\n"
        "FloatBox := Object [ <shape: boxedFloat64> ]\n"
        "Space := Namespace [ Namespaced := Object [ ] ]\n";
    fixture_t fixture;
    st_image_layout_result_t layout;
    CHECK(fixture_init(&fixture, source));
    st_image_layout_result_init(&layout);
    CHECK(st_image_layout_build(&layout, &fixture.graph, NULL)
          == ST_IMAGE_LAYOUT_OK);
    const st_class_graph_entity_t *class_entity = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Class");
    const st_class_graph_entity_t *concrete = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Concrete");
    const st_class_graph_entity_t *symbol = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Symbol");
    const st_class_graph_entity_t *large = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "LargePositiveInteger");
    const st_class_graph_entity_t *fraction = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Fraction");
    const st_class_graph_entity_t *block = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Block");
    const st_class_graph_entity_t *cell = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "ClosureCell");
    const st_class_graph_entity_t *space = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_NAMESPACE, "Space");
    const st_class_graph_entity_t *namespaced = find_entity(
        &fixture.graph, ST_CLASS_GRAPH_CLASS, "Namespaced");

    CHECK(class_entity && concrete && symbol && large && fraction && block && cell
          && space && namespaced);
    CHECK(st_image_layout_runtime_class_id(&layout, space->id) == 0u);
    CHECK(st_image_layout_runtime_class_id(&layout, namespaced->id)
          < namespaced->id);
    const st_image_runtime_class_layout_t *concrete_layout =
        st_image_layout_class(&layout,
            st_image_layout_runtime_class_id(&layout, concrete->id));
    const st_image_runtime_class_layout_t *symbol_layout =
        st_image_layout_class(&layout,
            st_image_layout_runtime_class_id(&layout, symbol->id));

    /* ABSTRACT is a declaration property; subclasses do not inherit it. */
    CHECK(concrete_layout && (concrete_layout->flags & ST_CLASS_ABSTRACT) == 0u);
    CHECK(symbol_layout && symbol_layout->shape_count == 3u);
    for (size_t index = 0u; symbol_layout && index < 3u; index++) {
        const st_image_runtime_shape_layout_t *shape =
            &layout.shapes[symbol_layout->shape_offset + index];
        CHECK(shape->runtime_class_id == symbol_layout->runtime_class_id);
        CHECK(shape->runtime_shape_id != 0u);
    }
    const st_image_runtime_class_layout_t *large_layout =
        st_image_layout_class(&layout,
            st_image_layout_runtime_class_id(&layout, large->id));
    const st_image_runtime_shape_layout_t *large_shape = large_layout
        ? st_image_layout_shape(&layout, large_layout->default_shape_id) : NULL;
    CHECK(large_shape && large_shape->fixed_word_count == 1u
          && large_shape->indexed_format == ST_INDEXED_UINT32
          && large_shape->bitmap_word_count == 1u
          && layout.pointer_bitmaps[large_shape->bitmap_offset] == 0u);
    const st_image_runtime_class_layout_t *fraction_layout =
        st_image_layout_class(
            &layout,
            st_image_layout_runtime_class_id(&layout, fraction->id));
    const st_image_runtime_shape_layout_t *fraction_shape = fraction_layout
        ? st_image_layout_shape(&layout, fraction_layout->default_shape_id)
        : NULL;
    CHECK(fraction_shape && fraction_shape->fixed_word_count == 2u
          && fraction_shape->indexed_format == ST_INDEXED_NONE
          && fraction_shape->bitmap_word_count == 1u
          && layout.pointer_bitmaps[fraction_shape->bitmap_offset]
              == UINT64_C(3));
    const st_image_runtime_class_layout_t *block_layout =
        st_image_layout_class(
            &layout,
            st_image_layout_runtime_class_id(&layout, block->id));
    CHECK(block_layout != NULL
          && (block_layout->flags & ST_CLASS_ABSTRACT) != 0u);
    const st_image_runtime_class_layout_t *cell_layout =
        st_image_layout_class(
            &layout, st_image_layout_runtime_class_id(&layout, cell->id));
    const st_image_runtime_shape_layout_t *cell_shape = cell_layout
        ? st_image_layout_shape(&layout, cell_layout->default_shape_id) : NULL;
    CHECK(cell_layout && (cell_layout->flags & ST_CLASS_ABSTRACT) != 0u);
    CHECK(cell_shape && cell_shape->fixed_word_count == 1u
          && cell_shape->indexed_format == ST_INDEXED_NONE
          && cell_shape->bitmap_word_count == 1u
          && layout.pointer_bitmaps[cell_shape->bitmap_offset] == UINT64_C(1));
    for (size_t index = 0u; index < fixture.graph.entity_count; index++) {
        const st_class_graph_entity_t *entity = &fixture.graph.entities[index];

        if (entity->kind != ST_CLASS_GRAPH_METACLASS) {
            continue;
        }

        const st_image_runtime_class_layout_t *meta = st_image_layout_class(
            &layout, st_image_layout_runtime_class_id(&layout, entity->id));
        const st_image_runtime_shape_layout_t *shape = meta
            ? st_image_layout_shape(&layout, meta->default_shape_id) : NULL;

        uint32_t expected_super = entity->superclass_id == 0u
            ? st_image_layout_runtime_class_id(&layout, class_entity->id)
            : st_image_layout_runtime_class_id(
                &layout, entity->superclass_id);

        /* The metaclass identity remains a finite self-knot, while the
         * superclass chain is the real Smalltalk bridge: a root metaclass
         * inherits from Class and therefore from Behavior. */
        CHECK(meta && meta->metaclass_id == meta->runtime_class_id);
        CHECK(meta && meta->superclass_id == expected_super);
        CHECK(shape
              && shape->fixed_word_count == class_entity->instance_slot_count);
    }
    st_image_layout_result_destroy(&layout);
    fixture_destroy(&fixture);
}

static void expect_status(const char *extra_source,
                          st_image_layout_status_t expected)
{
    static const char prefix[] =
        "Object := nil [ ]\n"
        "Class := Object [ <classObjectLayout: true> | name | ]\n";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t extra_length = strlen(extra_source);
    char *source = malloc(prefix_length + extra_length + 1u);
    fixture_t fixture;
    st_image_layout_result_t layout;

    CHECK(source != NULL);
    if (source == NULL) {
        return;
    }

    memcpy(source, prefix, prefix_length);
    memcpy(source + prefix_length, extra_source, extra_length + 1u);
    CHECK(fixture_init(&fixture, source));
    st_image_layout_result_init(&layout);
    CHECK(st_image_layout_build(&layout, &fixture.graph, NULL) == expected);
    CHECK(layout.implementation == NULL && layout.classes == NULL
          && layout.shapes == NULL);
    st_image_layout_result_destroy(&layout);
    fixture_destroy(&fixture);
    free(source);
}

static void test_invalid_contracts(void)
{
    expect_status("Bad := Object [ <shape: mystery> ]\n",
                  ST_IMAGE_LAYOUT_ERR_UNKNOWN_RECIPE);
    expect_status("Bad := Object [ <shape: indexedUInt8> "
                  "<shape: indexedUInt8> ]\n",
                  ST_IMAGE_LAYOUT_ERR_DUPLICATE_RECIPE);
    expect_status("Bad := Object [ <shape: indexedUInt8 default: true> "
                  "<shape: indexedUInt16 default: true> ]\n",
                  ST_IMAGE_LAYOUT_ERR_DUPLICATE_DEFAULT);
    expect_status("Bad := Object [ <shape: indexedUInt8> "
                  "<shape: indexedUInt16> ]\n",
                  ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT);
    expect_status("Bad := Object [ <shape: closure> | illegal | ]\n",
                  ST_IMAGE_LAYOUT_ERR_INCOMPATIBLE_SLOTS);
    expect_status("Bad := Object [ <shape: cell> | illegal | ]\n",
                  ST_IMAGE_LAYOUT_ERR_INCOMPATIBLE_SLOTS);
    expect_status("Bad := Object [ <abstract: indexedUInt8> ]\n",
                  ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA);
    expect_status("Bad := Object [ <abstract: true> <abstract: false> ]\n",
                  ST_IMAGE_LAYOUT_ERR_DUPLICATE_ABSTRACT);
    expect_status("OtherClass := Object [ <classObjectLayout: true> ]\n",
                  ST_IMAGE_LAYOUT_ERR_DUPLICATE_CLASS_OBJECT_LAYOUT);

    fixture_t fixture;
    st_image_layout_result_t layout;
    CHECK(fixture_init(&fixture, "Object := nil [ ]\n"));
    st_image_layout_result_init(&layout);
    CHECK(st_image_layout_build(&layout, &fixture.graph, NULL)
          == ST_IMAGE_LAYOUT_ERR_MISSING_CLASS_OBJECT_LAYOUT);
    st_image_layout_result_destroy(&layout);
    fixture_destroy(&fixture);
}

static void test_fault_injection(void)
{
    static const char source[] =
        "Object := nil [ ]\n"
        "Class := Object [ <classObjectLayout: true> | name | ]\n"
        "String := Object [ <shape: indexedUInt8 default: true> "
        "<shape: indexedUInt16> <shape: indexedUInt32> ]\n"
        "Symbol := String [ ]\n";
    fixture_t fixture;
    bool saw_success = false;
    CHECK(fixture_init(&fixture, source));
    for (size_t fail_after = 0u; fail_after < 256u && !saw_success;
         fail_after++) {
        failing_allocator_t allocator = { .fail_after = fail_after };
        st_image_layout_options_t options = {
            .allocator = {
                failing_allocate, failing_deallocate, &allocator
            }
        };
        st_image_layout_result_t layout;
        st_image_layout_result_init(&layout);
        st_image_layout_status_t status = st_image_layout_build(
            &layout, &fixture.graph, &options);
        saw_success = status == ST_IMAGE_LAYOUT_OK;
        CHECK(status == ST_IMAGE_LAYOUT_OK
              || status == ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY);
        st_image_layout_result_destroy(&layout);
        CHECK(allocator.live == 0u);
    }
    CHECK(saw_success);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_complete_contract();
    test_invalid_contracts();
    test_fault_injection();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk image layout: %u failure(s)\n", failures);
        return 1;
    }
    puts("smalltalk image layout: PASS");
    return 0;
}
