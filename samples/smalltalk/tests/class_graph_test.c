#include "st_class_graph.h"
#include "st_parser.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct {
    st_ast_unit_t unit;
    st_parser_t parser;
} fixture_t;

static bool text_is(st_ast_string_t value, const char *expected)
{
    size_t length = strlen(expected);
    return value.length == length
        && (length == 0u || memcmp(value.data, expected, length) == 0);
}

static bool fixture_init(fixture_t *fixture, const char *name,
                         const char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    if (!st_ast_unit_init(&fixture->unit, name)) return false;
    if (!st_parser_init_cstr(&fixture->parser, &fixture->unit, source)
            || !st_parse_compilation_unit(&fixture->parser)
            || !st_parser_at_end(&fixture->parser)) {
        fprintf(stderr, "parse failed for %s: %s\n", name,
                st_parse_status_string(st_parser_status(&fixture->parser)));
        st_parser_destroy(&fixture->parser);
        st_ast_unit_destroy(&fixture->unit);
        memset(fixture, 0, sizeof(*fixture));
        return false;
    }
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
    memset(fixture, 0, sizeof(*fixture));
}

static const st_class_graph_entity_t *find_entity(
    const st_class_graph_result_t *graph, st_class_graph_entity_kind_t kind,
    st_class_graph_id_t namespace_id, const char *name)
{
    size_t index;
    for (index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == kind && entity->namespace_id == namespace_id
                && text_is(entity->name, name)) return entity;
    }
    return NULL;
}

static const st_class_graph_method_t *find_method(
    const st_class_graph_result_t *graph, st_class_graph_id_t owner,
    const char *selector)
{
    size_t index;
    for (index = 0u; index < graph->method_count; index++) {
        if (graph->methods[index].owner == owner
                && text_is(graph->methods[index].selector, selector)) {
            return &graph->methods[index];
        }
    }
    return NULL;
}

static size_t diagnostic_count(const st_class_graph_result_t *graph,
                               st_class_graph_diagnostic_code_t code)
{
    size_t index;
    size_t count = 0u;
    for (index = 0u; index < graph->diagnostic_count; index++)
        if (graph->diagnostics[index].code == code) count++;
    return count;
}

static const st_sema_external_t *catalog_find(const st_sema_catalog_t *catalog,
                                               st_sema_external_kind_t kind,
                                               const char *name)
{
    size_t index;
    for (index = 0u; index < catalog->count; index++) {
        if (catalog->entries[index].kind == kind
                && text_is(catalog->entries[index].name, name)) {
            return &catalog->entries[index];
        }
    }
    return NULL;
}

