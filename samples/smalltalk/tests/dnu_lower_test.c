#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_lower.h"
#include "st_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    char *source;
    st_ast_unit_t unit;
    st_parser_t parser;
} fixture_t;

typedef struct {
    char *assembly;
    uint32_t root_capacity;
    uint32_t safepoint_count;
} lowered_t;

static bool text_is(st_ast_string_t text, const char *expected)
{
    size_t length = strlen(expected);
    return text.data != NULL && text.length == length
        && memcmp(text.data, expected, length) == 0;
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
    if (!st_ast_unit_init(&fixture->unit, "DnuAot.st")
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
    st_class_graph_id_t owner = ST_CLASS_GRAPH_INVALID_ID;
    for (size_t index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && text_is(entity->name, "DnuProbe")) {
            owner = entity->id;
            break;
        }
    }
    for (size_t index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *method = &graph->methods[index];
        if (method->owner == owner && text_is(method->selector, "run:")) {
            return method;
        }
    }
    return NULL;
}

static bool analyze(
    const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method, st_sema_result_t *sema)
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

static bool lower_target(
    anvil_arch_t architecture, const st_class_graph_result_t *graph,
    const st_class_graph_method_t *method,
    const st_selector_table_t *selectors, lowered_t *lowered)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(architecture);
    st_sema_result_t sema;
    st_lower_result_t result;
    size_t assembly_length = 0u;
    memset(lowered, 0, sizeof(*lowered));
    st_lower_result_init(&result);
    bool ok = context != NULL && analyze(graph, method, &sema);
    if (!ok) {
        if (context != NULL) {
            anvil_ctx_destroy(context);
        }
        return false;
    }
    st_lower_options_t options = {
        .symbol_name = "st_DnuProbe_run",
        .linkage = ANVIL_LINK_EXTERNAL,
        .selectors = selectors
    };
    ok = st_lower_method(
             &result, context, graph, method->id, &sema, &options)
            == ST_LOWER_OK
        && result.module != NULL
        && result.function != NULL
        && result.method_flags == ST_METHOD_CAN_UNWIND
        && result.required_root_capacity != 0u
        && result.required_root_capacity <= 64u
        && result.safepoint_count != 0u
        && result.root_map_count == result.safepoint_count
        && anvil_module_codegen(
            result.module, &lowered->assembly, &assembly_length) == ANVIL_OK
        && lowered->assembly != NULL
        && assembly_length != 0u
        && strstr(lowered->assembly, "st_aot_send_resolve") != NULL
        && strstr(lowered->assembly, "st_aot_send_failure") != NULL
        && strstr(lowered->assembly, "st_aot_control_scope_enter") != NULL
        && strstr(lowered->assembly, "st_aot_control_scope_leave") != NULL;
    if (ok) {
        lowered->root_capacity = result.required_root_capacity;
        lowered->safepoint_count = result.safepoint_count;
    } else {
        free(lowered->assembly);
        memset(lowered, 0, sizeof(*lowered));
    }
    st_lower_result_destroy(&result);
    st_sema_result_destroy(&sema);
    anvil_ctx_destroy(context);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool cross_assemble_arm64(const char *assembly)
{
    if (access("/usr/bin/clang", X_OK) != 0) {
        return true;
    }
    char source[] = "/tmp/anvil-st-dnu-arm64-XXXXXX.s";
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

static bool execute_x86_64(const lowered_t *lowered)
{
#if defined(__x86_64__) && !defined(_WIN32)
    char assembly_path[192];
    char executable_path[192];
    char command[8192];
    long process_id = (long)getpid();
    int assembly_length = snprintf(
        assembly_path, sizeof(assembly_path),
        "/tmp/anvil-st-dnu-%ld.s", process_id);
    int executable_length = snprintf(
        executable_path, sizeof(executable_path),
        "/tmp/anvil-st-dnu-%ld", process_id);
    if (assembly_length <= 0
            || (size_t)assembly_length >= sizeof(assembly_path)
            || executable_length <= 0
            || (size_t)executable_length >= sizeof(executable_path)
            || !write_text(assembly_path, lowered->assembly)) {
        return false;
    }
    int command_length = snprintf(
        command, sizeof(command),
        "cc -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread "
        "-DST_DNU_AOT_HARNESS -DGENERATED_ROOT_CAPACITY=%u "
        "-DGENERATED_SAFEPOINT_COUNT=%u -Iinclude "
        "-Isamples/smalltalk/include %s "
        "samples/smalltalk/tests/dnu_test.c "
        "samples/smalltalk/src/runtime/value.c "
        "samples/smalltalk/src/runtime/runtime.c "
        "samples/smalltalk/src/runtime/lookup.c "
        "samples/smalltalk/src/runtime/send_bridge.c "
        "samples/smalltalk/src/runtime/control/control.c "
        "samples/smalltalk/src/runtime/control/control_roots.c "
        "samples/smalltalk/src/runtime/control/control_bridge.c "
        "samples/smalltalk/src/runtime/heap.c "
        "samples/smalltalk/src/runtime/image_runtime.c "
        "samples/smalltalk/src/runtime/primitives/core_primitives.c "
        "samples/smalltalk/src/runtime/primitives/heap_primitives.c "
        "samples/smalltalk/src/runtime/primitives/symbol_intern.c "
        "samples/smalltalk/src/runtime/primitives/reflection_primitives.c "
        "samples/smalltalk/src/runtime/aot_bootstrap.c "
        "samples/smalltalk/src/runtime/dnu.c -o %s",
        lowered->root_capacity, lowered->safepoint_count,
        assembly_path, executable_path);
    bool ok = command_length > 0
        && (size_t)command_length < sizeof(command)
        && system(command) == 0
        && system(executable_path) == 0;
    unlink(assembly_path);
    unlink(executable_path);
    return ok;
#else
    (void)lowered;
    return true;
#endif
}

int main(void)
{
    static const char source[] =
        "Object := nil [ ] "
        "DnuProbe := Object [ "
        "run: receiver [ ^receiver missing: 42 ] "
        "]";
    fixture_t fixture;
    CHECK(fixture_init(&fixture, source));
    if (failures != 0u) {
        return 1;
    }
    const st_ast_unit_t *units[1] = { &fixture.unit };
    st_class_graph_result_t graph;
    st_class_graph_result_init(&graph);
    CHECK(st_class_graph_build(&graph, units, 1u, NULL) == ST_CLASS_GRAPH_OK);
    CHECK(st_class_graph_succeeded(&graph));
    const st_class_graph_method_t *method = find_method(&graph);
    CHECK(method != NULL);

    st_selector_table_t selectors = {0};
    st_selector_id_t selector_id = 0u;
    CHECK(st_selector_table_init(
        &selectors, (st_selector_allocator_t) {0}, UINT64_C(0x444e5531)));
    CHECK(st_selector_intern(
              &selectors, "doesNotUnderstand:", 18u, &selector_id)
          == ST_SELECTOR_OK && selector_id == 1u);
    CHECK(st_selector_intern(
              &selectors, "missing:", 8u, &selector_id)
          == ST_SELECTOR_OK && selector_id == 2u);
    CHECK(st_selector_intern(
              &selectors, "unaryMissing", 12u, &selector_id)
          == ST_SELECTOR_OK && selector_id == 3u);
    CHECK(st_selector_table_freeze(&selectors));

    lowered_t x86 = {0};
    CHECK(lower_target(
        ANVIL_ARCH_X86_64, &graph, method, &selectors, &x86));
    if (x86.assembly != NULL) {
        CHECK(execute_x86_64(&x86));
    }
    lowered_t arm = {0};
    CHECK(lower_target(
        ANVIL_ARCH_ARM64, &graph, method, &selectors, &arm));
    CHECK(arm.root_capacity == x86.root_capacity);
    CHECK(arm.safepoint_count == x86.safepoint_count);
    if (arm.assembly != NULL) {
        CHECK(cross_assemble_arm64(arm.assembly));
    }

    free(x86.assembly);
    free(arm.assembly);
    st_selector_table_destroy(&selectors);
    st_class_graph_result_destroy(&graph);
    fixture_destroy(&fixture);
    if (failures != 0u) {
        fprintf(stderr, "Smalltalk DNU lowering: %u failure(s)\n", failures);
        return 1;
    }
    puts("Smalltalk DNU lowering: PASS (x86-64 asm/link/exec, ARM64 cross-asm)");
    return 0;
}
