#define _POSIX_C_SOURCE 200809L

#include "st_class_graph.h"
#include "st_source_bundle.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

#define EXAMPLE_PATH_CAPACITY 1024u
#define EXAMPLE_MANIFEST_CAPACITY 4096u
#define EXAMPLE_TRANSCRIPT_EXTERNAL_ID UINT32_C(0xfffffffe)

typedef struct {
    const char *directory_name;
    const char *source_name;
    const char *class_name;
} example_spec_t;

static bool text_is(st_ast_string_t text, const char *expected)
{
    size_t length = strlen(expected);
    return text.length == length
        && (length == 0u || memcmp(text.data, expected, length) == 0);
}

static const char *first_existing_directory(const char *local,
                                             const char *root)
{
    struct stat metadata;
    if (stat(local, &metadata) == 0 && S_ISDIR(metadata.st_mode)) return local;
    if (stat(root, &metadata) == 0 && S_ISDIR(metadata.st_mode)) return root;
    return NULL;
}

static const char *first_existing_examples_directory(void)
{
    if (access("examples/hello/application.manifest", R_OK) == 0)
        return "examples";
    if (access("samples/smalltalk/examples/hello/application.manifest", R_OK)
            == 0) return "samples/smalltalk/examples";
    return NULL;
}

static bool join_path(char *out, size_t capacity,
                      const char *left, const char *right)
{
    int length = snprintf(out, capacity, "%s/%s", left, right);
    return length >= 0 && (size_t)length < capacity;
}

/* Application manifests are intentionally source lists, not a partially
 * implemented command-line format.  This fixture reader accepts exactly one
 * basename so an unnoticed second source or path escape fails the gate. */
static bool read_single_source_manifest(const char *directory,
                                        char *source_path,
                                        size_t source_path_capacity,
                                        const char *expected_source)
{
    char manifest_path[EXAMPLE_PATH_CAPACITY];
    unsigned char bytes[EXAMPLE_MANIFEST_CAPACITY + 1u];
    FILE *stream;
    size_t length;
    bool read_failed;
    bool close_failed;
    size_t offset = 0u;
    size_t source_count = 0u;
    char source_name[EXAMPLE_PATH_CAPACITY];

    if (!join_path(manifest_path, sizeof(manifest_path), directory,
                   "application.manifest")) return false;
    stream = fopen(manifest_path, "rb");
    if (stream == NULL) return false;
    length = fread(bytes, 1u, EXAMPLE_MANIFEST_CAPACITY + 1u, stream);
    read_failed = ferror(stream) != 0;
    close_failed = fclose(stream) != 0;
    if (read_failed || close_failed || length > EXAMPLE_MANIFEST_CAPACITY)
        return false;
    if (memchr(bytes, '\0', length) != NULL) return false;

    while (offset < length) {
        size_t begin = offset;
        size_t line_length;
        while (offset < length && bytes[offset] != '\n') offset++;
        line_length = offset - begin;
        if (line_length != 0u && bytes[begin + line_length - 1u] == '\r')
            line_length--;
        if (line_length != 0u && bytes[begin] != '#') {
            size_t index;
            if (source_count != 0u || line_length >= sizeof(source_name))
                return false;
            for (index = 0u; index < line_length; index++) {
                unsigned char byte = bytes[begin + index];
                if (byte == '/' || byte == '\\' || iscntrl(byte))
                    return false;
            }
            memcpy(source_name, bytes + begin, line_length);
            source_name[line_length] = '\0';
            source_count++;
        }
        if (offset < length) offset++;
    }
    if (source_count != 1u || strcmp(source_name, expected_source) != 0)
        return false;
    return join_path(source_path, source_path_capacity, directory, source_name);
}