static void test_image_application_graph_and_catalogs(void)
{
    fixture_t image;
    fixture_t application;
    const st_ast_unit_t *units[2];
    st_class_graph_result_t graph;
    const st_class_graph_entity_t *object;
    const st_class_graph_entity_t *base;
    const st_class_graph_entity_t *child;
    const st_class_graph_method_t *probe;
    const st_class_graph_method_t *meta_probe;
    st_sema_catalog_t catalog;
    st_sema_result_t sema;
    st_class_graph_sema_view_t view;
    st_class_graph_stats_t stats;

    CHECK(fixture_init(&image, "image.st",
        "Object := nil [ | root RootVar | "
        "foo [ ^root ] class rootClass [ ^RootVar ] ] "
        "Base := Object [ | base BaseVar | foo [ ^base ] ]"));
    CHECK(fixture_init(&application, "application.st",
        "Child := Base [ | child ChildVar | "
        "probe [ super foo. root. base. child. RootVar. BaseVar. ChildVar. Base ] "
        "class metaProbe [ super rootClass. ^ChildVar ] ] "
        "Base extend [ added [ ^RootVar ] ]"));
    units[0] = &image.unit;
    units[1] = &application.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 2u, NULL)
          == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    CHECK(graph.entity_count == 6u);
    object = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "Object");
    base = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "Base");
    child = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "Child");
    CHECK(object != NULL && object->id == 1u && object->metaclass_id == 2u);
    CHECK(base != NULL && base->id == 3u && base->superclass_id == object->id);
    CHECK(child != NULL && child->id == 5u && child->superclass_id == base->id);
    CHECK(st_class_graph_entity(&graph, 4u)->superclass_id
          == object->metaclass_id);
    CHECK(st_class_graph_entity(&graph, 6u)->superclass_id
          == base->metaclass_id);
    CHECK(child->instance_slot_count == 3u);
    CHECK(text_is(graph.instance_slots[child->instance_slot_offset + 0u].name,
                  "root"));
    CHECK(text_is(graph.instance_slots[child->instance_slot_offset + 1u].name,
                  "base"));
    CHECK(text_is(graph.instance_slots[child->instance_slot_offset + 2u].name,
                  "child"));
    CHECK(graph.instance_slots[child->instance_slot_offset + 2u].slot == 2u);
    CHECK(child->class_variable_count == 3u);
    CHECK(text_is(graph.class_variables[child->class_variable_offset + 0u].name,
                  "RootVar"));
    CHECK(text_is(graph.class_variables[child->class_variable_offset + 2u].name,
                  "ChildVar"));
    probe = find_method(&graph, child->id, "probe");
    meta_probe = find_method(&graph, child->metaclass_id, "metaProbe");
    CHECK(probe != NULL && probe->lexical_super == base->id);
    CHECK(meta_probe != NULL
        && meta_probe->lexical_super == base->metaclass_id);
    CHECK(find_method(&graph, base->id, "added") != NULL);
    CHECK(probe != NULL && probe->origin.unit_index == 1u
        && text_is(probe->origin.source_name, "application.st"));

    CHECK(st_class_graph_stats(&graph, &stats));
    CHECK(stats.shared_global_count == 3u);
    CHECK(stats.compatibility_view_count == 0u);
    st_class_graph_sema_view_init(&view);
    CHECK(st_class_graph_sema_view_build_minimal(&view, &graph, probe->id) ==
          ST_CLASS_GRAPH_OK);
    CHECK(view.catalog.count == 7u);
    CHECK(catalog_find(&view.catalog,
                       ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, "root") != NULL);
    CHECK(catalog_find(&view.catalog,
                       ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, "child")->slot ==
          2u);
    CHECK(catalog_find(&view.catalog,
                       ST_SEMA_EXTERNAL_CLASS_VARIABLE, "BaseVar") != NULL);
    CHECK(catalog_find(&view.catalog, ST_SEMA_EXTERNAL_GLOBAL, "Base")
              ->external_id == base->id);
    CHECK(catalog_find(&view.catalog, ST_SEMA_EXTERNAL_GLOBAL, "Child") ==
          NULL);
    st_sema_result_init(&sema);
    CHECK(st_sema_analyze_method(&sema, probe->node, &view.catalog) ==
          ST_SEMA_OK);
    CHECK(st_sema_succeeded(&sema));
    st_sema_result_destroy(&sema);
    st_class_graph_sema_view_destroy(&view);

    CHECK(st_class_graph_sema_catalog_for_method(&graph, probe->id, &catalog));
    {
        const st_sema_external_t *first_entries = catalog.entries;
        st_sema_catalog_t repeated;
        CHECK(st_class_graph_sema_catalog_for_method(&graph, probe->id,
                                                     &repeated));
        CHECK(repeated.entries == first_entries &&
              repeated.count == catalog.count);
    }
    CHECK(catalog.has_lexical_super);
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, "root")
          != NULL);
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, "child")
          ->slot == 2u);
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_CLASS_VARIABLE, "BaseVar")
          != NULL);
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_GLOBAL, "Base")
          ->external_id == base->id);
    CHECK(catalog.count == 9u);
    CHECK(catalog.entries[6u].kind == ST_SEMA_EXTERNAL_GLOBAL &&
          text_is(catalog.entries[6u].name, "Object"));
    CHECK(catalog.entries[7u].kind == ST_SEMA_EXTERNAL_GLOBAL &&
          text_is(catalog.entries[7u].name, "Base"));
    CHECK(catalog.entries[8u].kind == ST_SEMA_EXTERNAL_GLOBAL &&
          text_is(catalog.entries[8u].name, "Child"));
    st_sema_result_init(&sema);
    CHECK(st_sema_analyze_method(&sema, probe->node, &catalog) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&sema));
    st_sema_result_destroy(&sema);

    CHECK(st_class_graph_sema_catalog_for_method(
        &graph, meta_probe->id, &catalog));
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_INSTANCE_VARIABLE, "root")
          == NULL);
    CHECK(catalog_find(&catalog, ST_SEMA_EXTERNAL_CLASS_VARIABLE, "ChildVar")
          != NULL);
    st_sema_result_init(&sema);
    CHECK(st_sema_analyze_method(&sema, meta_probe->node, &catalog)
          == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&sema));
    st_sema_result_destroy(&sema);

    CHECK(st_class_graph_stats(&graph, &stats));
    CHECK(stats.compatibility_view_count == 2u);
    CHECK(stats.compatibility_entry_count == 15u);

    st_class_graph_result_destroy(&graph);
    fixture_destroy(&application);
    fixture_destroy(&image);
}

