#include "st_class_graph.h"
#include "st_source_bundle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
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

static const st_class_graph_entity_t *find_class(
    const st_class_graph_result_t *graph, const char *name)
{
    size_t index;
    for (index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && entity->namespace_id == ST_CLASS_GRAPH_INVALID_ID
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
        const st_class_graph_method_t *method = &graph->methods[index];
        if (method->owner == owner && text_is(method->selector, selector))
            return method;
    }
    return NULL;
}

static bool method_references_instance_slot(
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method,
    const char *name,
    uint32_t slot,
    st_sema_access_t access)
{
    st_class_graph_sema_view_t view;
    st_sema_result_t sema;
    bool found = false;

    if (graph == NULL || method == NULL || name == NULL) {
        return false;
    }
    st_class_graph_sema_view_init(&view);
    st_sema_result_init(&sema);
    if (st_class_graph_sema_view_build_minimal(
            &view, graph, method->id) != ST_CLASS_GRAPH_OK
            || st_sema_analyze_method(
                &sema, method->node, &view.catalog) != ST_SEMA_OK
            || !st_sema_succeeded(&sema)) {
        goto done;
    }
    for (size_t index = 0u; index < sema.reference_count; index++) {
        const st_sema_reference_t *reference = &sema.references[index];
        const st_sema_binding_t *binding;

        if (reference->binding >= sema.binding_count) {
            continue;
        }
        binding = &sema.bindings[reference->binding];
        if (reference->access == access
                && binding->kind == ST_SEMA_BIND_INSTANCE_VARIABLE
                && binding->slot == slot
                && text_is(binding->name, name)) {
            found = true;
            break;
        }
    }
done:
    st_sema_result_destroy(&sema);
    st_class_graph_sema_view_destroy(&view);
    return found;
}