static const st_class_graph_entity_t *find_top_level_class(
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

static bool analyze_method(const st_class_graph_result_t *graph,
                           const st_class_graph_method_t *method,
                           st_sema_result_t *sema)
{
    st_class_graph_sema_view_t view;
    bool succeeded;
    st_sema_result_init(sema);
    st_class_graph_sema_view_init(&view);
    if (st_class_graph_sema_view_build_minimal(&view, graph, method->id)
            != ST_CLASS_GRAPH_OK) {
        st_class_graph_sema_view_destroy(&view);
        return false;
    }
    succeeded = st_sema_analyze_method(sema, method->node, &view.catalog)
                    == ST_SEMA_OK
        && st_sema_succeeded(sema);
    st_class_graph_sema_view_destroy(&view);
    return succeeded;
}

/* Transcript is an image-initialized object global, not a class.  Until the
 * bootstrap-global table is promoted into the production graph API, this
 * frontend gate supplies precisely that one external to semantic analysis.
 * It deliberately supplies no value and cannot be used for lowering. */
static bool analyze_method_with_transcript(
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method, st_sema_result_t *sema)
{
    static const char transcript_name[] = "Transcript";
    st_class_graph_sema_view_t view;
    st_sema_external_t *entries;
    st_sema_catalog_t catalog;
    size_t count;
    bool succeeded;

    st_sema_result_init(sema);
    st_class_graph_sema_view_init(&view);
    if (st_class_graph_sema_view_build_minimal(&view, graph, method->id)
            != ST_CLASS_GRAPH_OK) {
        st_class_graph_sema_view_destroy(&view);
        return false;
    }
    count = view.catalog.count;
    if (count == SIZE_MAX) {
        st_class_graph_sema_view_destroy(&view);
        return false;
    }
    entries = malloc((count + 1u) * sizeof(*entries));
    if (entries == NULL) {
        st_class_graph_sema_view_destroy(&view);
        return false;
    }
    if (count != 0u)
        memcpy(entries, view.catalog.entries, count * sizeof(*entries));
    memset(&entries[count], 0, sizeof(entries[count]));
    entries[count].name.data = transcript_name;
    entries[count].name.length = sizeof(transcript_name) - 1u;
    entries[count].kind = ST_SEMA_EXTERNAL_GLOBAL;
    entries[count].external_id = EXAMPLE_TRANSCRIPT_EXTERNAL_ID;
    catalog = view.catalog;
    catalog.entries = entries;
    catalog.count = count + 1u;
    succeeded = st_sema_analyze_method(sema, method->node, &catalog)
                    == ST_SEMA_OK
        && st_sema_succeeded(sema);
    free(entries);
    st_class_graph_sema_view_destroy(&view);
    return succeeded;
}

static const st_ast_node_t *expression_literal_argument(
    const st_ast_node_t *message, size_t index)
{
    const st_ast_node_t *argument;
    if (message == NULL || message->kind != ST_AST_MESSAGE
            || index >= message->as.message.arguments.count) return NULL;
    argument = message->as.message.arguments.items[index];
    if (argument == NULL) return NULL;
    if (argument->kind != ST_AST_EXPRESSION) return argument;
    if (argument->as.expression.assignments.count != 0u
            || argument->as.expression.messages.count != 0u) return NULL;
    return argument->as.expression.receiver;
}

static void check_hello_protocol(const st_class_graph_result_t *graph,
                                 const st_class_graph_entity_t *application)
{
    static const char output[] = "Hello from Anvil Smalltalk";
    const st_class_graph_method_t *method = find_method(
        graph, application->id, "run");
    const st_ast_node_t *body;
    const st_ast_node_t *send;
    const st_ast_node_t *next_put_all;
    const st_ast_node_t *lf;
    const st_ast_node_t *argument;
    const st_sema_reference_t *reference;
    const st_sema_binding_t *binding;
    st_sema_result_t sema;

    CHECK(method != NULL);
    if (method == NULL) return;
    body = method->node->as.method.body;
    CHECK(body != NULL && body->kind == ST_AST_BLOCK);
    if (body == NULL || body->kind != ST_AST_BLOCK) return;
    CHECK(body->as.block.expressions.count == 2u);
    if (body->as.block.expressions.count != 2u) return;
    send = body->as.block.expressions.items[0];
    CHECK(send != NULL && send->kind == ST_AST_EXPRESSION);
    if (send == NULL || send->kind != ST_AST_EXPRESSION) return;
    CHECK(send->as.expression.receiver != NULL
          && send->as.expression.receiver->kind == ST_AST_VARIABLE);
    if (send->as.expression.receiver == NULL
            || send->as.expression.receiver->kind != ST_AST_VARIABLE) return;
    CHECK(text_is(send->as.expression.receiver->as.variable.name,
                  "Transcript"));
    CHECK(send->as.expression.messages.count == 2u);
    if (send->as.expression.messages.count != 2u) return;
    next_put_all = send->as.expression.messages.items[0];
    lf = send->as.expression.messages.items[1];
    CHECK(next_put_all != NULL && next_put_all->kind == ST_AST_MESSAGE);
    CHECK(lf != NULL && lf->kind == ST_AST_MESSAGE);
    if (next_put_all == NULL || next_put_all->kind != ST_AST_MESSAGE
            || lf == NULL || lf->kind != ST_AST_MESSAGE) return;
    CHECK(text_is(next_put_all->as.message.selector, "nextPutAll:"));
    CHECK(!next_put_all->as.message.starts_cascade);
    CHECK(text_is(lf->as.message.selector, "lf"));
    CHECK(lf->as.message.starts_cascade);
    CHECK(lf->as.message.arguments.count == 0u);
    argument = expression_literal_argument(next_put_all, 0u);
    CHECK(argument != NULL && argument->kind == ST_AST_STRING);
    if (argument != NULL && argument->kind == ST_AST_STRING)
        CHECK(argument->as.text.length == sizeof(output) - 1u
              && memcmp(argument->as.text.data, output,
                        sizeof(output) - 1u) == 0);

    if (!analyze_method_with_transcript(graph, method, &sema)) {
        CHECK(false);
        st_sema_result_destroy(&sema);
        return;
    }
    reference = st_sema_reference_for_node(
        &sema, send->as.expression.receiver);
    CHECK(reference != NULL && reference->binding < sema.binding_count);
    if (reference != NULL && reference->binding < sema.binding_count) {
        binding = &sema.bindings[reference->binding];
        CHECK(binding->kind == ST_SEMA_BIND_GLOBAL);
        CHECK(binding->external_id == EXAMPLE_TRANSCRIPT_EXTERNAL_ID);
        CHECK((binding->flags & ST_SEMA_BINDING_EXTERNAL) != 0u);
    }
    st_sema_result_destroy(&sema);
}

static void check_capture_mode(const st_class_graph_result_t *graph,
                               const st_class_graph_entity_t *application,
                               const char *selector,
                               st_sema_capture_mode_t expected_mode,
                               bool expected_nonlocal_return)
{
    const st_class_graph_method_t *method = find_method(
        graph, application->id, selector);
    st_sema_result_t sema;
    CHECK(method != NULL);
    if (method == NULL) return;
    if (!analyze_method(graph, method, &sema)) {
        CHECK(false);
        st_sema_result_destroy(&sema);
        return;
    }
    CHECK(sema.block_count == 1u);
    CHECK(sema.capture_count == 1u);
    if (sema.capture_count == 1u)
        CHECK(sema.captures[0].mode == expected_mode);
    if (sema.block_count == 1u)
        CHECK(sema.blocks[0].has_nonlocal_return == expected_nonlocal_return);
    st_sema_result_destroy(&sema);
}

static void check_nonlocal_return(const st_class_graph_result_t *graph,
                                  const st_class_graph_entity_t *application)
{
    const st_class_graph_method_t *method = find_method(
        graph, application->id, "nonLocalAnswer");
    st_sema_result_t sema;
    CHECK(method != NULL);
    if (method == NULL) return;
    if (!analyze_method(graph, method, &sema)) {
        CHECK(false);
        st_sema_result_destroy(&sema);
        return;
    }
    CHECK(sema.block_count == 1u);
    CHECK(sema.capture_count == 0u);
    CHECK(sema.return_count == 2u);
    if (sema.block_count == 1u)
        CHECK(sema.blocks[0].has_nonlocal_return);
    if (sema.return_count == 2u) {
        CHECK(sema.returns[0].kind == ST_SEMA_RETURN_HOME_METHOD
              || sema.returns[1].kind == ST_SEMA_RETURN_HOME_METHOD);
    }
    st_sema_result_destroy(&sema);
}

static void test_example(const char *image_directory,
                         const char *examples_directory,
                         const example_spec_t *spec)
{
    char application_directory[EXAMPLE_PATH_CAPACITY];
    char source_path[EXAMPLE_PATH_CAPACITY];
    const char *application_paths[1];
    st_source_bundle_t bundle;
    const st_ast_unit_t **units = NULL;
    st_class_graph_result_t graph;
    const st_class_graph_entity_t *object;
    const st_class_graph_entity_t *application;
    const st_class_graph_method_t *bootstrap_external_method = NULL;
    size_t index;

    if (!join_path(application_directory, sizeof(application_directory),
                   examples_directory, spec->directory_name)) {
        CHECK(false);
        return;
    }
    if (!read_single_source_manifest(application_directory, source_path,
                                     sizeof(source_path), spec->source_name)) {
        CHECK(false);
        return;
    }
    application_paths[0] = source_path;
    CHECK(st_source_bundle_load(&bundle, image_directory,
                                application_paths, 1u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) {
        fprintf(stderr, "%s: source load failed in %s: %s (%s)\n",
                spec->directory_name,
                st_source_load_phase_string(bundle.diagnostic.phase),
                st_source_load_status_string(bundle.diagnostic.status),
                bundle.diagnostic.path);
        st_source_bundle_destroy(&bundle);
        return;
    }

    CHECK(bundle.image_count >= 37u);
    CHECK(bundle.count == bundle.image_count + 1u);
    for (index = 0u; index < bundle.image_count; index++) {
        CHECK(bundle.files[index].origin == ST_SOURCE_ORIGIN_IMAGE);
        CHECK(bundle.files[index].ordinal == index);
    }
    CHECK(bundle.files[bundle.image_count].origin
          == ST_SOURCE_ORIGIN_APPLICATION);
    CHECK(bundle.files[bundle.image_count].ordinal == bundle.image_count);
    CHECK(strcmp(bundle.files[bundle.image_count].path, source_path) == 0);

    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    for (index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;

    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, bundle.count, NULL)
          == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    if (!st_class_graph_succeeded(&graph)) {
        fprintf(stderr, "%s: class graph has %zu diagnostic(s)\n",
                spec->directory_name, graph.diagnostic_count);
        goto done;
    }
    object = find_top_level_class(&graph, "Object");
    application = find_top_level_class(&graph, spec->class_name);
    CHECK(object != NULL);
    CHECK(application != NULL);
    if (object != NULL && application != NULL) {
        CHECK(application->superclass_id == object->id);
        CHECK(application->origin.unit_index == bundle.image_count);
        CHECK(text_is(application->origin.source_name, source_path));
        if (strcmp(spec->directory_name, "hello") == 0)
            bootstrap_external_method = find_method(
                &graph, application->id, "run");
    }

    for (index = 0u; index < graph.method_count; index++) {
        st_sema_result_t sema;
        if (&graph.methods[index] == bootstrap_external_method) continue;
        CHECK(analyze_method(&graph, &graph.methods[index], &sema));
        st_sema_result_destroy(&sema);
    }
    if (application != NULL && strcmp(spec->directory_name, "hello") == 0)
        check_hello_protocol(&graph, application);
    if (application != NULL && strcmp(spec->directory_name, "closures") == 0) {
        check_capture_mode(&graph, application, "makeAdder:",
                           ST_SEMA_CAPTURE_VALUE, false);
        check_capture_mode(&graph, application, "makeSelf",
                           ST_SEMA_CAPTURE_SELF, false);
        check_nonlocal_return(&graph, application);
    }

done:
    st_class_graph_result_destroy(&graph);
    free(units);
    st_source_bundle_destroy(&bundle);
}

int main(void)
{
    static const example_spec_t examples[] = {
        {"hello", "HelloApplication.st", "HelloApplication"},
        {"closures", "ClosuresApplication.st", "ClosuresApplication"}
    };
    const char *image_directory = first_existing_directory(
        "st-image", "samples/smalltalk/st-image");
    const char *examples_directory = first_existing_examples_directory();
    size_t index;

    CHECK(image_directory != NULL);
    CHECK(examples_directory != NULL);
    if (image_directory == NULL || examples_directory == NULL)
        return EXIT_FAILURE;
    for (index = 0u; index < sizeof(examples) / sizeof(examples[0]); index++)
        test_example(image_directory, examples_directory, &examples[index]);

    if (failures != 0u) {
        fprintf(stderr, "smalltalk examples frontend: %u failure(s)\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk examples frontend: PASS");
    return EXIT_SUCCESS;
}