static void test_forward_superclasses_namespaces_and_ids(void)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    const st_class_graph_entity_t *object;
    const st_class_graph_entity_t *outer;
    const st_class_graph_entity_t *a;
    const st_class_graph_entity_t *b;
    CHECK(fixture_init(&fixture, "forward.st",
        "Object := nil [ ] A := B [ | a | ] B := Object [ | b | ] "
        "Outer := Namespace [ NChild := NBase [ ] NBase := Object [ ] ]"));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    object = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "Object");
    a = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "A");
    b = find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u, "B");
    outer = find_entity(&graph, ST_CLASS_GRAPH_NAMESPACE, 0u, "Outer");
    CHECK(object != NULL && a != NULL && b != NULL && outer != NULL);
    CHECK(a->superclass_id == b->id && b->superclass_id == object->id);
    CHECK(a->instance_slot_count == 2u);
    CHECK(a->id == 3u && b->id == 5u && outer->id == 7u);
    CHECK(find_entity(&graph, ST_CLASS_GRAPH_CLASS, outer->id, "NChild")
              ->superclass_id
          == find_entity(&graph, ST_CLASS_GRAPH_CLASS, outer->id, "NBase")->id);
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
}

static void test_layered_namespace_precedence(void)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    const st_class_graph_entity_t *outer;
    const st_class_graph_entity_t *inner;
    const st_class_graph_entity_t *inner_x;
    const st_class_graph_entity_t *user;
    const st_class_graph_method_t *probe;
    st_class_graph_sema_view_t view;
    st_sema_catalog_t full;
    CHECK(fixture_init(&fixture, "scopes.st",
        "Object := nil [ ] X := Object [ ] "
        "Outer := Namespace [ X := Object [ ] "
        "Inner := Namespace [ X := Object [ ] "
        "User := Object [ probe [ ^X ] ] ] ]"));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) ==
          ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    outer = find_entity(&graph, ST_CLASS_GRAPH_NAMESPACE, 0u, "Outer");
    inner = outer == NULL ? NULL : find_entity(
        &graph, ST_CLASS_GRAPH_NAMESPACE, outer->id, "Inner");
    inner_x = inner == NULL ? NULL : find_entity(
        &graph, ST_CLASS_GRAPH_CLASS, inner->id, "X");
    user = inner == NULL ? NULL : find_entity(
        &graph, ST_CLASS_GRAPH_CLASS, inner->id, "User");
    CHECK(outer != NULL && inner != NULL && inner_x != NULL && user != NULL);
    probe = user == NULL ? NULL : find_method(&graph, user->id, "probe");
    CHECK(probe != NULL);
    st_class_graph_sema_view_init(&view);
    CHECK(st_class_graph_sema_view_build_minimal(&view, &graph, probe->id) ==
          ST_CLASS_GRAPH_OK);
    CHECK(view.catalog.count == 1u);
    CHECK(catalog_find(&view.catalog, ST_SEMA_EXTERNAL_GLOBAL, "X")
              ->external_id == inner_x->id);
    st_class_graph_sema_view_destroy(&view);
    CHECK(st_class_graph_sema_catalog_for_method(&graph, probe->id, &full));
    CHECK(catalog_find(&full, ST_SEMA_EXTERNAL_GLOBAL, "X")->external_id ==
          inner_x->id);
    /* X is shadowed twice, so only the innermost binding is observable. */
    {
        size_t x_count = 0u;
        for (size_t index = 0u; index < full.count; index++)
            if (full.entries[index].kind == ST_SEMA_EXTERNAL_GLOBAL &&
                text_is(full.entries[index].name, "X"))
                x_count++;
        CHECK(x_count == 1u);
    }
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
}