int main(void)
{
    const char *image = first_existing(
        "st-image", "samples/smalltalk/st-image");
    const char *application = first_existing(
        "tests/fixtures/HelloApplication.st",
        "samples/smalltalk/tests/fixtures/HelloApplication.st");
    const char *applications[1];
    st_source_bundle_t bundle;
    st_class_graph_result_t graph;
    const st_ast_unit_t **units = NULL;
    const st_class_graph_entity_t *object;
    const st_class_graph_entity_t *character;
    const st_class_graph_entity_t *fraction;
    const st_class_graph_entity_t *block;
    const st_class_graph_entity_t *string;
    const st_class_graph_entity_t *symbol;
    const st_class_graph_entity_t *association;
    const st_class_graph_entity_t *ordered_collection;
    const st_class_graph_entity_t *hashed_collection;
    const st_class_graph_entity_t *set;
    const st_class_graph_entity_t *dictionary;
    const st_class_graph_entity_t *application_class;
    size_t index;

    CHECK(image != NULL && application != NULL);
    if (image == NULL || application == NULL) return EXIT_FAILURE;
    applications[0] = application;
    CHECK(st_source_bundle_load(&bundle, image, applications, 1u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) {
        fprintf(stderr, "image load failed in %s: %s (%s)\n",
                st_source_load_phase_string(bundle.diagnostic.phase),
                st_source_load_status_string(bundle.diagnostic.status),
                bundle.diagnostic.path);
        st_source_bundle_destroy(&bundle);
        return EXIT_FAILURE;
    }
    CHECK(bundle.image_count >= 37u && bundle.count == bundle.image_count + 1u);
    CHECK(bundle.files[0].origin == ST_SOURCE_ORIGIN_IMAGE);
    CHECK(bundle.files[bundle.image_count - 1u].origin
          == ST_SOURCE_ORIGIN_IMAGE);
    CHECK(bundle.files[bundle.image_count].origin
          == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(strcmp(bundle.files[0].path, "Kernel/Object.st") == 0);

    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) {
        st_source_bundle_destroy(&bundle);
        return EXIT_FAILURE;
    }
    for (index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;

    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, bundle.count, NULL)
          == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    if (!st_class_graph_succeeded(&graph)) {
        fprintf(stderr, "class graph has %zu diagnostic(s)\n",
                graph.diagnostic_count);
    }
    CHECK(graph.entity_count == bundle.count * 2u);
    for (index = 0u; index < graph.method_count; index++) {
        st_class_graph_sema_view_t view;
        st_sema_result_t sema;
        st_class_graph_sema_view_init(&view);
        CHECK(st_class_graph_sema_view_build_minimal(
            &view, &graph, graph.methods[index].id) == ST_CLASS_GRAPH_OK);
        st_sema_result_init(&sema);
        CHECK(st_sema_analyze_method(
            &sema, graph.methods[index].node, &view.catalog) == ST_SEMA_OK);
        if (!st_sema_succeeded(&sema)) {
            fprintf(stderr, "semantic analysis failed for %.*s (%zu error(s))\n",
                    (int)graph.methods[index].selector.length,
                    graph.methods[index].selector.data,
                    sema.diagnostic_count);
        }
        CHECK(st_sema_succeeded(&sema));
        st_sema_result_destroy(&sema);
        st_class_graph_sema_view_destroy(&view);
    }
    object = find_class(&graph, "Object");
    character = find_class(&graph, "Character");
    fraction = find_class(&graph, "Fraction");
    block = find_class(&graph, "Block");
    string = find_class(&graph, "String");
    symbol = find_class(&graph, "Symbol");
    association = find_class(&graph, "Association");
    ordered_collection = find_class(&graph, "OrderedCollection");
    hashed_collection = find_class(&graph, "HashedCollection");
    set = find_class(&graph, "Set");
    dictionary = find_class(&graph, "Dictionary");
    application_class = find_class(&graph, "HelloApplication");
    CHECK(object != NULL && object->id == 1u && object->metaclass_id == 2u);
    CHECK(application_class != NULL);
    CHECK(character != NULL);
    CHECK(fraction != NULL);
    CHECK(block != NULL);
    CHECK(string != NULL);
    CHECK(symbol != NULL);
    if (object != NULL) {
        CHECK(find_method(&graph, object->id, "isMemberOf:") != NULL);
        CHECK(find_method(&graph, object->id, "respondsTo:") != NULL);
        CHECK(find_method(&graph, object->id, "isString") != NULL);
        CHECK(find_method(&graph, object->id, "isSymbol") != NULL);
    }
    if (character != NULL) {
        CHECK(find_method(&graph, character->id, "codePoint") != NULL);
    }
    if (fraction != NULL) {
        const st_class_graph_method_t *numerator_method;
        const st_class_graph_method_t *initializer_method;

        CHECK(fraction->instance_slot_count == 2u);
        CHECK(text_is(
            graph.instance_slots[fraction->instance_slot_offset].name,
            "numerator"));
        CHECK(text_is(
            graph.instance_slots[fraction->instance_slot_offset + 1u].name,
            "denominator"));
        CHECK(find_method(
            &graph, fraction->metaclass_id, "numerator:denominator:")
              != NULL);
        CHECK(find_method(&graph, fraction->metaclass_id, "new") != NULL);
        CHECK(find_method(&graph, fraction->metaclass_id, "new:") != NULL);
        CHECK(find_method(&graph, fraction->metaclass_id, "basicNew")
              != NULL);
        CHECK(find_method(&graph, fraction->metaclass_id, "basicNew:")
              != NULL);
        numerator_method = find_method(&graph, fraction->id, "numerator");
        initializer_method = find_method(
            &graph, fraction->id, "initializeNumerator:denominator:");
        CHECK(numerator_method != NULL && initializer_method != NULL);
        CHECK(method_references_instance_slot(
            &graph, numerator_method, "numerator", 0u,
            ST_SEMA_ACCESS_READ));
        CHECK(method_references_instance_slot(
            &graph, initializer_method, "numerator", 0u,
            ST_SEMA_ACCESS_WRITE));
        CHECK(method_references_instance_slot(
            &graph, initializer_method, "denominator", 1u,
            ST_SEMA_ACCESS_WRITE));
        CHECK(find_method(&graph, fraction->id, "quo:") != NULL);
        CHECK(find_method(&graph, fraction->id, "rem:") != NULL);
        CHECK(find_method(&graph, fraction->id, "//") != NULL);
        CHECK(find_method(&graph, fraction->id, "\\\\") != NULL);
        CHECK(find_method(&graph, fraction->id, "rounded") != NULL);
        CHECK(find_method(&graph, fraction->id, "hash") != NULL);
        CHECK(find_method(&graph, fraction->id, "instVarAt:put:") != NULL);
    }
    if (block != NULL) {
        CHECK(find_method(&graph, block->metaclass_id, "new") != NULL);
        CHECK(find_method(&graph, block->metaclass_id, "new:") != NULL);
        CHECK(find_method(&graph, block->metaclass_id, "basicNew") != NULL);
        CHECK(find_method(&graph, block->metaclass_id, "basicNew:") != NULL);
    }
    if (string != NULL) {
        CHECK(find_method(&graph, string->id, "asString") != NULL);
        CHECK(find_method(&graph, string->id, "isString") != NULL);
    }
    if (object != NULL && application_class != NULL) {
        CHECK(application_class->superclass_id == object->id);
        CHECK(application_class->id + 1u == application_class->metaclass_id);
        CHECK(find_method(&graph, application_class->id, "run") != NULL);
    }
    if (symbol != NULL) {
        CHECK(find_method(&graph, symbol->id, "asSymbol") != NULL);
        CHECK(find_method(&graph, symbol->metaclass_id, "new") != NULL);
        CHECK(find_method(&graph, symbol->metaclass_id, "new:") != NULL);
        CHECK(find_method(&graph, symbol->metaclass_id, "basicNew") != NULL);
        CHECK(find_method(&graph, symbol->metaclass_id, "basicNew:") != NULL);
        CHECK(find_method(&graph, symbol->id, "isSymbol") != NULL);
    }
    CHECK(association != NULL && association->instance_slot_count == 2u);
    CHECK(ordered_collection != NULL
          && ordered_collection->instance_slot_count == 3u);
    CHECK(hashed_collection != NULL
          && hashed_collection->instance_slot_count == 2u);
    CHECK(set != NULL && hashed_collection != NULL
          && set->superclass_id == hashed_collection->id);
    CHECK(dictionary != NULL && hashed_collection != NULL
          && dictionary->superclass_id == hashed_collection->id);
    if (association != NULL) {
        CHECK(text_is(
            graph.instance_slots[association->instance_slot_offset].name,
            "key"));
        CHECK(text_is(
            graph.instance_slots[association->instance_slot_offset + 1u].name,
            "value"));
        CHECK(find_method(&graph, association->metaclass_id, "key:value:")
              != NULL);
    }
    if (ordered_collection != NULL) {
        CHECK(find_method(&graph, ordered_collection->id, "addFirst:")
              != NULL);
        CHECK(find_method(&graph, ordered_collection->id, "addLast:")
              != NULL);
        CHECK(find_method(&graph, ordered_collection->id, "removeAt:")
              != NULL);
        CHECK(find_method(&graph, ordered_collection->id, ",") != NULL);
    }
    if (dictionary != NULL) {
        CHECK(find_method(&graph, dictionary->id, "at:put:") != NULL);
        CHECK(find_method(&graph, dictionary->id, "removeKey:ifAbsent:")
              != NULL);
        CHECK(find_method(&graph, dictionary->id, "keysAndValuesDo:")
              != NULL);
    }

    st_class_graph_result_destroy(&graph);
    free(units);
    st_source_bundle_destroy(&bundle);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk image: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk image: PASS");
    return EXIT_SUCCESS;
}
