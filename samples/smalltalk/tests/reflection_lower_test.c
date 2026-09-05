#include "st_lower.h"
#include "st_parser.h"
#include "st_reflection_primitives.h"

#include "anvil/anvil_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    char *source;
    st_ast_unit_t unit;
    st_parser_t parser;
} fixture_t;

static bool text_is(st_ast_string_t value, const char *expected)
{
    size_t length = strlen(expected);
    return value.data != NULL && value.length == length
        && memcmp(value.data, expected, length) == 0;
}

static bool fixture_init(fixture_t *fixture, const char *source)
{
    size_t length = strlen(source);
    memset(fixture, 0, sizeof(*fixture));
    fixture->source = malloc(length + 1u);
    if (fixture->source == NULL) {
        return false;
    }
    memcpy(fixture->source, source, length + 1u);
    if (!st_ast_unit_init(&fixture->unit, "ReflectionAot.st")
            || !st_parser_init_cstr(
                &fixture->parser, &fixture->unit, fixture->source)
            || !st_parse_compilation_unit(&fixture->parser)
            || !st_parser_at_end(&fixture->parser)) {
        st_parser_destroy(&fixture->parser);
        st_ast_unit_destroy(&fixture->unit);
        free(fixture->source);
        memset(fixture, 0, sizeof(*fixture));
        return false;
    }
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_parser_destroy(&fixture->parser);
    st_ast_unit_destroy(&fixture->unit);
    free(fixture->source);
    memset(fixture, 0, sizeof(*fixture));
}

static const st_class_graph_method_t *find_method(
    const st_class_graph_result_t *graph)
{
    st_class_graph_id_t behavior = ST_CLASS_GRAPH_INVALID_ID;
    for (size_t index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && text_is(entity->name, "Behavior")) {
            behavior = entity->id;
            break;
        }
    }
    for (size_t index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *method = &graph->methods[index];
        if (method->owner == behavior && !method->class_side
                && text_is(method->selector, "lookupSelector:")) {
            return method;
        }
    }
    return NULL;
}

static bool analyze(const st_class_graph_result_t *graph,
                    const st_class_graph_method_t *method,
                    st_sema_result_t *sema)
{
    st_class_graph_sema_view_t view;
    bool ok = false;
    st_sema_result_init(sema);
    st_class_graph_sema_view_init(&view);
    if (method != NULL
            && st_class_graph_sema_view_build_minimal(
                &view, graph, method->id) == ST_CLASS_GRAPH_OK) {
        ok = st_sema_analyze_method(
                 sema, method->node, &view.catalog) == ST_SEMA_OK
            && st_sema_succeeded(sema);
    }
    st_class_graph_sema_view_destroy(&view);
    return ok;
}

static const st_primitive_binding_t *find_binding(
    const st_primitive_result_t *primitives,
    const st_ast_node_t *method)
{
    for (size_t index = 0u; index < primitives->binding_count; index++) {
        if (primitives->bindings[index].method == method) {
            return &primitives->bindings[index];
        }
    }
    return NULL;
}

static bool write_text(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool cross_assemble_arm64(const char *assembly)
{
    if (access("/usr/bin/clang", X_OK) != 0) {
        return true;
    }
    char source[] = "/tmp/anvil-st-reflection-arm64-XXXXXX.s";
    int descriptor = mkstemps(source, 2);
    if (descriptor < 0) {
        return false;
    }
    FILE *file = fdopen(descriptor, "wb");
    bool ok = file != NULL && fputs(assembly, file) >= 0;
    if (file != NULL) {
        ok = fclose(file) == 0 && ok;
    } else {
        close(descriptor);
    }
    char object[192];
    char command[640];
    int object_length = snprintf(object, sizeof(object), "%s.o", source);
    int command_length = snprintf(
        command, sizeof(command),
        "/usr/bin/clang --target=aarch64-linux-gnu -c %s -o %s",
        source, object);
    if (object_length <= 0 || (size_t)object_length >= sizeof(object)
            || command_length <= 0
            || (size_t)command_length >= sizeof(command)) {
        ok = false;
    }
    if (ok) {
        ok = system(command) == 0;
    }
    unlink(source);
    unlink(object);
    return ok;
}

static bool execute_x86_64(const char *assembly, uint32_t root_capacity,
                           uint32_t safepoint_count)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long process_id = (long)getpid();
    char assembly_path[192];
    char executable_path[192];
    char command[6144];
    int assembly_length = snprintf(
        assembly_path, sizeof(assembly_path),
        "/tmp/anvil-st-reflection-%ld.s", process_id);
    int executable_length = snprintf(
        executable_path, sizeof(executable_path),
        "/tmp/anvil-st-reflection-%ld", process_id);
    if (assembly_length <= 0
            || (size_t)assembly_length >= sizeof(assembly_path)
            || executable_length <= 0
            || (size_t)executable_length >= sizeof(executable_path)
            || !write_text(assembly_path, assembly)) {
        return false;
    }
    int command_length = snprintf(
        command, sizeof(command),
        "/usr/bin/gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread "
        "-DGENERATED_ROOT_CAPACITY=%u -DGENERATED_SAFEPOINT_COUNT=%u "
        "-Iinclude -Isamples/smalltalk/include %s "
        "samples/smalltalk/tests/reflection_primitives_aot_harness.c "
        "samples/smalltalk/src/runtime/value.c "
        "samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/posix/runtime.c "
        "samples/smalltalk/src/runtime/lookup.c "
        "samples/smalltalk/src/runtime/send_bridge.c "
        "samples/smalltalk/src/runtime/control/control.c "
        "samples/smalltalk/src/runtime/control/control_roots.c "
        "samples/smalltalk/src/runtime/heap.c "
        "samples/smalltalk/src/runtime/image_runtime.c "
        "samples/smalltalk/src/runtime/primitives/primitive_bridge.c "
        "samples/smalltalk/src/runtime/primitives/reflection_primitives.c "
        "samples/smalltalk/src/runtime/primitives/"
        "reflection_primitive_bridge.c -o %s",
        root_capacity, safepoint_count, assembly_path, executable_path);
    bool ok = command_length > 0
        && (size_t)command_length < sizeof(command)
        && system(command) == 0 && system(executable_path) == 0;
    unlink(assembly_path);
    unlink(executable_path);
    return ok;
#else
    (void)assembly;
    (void)root_capacity;
    (void)safepoint_count;
    return true;
#endif
}