static void test_language_diagnostics(void)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    CHECK(fixture_init(&fixture, "negative.st",
        "Object := nil [ | root RootVar | foo [ ] class same [ ] ] "
        "Late extend [ before [ ] ] "
        "Late := Object [ before [ ] before [ ] class before [ ] ] "
        "Missing extend [ x [ ] ] "
        "Dup := Object [ ] Dup := Object [ ] "
        "Bad := Unknown [ ] "
        "A := B [ ] B := A [ ] "
        "Vars := Object [ | root RootVar own own | ] "
        "Object extend [ foo [ ] ] "
        "Space := Namespace [ ] Space extend [ x [ ] ]"));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) == ST_CLASS_GRAPH_OK);
    CHECK(!st_class_graph_succeeded(&graph));
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_EXTENSION_BEFORE_TARGET) == 1u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_MISSING) == 1u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_NOT_CLASS) == 1u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_DUPLICATE_DEFINITION) == 1u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_SUPERCLASS_MISSING) == 1u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_INHERITANCE_CYCLE) == 2u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_DUPLICATE_VARIABLE) == 3u);
    CHECK(diagnostic_count(&graph,
          ST_CLASS_GRAPH_DIAG_DUPLICATE_METHOD) == 2u);
    CHECK(graph.diagnostic_count != 0u
        && text_is(graph.diagnostics[0].origin.source_name, "negative.st"));
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
}

static void test_entity_hash_growth(void)
{
    enum { CLASS_COUNT = 300, SOURCE_CAPACITY = 32768 };
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    char *source = malloc(SOURCE_CAPACITY);
    size_t length = 0u;
    size_t index;
    CHECK(source != NULL);
    if (source == NULL) return;
    length = (size_t)snprintf(source, SOURCE_CAPACITY,
                             "Object := nil [ ] ");
    for (index = 0u; index < CLASS_COUNT; index++) {
        int amount = index == 0u
            ? snprintf(source + length, SOURCE_CAPACITY - length,
                       "C0 := Object [ ] ")
            : snprintf(source + length, SOURCE_CAPACITY - length,
                       "C%zu := C%zu [ ] ", index, index - 1u);
        CHECK(amount > 0 && (size_t)amount < SOURCE_CAPACITY - length);
        if (amount <= 0 || (size_t)amount >= SOURCE_CAPACITY - length) break;
        length += (size_t)amount;
    }
    CHECK(index == CLASS_COUNT);
    CHECK(fixture_init(&fixture, "hash-growth.st", source));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    CHECK(graph.entity_count == 2u * (CLASS_COUNT + 1u));
    {
        const st_class_graph_entity_t *last = find_entity(
            &graph, ST_CLASS_GRAPH_CLASS, 0u, "C299");
        const st_class_graph_entity_t *parent = find_entity(
            &graph, ST_CLASS_GRAPH_CLASS, 0u, "C298");
        CHECK(last != NULL && parent != NULL
              && last->superclass_id == parent->id);
    }
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
    free(source);
}

static void test_ten_thousand_class_method_stress(void)
{
    enum { CLASS_COUNT = 10000 };
    const size_t source_capacity = (size_t)CLASS_COUNT * 64u + 64u;
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    st_class_graph_stats_t stats;
    st_class_graph_sema_view_t view;
    st_sema_result_t sema;
    char *source = malloc(source_capacity);
    size_t length;
    size_t index;
    clock_t begin;
    clock_t end;
    CHECK(source != NULL);
    if (!source) return;
    length = (size_t)snprintf(source, source_capacity, "Object := nil [ ] ");
    for (index = 0u; index < CLASS_COUNT; index++) {
        int amount = snprintf(source + length, source_capacity - length,
                              "C%zu := Object [ m%zu [ ^Object ] ] ",
                              index, index);
        CHECK(amount > 0 && (size_t)amount < source_capacity - length);
        if (amount <= 0 || (size_t)amount >= source_capacity - length) break;
        length += (size_t)amount;
    }
    CHECK(index == CLASS_COUNT);
    CHECK(fixture_init(&fixture, "stress-10k.st", source));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    begin = clock();
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) ==
          ST_CLASS_GRAPH_OK);
    end = clock();
    CHECK(st_class_graph_succeeded(&graph));
    CHECK(graph.entity_count == 2u * ((size_t)CLASS_COUNT + 1u));
    CHECK(graph.method_count == CLASS_COUNT);
    CHECK(graph.catalog_entry_count == (size_t)CLASS_COUNT + 1u);
    CHECK(st_class_graph_stats(&graph, &stats));
    CHECK(stats.shared_global_count == (size_t)CLASS_COUNT + 1u);
    CHECK(stats.compatibility_view_count == 0u);
    CHECK(stats.compatibility_entry_count == 0u);
    CHECK(stats.method_index_capacity >= CLASS_COUNT);
    CHECK(stats.method_index_probe_count < (size_t)CLASS_COUNT * 64u);
    /* A method in a 10k-global image passes only the referenced Object binding
     * to sema, avoiding both a 10k allocation and sema's quadratic catalog
     * validation path. */
    st_class_graph_sema_view_init(&view);
    CHECK(st_class_graph_sema_view_build_minimal(
              &view, &graph, graph.methods[CLASS_COUNT - 1u].id) ==
          ST_CLASS_GRAPH_OK);
    CHECK(view.catalog.count == 1u);
    CHECK(catalog_find(&view.catalog, ST_SEMA_EXTERNAL_GLOBAL, "Object") !=
          NULL);
    st_sema_result_init(&sema);
    CHECK(st_sema_analyze_method(
              &sema, graph.methods[CLASS_COUNT - 1u].node,
              &view.catalog) == ST_SEMA_OK);
    CHECK(st_sema_succeeded(&sema));
    st_sema_result_destroy(&sema);
    st_class_graph_sema_view_destroy(&view);
    fprintf(stderr,
            "class graph stress: %u classes, %zu methods, %zu shared globals, "
            "%zu method-hash probes, %.3f CPU s\n",
            CLASS_COUNT, graph.method_count, stats.shared_global_count,
            stats.method_index_probe_count,
            (double)(end - begin) / (double)CLOCKS_PER_SEC);
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
    free(source);
}

typedef struct {
    const st_class_graph_result_t *graph;
    _Atomic(bool) done;
    _Atomic(unsigned) errors;
} catalog_stats_race_t;

static void *catalog_materializer_thread(void *argument)
{
    catalog_stats_race_t *state = argument;
    for (size_t index = 0u; index < state->graph->method_count; index++) {
        st_sema_catalog_t catalog;
        if (!st_class_graph_sema_catalog_for_method(
                state->graph, state->graph->methods[index].id, &catalog) ||
            catalog.count == 0u)
            atomic_fetch_add_explicit(&state->errors, 1u,
                                      memory_order_relaxed);
    }
    atomic_store_explicit(&state->done, true, memory_order_release);
    return NULL;
}