static bool lower_for_target(
    anvil_arch_t architecture,
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method,
    const st_primitive_binding_t *binding,
    char **assembly_out,
    uint32_t *root_capacity_out,
    uint32_t *safepoint_count_out)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(architecture);
    st_sema_result_t sema;
    st_lower_result_t result;
    char *assembly = NULL;
    size_t assembly_length = 0u;
    bool ok = context != NULL && analyze(graph, method, &sema);
    if (!ok) {
        if (context != NULL) {
            anvil_ctx_destroy(context);
        }
        return false;
    }
    st_lower_options_t options = {
        .symbol_name = "st_Behavior_lookupSelector_aot",
        .linkage = ANVIL_LINK_EXTERNAL,
        .primitive_binding = binding
    };
    st_lower_result_init(&result);
    ok = st_lower_method(
             &result, context, graph, method->id,
             &sema, &options) == ST_LOWER_OK
        && result.module != NULL && result.function != NULL
        && result.has_primitive
        && result.primitive_failure_policy == ST_PRIMITIVE_FALL_THROUGH
        && result.required_root_capacity > 0u
        && result.safepoint_count > 0u
        && result.root_map_count == result.safepoint_count
        && anvil_module_codegen(
            result.module, &assembly, &assembly_length) == ANVIL_OK
        && assembly != NULL && assembly_length != 0u
        && strstr(
            assembly,
            "st_aot_behavior_lookup_selector_primitive_execute") != NULL;
    if (ok) {
        *assembly_out = assembly;
        *root_capacity_out = result.required_root_capacity;
        *safepoint_count_out = result.safepoint_count;
        assembly = NULL;
    }
    free(assembly);
    st_lower_result_destroy(&result);
    st_sema_result_destroy(&sema);
    anvil_ctx_destroy(context);
    return ok;
}

int main(void)
{
    static const char source[] =
        "Object := nil [ ] "
        "Behavior := Object [ "
        "lookupSelector: aSelector [ "
        "<primitive: BehaviorLookupSelectorPrimitive> ^self "
        "] ]";
    fixture_t fixture;
    CHECK(fixture_init(&fixture, source));
    if (failures != 0u) {
        return EXIT_FAILURE;
    }

    const st_ast_unit_t *units[1] = { &fixture.unit };
    st_class_graph_result_t graph;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL)
          == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    const st_class_graph_method_t *method = find_method(&graph);
    CHECK(method != NULL);

    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t primitives;
    st_primitive_result_init(&primitives);
    CHECK(st_primitive_catalog_init(
        &catalog, (st_primitive_allocator_t) {0}));
    size_t spec_count = 0u;
    const st_primitive_spec_t *specs = st_reflection_primitive_specs(
        &spec_count);
    CHECK(specs != NULL && spec_count == 1u);
    CHECK(st_primitive_catalog_register(&catalog, &specs[0], NULL)
          == ST_PRIMITIVE_OK);
    CHECK(st_primitive_resolve(
              &primitives, units, 1u, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(st_primitive_result_succeeded(&primitives));
    const st_primitive_binding_t *binding = find_binding(
        &primitives, method == NULL ? NULL : method->node);
    CHECK(binding != NULL && binding->primitive != NULL
          && binding->primitive->implementation_kind
              == ST_PRIMITIVE_RUNTIME_SYMBOL);

    char *x86_assembly = NULL;
    uint32_t x86_roots = 0u;
    uint32_t x86_safepoints = 0u;
    CHECK(lower_for_target(
        ANVIL_ARCH_X86_64, &graph, method, binding,
        &x86_assembly, &x86_roots, &x86_safepoints));
    if (x86_assembly != NULL) {
        CHECK(execute_x86_64(
            x86_assembly, x86_roots, x86_safepoints));
    }
    free(x86_assembly);

    char *arm_assembly = NULL;
    uint32_t arm_roots = 0u;
    uint32_t arm_safepoints = 0u;
    CHECK(lower_for_target(
        ANVIL_ARCH_ARM64, &graph, method, binding,
        &arm_assembly, &arm_roots, &arm_safepoints));
    CHECK(arm_roots == x86_roots
          && arm_safepoints == x86_safepoints);
    if (arm_assembly != NULL) {
        CHECK(cross_assemble_arm64(arm_assembly));
    }
    free(arm_assembly);

    st_primitive_result_destroy(&primitives);
    st_primitive_catalog_destroy(&catalog);
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
    if (failures != 0u) {
        fprintf(stderr, "reflection lowering: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk reflection lowering: PASS (x86_64 exec, ARM64 asm)");
    return EXIT_SUCCESS;
}