static void *catalog_stats_reader_thread(void *argument)
{
    catalog_stats_race_t *state = argument;
    size_t previous_views = 0u;
    do {
        st_class_graph_stats_t stats = {0};
        if (!st_class_graph_stats(state->graph, &stats) ||
            stats.compatibility_view_count < previous_views ||
            stats.compatibility_view_count > state->graph->method_count ||
            stats.compatibility_entry_count <
                stats.compatibility_view_count) {
            atomic_fetch_add_explicit(&state->errors, 1u,
                                      memory_order_relaxed);
        }
        previous_views = stats.compatibility_view_count;
    } while (!atomic_load_explicit(&state->done, memory_order_acquire));
    return NULL;
}

static void test_concurrent_catalog_materialization_and_stats(void)
{
    enum { CLASS_COUNT = 128, SOURCE_CAPACITY = 16384 };
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_class_graph_result_t graph;
    catalog_stats_race_t state;
    pthread_t materializer;
    pthread_t stats_reader;
    char *source = malloc(SOURCE_CAPACITY);
    size_t length;
    size_t index;
    CHECK(source != NULL);
    if (!source) return;
    length = (size_t)snprintf(source, SOURCE_CAPACITY, "Object := nil [ ] ");
    for (index = 0u; index < CLASS_COUNT; index++) {
        int amount = snprintf(source + length, SOURCE_CAPACITY - length,
                              "R%zu := Object [ r%zu [ ^Object ] ] ",
                              index, index);
        CHECK(amount > 0 && (size_t)amount < SOURCE_CAPACITY - length);
        if (amount <= 0 || (size_t)amount >= SOURCE_CAPACITY - length) break;
        length += (size_t)amount;
    }
    CHECK(index == CLASS_COUNT);
    CHECK(fixture_init(&fixture, "catalog-race.st", source));
    units[0] = &fixture.unit;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) ==
          ST_CLASS_GRAPH_OK);
    state.graph = &graph;
    atomic_init(&state.done, false);
    atomic_init(&state.errors, 0u);
    CHECK(pthread_create(&materializer, NULL, catalog_materializer_thread,
                         &state) == 0);
    CHECK(pthread_create(&stats_reader, NULL, catalog_stats_reader_thread,
                         &state) == 0);
    CHECK(pthread_join(materializer, NULL) == 0);
    CHECK(pthread_join(stats_reader, NULL) == 0);
    CHECK(atomic_load_explicit(&state.errors, memory_order_relaxed) == 0u);
    {
        st_class_graph_stats_t stats;
        CHECK(st_class_graph_stats(&graph, &stats));
        CHECK(stats.compatibility_view_count == CLASS_COUNT);
        CHECK(stats.compatibility_entry_count ==
              (size_t)CLASS_COUNT * ((size_t)CLASS_COUNT + 1u));
    }
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
    free(source);
}

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t outstanding;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *memory;
    if (fault->calls++ >= fault->fail_at) return NULL;
    memory = malloc(size);
    if (memory != NULL) fault->outstanding++;
    return memory;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (pointer != NULL) {
        CHECK(fault->outstanding != 0u);
        if (fault->outstanding != 0u) fault->outstanding--;
        free(pointer);
    }
}

static void test_allocator_fault_transactionality(void)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    size_t fail_at;
    bool reached_success = false;
    CHECK(fixture_init(&fixture, "fault.st",
        "Object := nil [ | root RootVar | foo [ ^root ] ] "
        "Child := Object [ | child ChildVar | probe [ super foo. child ] ]"));
    units[0] = &fixture.unit;
    for (fail_at = 0u; fail_at < 256u; fail_at++) {
        fault_allocator_t fault;
        st_class_graph_options_t options;
        st_class_graph_result_t graph;
        st_class_graph_status_t status;
        memset(&fault, 0, sizeof(fault));
        fault.fail_at = fail_at;
        memset(&options, 0, sizeof(options));
        options.allocator.allocate = fault_allocate;
        options.allocator.deallocate = fault_deallocate;
        options.allocator.user = &fault;
        st_class_graph_result_init(&graph);
        status = st_class_graph_build(&graph, units, 1u, &options);
        if (status == ST_CLASS_GRAPH_OK) {
            const st_class_graph_method_t *probe = find_method(
                &graph, find_entity(&graph, ST_CLASS_GRAPH_CLASS, 0u,
                                    "Child")->id, "probe");
            st_class_graph_sema_view_t view;
            st_sema_catalog_t catalog;
            size_t live_before = fault.outstanding;
            CHECK(st_class_graph_succeeded(&graph));
            CHECK(probe != NULL);
            st_class_graph_sema_view_init(&view);
            fault.fail_at = fault.calls;
            CHECK(st_class_graph_sema_view_build_minimal(
                      &view, &graph, probe->id) ==
                  ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY);
            CHECK(!view.initialized && view.catalog.entries == NULL);
            CHECK(fault.outstanding == live_before);
            CHECK(!st_class_graph_sema_catalog_for_method(
                &graph, probe->id, &catalog));
            CHECK(fault.outstanding == live_before);
            /* Let the temporary name hash succeed, then fail the final compact
             * entry vector. The temporary must still be rolled back. */
            fault.fail_at = fault.calls + 1u;
            CHECK(st_class_graph_sema_view_build_minimal(
                      &view, &graph, probe->id) ==
                  ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY);
            CHECK(!view.initialized && view.catalog.entries == NULL);
            CHECK(fault.outstanding == live_before);
            fault.fail_at = SIZE_MAX;
            CHECK(st_class_graph_sema_view_build_minimal(
                      &view, &graph, probe->id) == ST_CLASS_GRAPH_OK);
            CHECK(view.initialized && view.catalog.count != 0u);
            st_class_graph_sema_view_destroy(&view);
            CHECK(fault.outstanding == live_before);
            CHECK(st_class_graph_sema_catalog_for_method(
                &graph, probe->id, &catalog));
            CHECK(catalog.count != 0u);
            reached_success = true;
        } else {
            CHECK(status == ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY);
            CHECK(graph.implementation == NULL && graph.entities == NULL
                && graph.methods == NULL && graph.diagnostics == NULL);
        }
        st_class_graph_result_destroy(&graph);
        CHECK(fault.outstanding == 0u);
        if (reached_success) break;
    }
    CHECK(reached_success);
    fixture_destroy(&fixture);
}

static void test_public_validation(void)
{
    st_class_graph_result_t graph;
    st_sema_catalog_t catalog;
    st_class_graph_sema_view_t view;
    st_class_graph_stats_t stats;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, NULL, 1u, NULL)
          == ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
    CHECK(!st_class_graph_succeeded(&graph));
    CHECK(st_class_graph_entity(&graph, 0u) == NULL);
    CHECK(st_class_graph_method(&graph, 0u) == NULL);
    CHECK(!st_class_graph_sema_catalog_for_method(&graph, 0u, &catalog));
    st_class_graph_sema_view_init(&view);
    CHECK(st_class_graph_sema_view_build_minimal(&view, &graph, 0u) ==
          ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
    CHECK(!view.initialized && view.catalog.entries == NULL);
    CHECK(!st_class_graph_stats(&graph, &stats));
    view.catalog.count = 1u;
    CHECK(st_class_graph_sema_view_build_minimal(&view, &graph, 0u) ==
          ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
    st_class_graph_sema_view_init(&view);
    st_class_graph_sema_view_destroy(&view);
    CHECK(strcmp(st_class_graph_status_string(
                 ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY), "out of memory") == 0);
    CHECK(strcmp(st_class_graph_diagnostic_string(
                 ST_CLASS_GRAPH_DIAG_INHERITANCE_CYCLE),
                 "inheritance cycle") == 0);
    st_class_graph_result_destroy(&graph);
}

int main(void)
{
    test_image_application_graph_and_catalogs();
    test_forward_superclasses_namespaces_and_ids();
    test_layered_namespace_precedence();
    test_language_diagnostics();
    test_entity_hash_growth();
    test_ten_thousand_class_method_stress();
    test_concurrent_catalog_materialization_and_stats();
    test_allocator_fault_transactionality();
    test_public_validation();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk class graph: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk class graph: all tests passed");
    return EXIT_SUCCESS;
}
