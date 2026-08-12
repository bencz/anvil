#include "st_class_graph.h"
#include "st_image_emit.h"
#include "st_source_bundle.h"

#include <anvil/anvil.h>
#include <anvil/anvil_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

static bool command_succeeded(const char *label, const char *command)
{
    int status = system(command);
    if (status == 0) {
        return true;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "%s exit=%d\n", label, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s signal=%d\n", label, WTERMSIG(status));
    } else {
        fprintf(stderr, "%s returned system status=%d\n", label, status);
    }
    return false;
}

typedef struct allocation_header {
    size_t size;
} allocation_header_t;

typedef struct {
    size_t fail_after;
    size_t successes;
    size_t live;
} failing_allocator_t;

typedef struct {
    st_source_bundle_t bundle;
    st_class_graph_result_t graph;
    st_selector_table_t selectors;
    const st_ast_unit_t **units;
} image_fixture_t;

typedef struct {
    st_image_aot_method_artifact_t *items;
    char **symbols;
    size_t count;
    size_t run_index;
    st_image_root_map_metadata_t run_maps[2];
    uint64_t run_bitmaps[2];
} artifact_fixture_t;

static void *failing_allocate(void *user, size_t size)
{
    failing_allocator_t *state = user;
    allocation_header_t *header;
    if (state->successes == state->fail_after) return NULL;
    if (size > SIZE_MAX - sizeof(*header)) return NULL;
    header = malloc(sizeof(*header) + size);
    if (header == NULL) return NULL;
    header->size = size;
    state->successes++;
    state->live++;
    return header + 1;
}

static void failing_deallocate(void *user, void *pointer)
{
    failing_allocator_t *state = user;
    allocation_header_t *header;
    if (pointer == NULL) return;
    header = (allocation_header_t *)pointer - 1;
    (void)header->size;
    CHECK(state->live != 0u);
    state->live--;
    free(header);
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static bool fixture_init(image_fixture_t *fixture)
{
    const char *image = first_existing(
        "st-image", "samples/smalltalk/st-image");
    const char *application = first_existing(
        "tests/fixtures/HelloApplication.st",
        "samples/smalltalk/tests/fixtures/HelloApplication.st");
    const char *applications[1];
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    if (image == NULL || application == NULL) return false;
    applications[0] = application;
    if (st_source_bundle_load(&fixture->bundle, image, applications, 1u, NULL)
            != ST_SOURCE_LOAD_OK) return false;
    fixture->units = malloc(fixture->bundle.count * sizeof(*fixture->units));
    if (fixture->units == NULL) return false;
    for (index = 0u; index < fixture->bundle.count; index++)
        fixture->units[index] = &fixture->bundle.files[index].ast;
    st_class_graph_result_init(&fixture->graph);
    if (st_class_graph_build(&fixture->graph, fixture->units,
                             fixture->bundle.count, NULL)
                != ST_CLASS_GRAPH_OK
            || !st_class_graph_succeeded(&fixture->graph)) {
        return false;
    }
    return st_selector_table_build_for_units(
               &fixture->selectors, fixture->units, fixture->bundle.count,
               (st_selector_allocator_t){0},
               UINT64_C(0x5354494d41474535)) == ST_SELECTOR_OK;
}

static void fixture_destroy(image_fixture_t *fixture)
{
    st_selector_table_destroy(&fixture->selectors);
    st_class_graph_result_destroy(&fixture->graph);
    free(fixture->units);
    st_source_bundle_destroy(&fixture->bundle);
    memset(fixture, 0, sizeof(*fixture));
}

static bool artifact_fixture_init(artifact_fixture_t *artifacts,
                                  const image_fixture_t *fixture)
{
    size_t index;
    memset(artifacts, 0, sizeof(*artifacts));
    artifacts->run_index = SIZE_MAX;
    artifacts->count = fixture->graph.method_count;
    artifacts->items = calloc(artifacts->count, sizeof(*artifacts->items));
    artifacts->symbols = calloc(artifacts->count, sizeof(*artifacts->symbols));
    if (artifacts->count != 0u
            && (artifacts->items == NULL || artifacts->symbols == NULL)) {
        free(artifacts->symbols);
        free(artifacts->items);
        memset(artifacts, 0, sizeof(*artifacts));
        return false;
    }
    for (index = 0u; index < artifacts->count; index++) {
        const st_class_graph_method_t *method = &fixture->graph.methods[index];
        st_image_aot_method_artifact_t *artifact = &artifacts->items[index];
        int symbol_length;
        artifact->method_id = method->id;
        artifact->owner = method->owner;
        artifact->selector = method->selector.data;
        artifact->selector_length = method->selector.length;
        artifact->arity = (uint32_t)method->node->as.method.arguments.count;
        artifacts->symbols[index] = malloc(32u);
        if (!artifacts->symbols[index]) goto fail;
        symbol_length = snprintf(artifacts->symbols[index], 32u,
                                 "st_test_method_%08u", method->id);
        if (symbol_length <= 0 || symbol_length >= 32) goto fail;
        artifact->symbol = artifacts->symbols[index];
        artifact->symbol_length = (size_t)symbol_length;
        if (method->selector.length == 3u &&
            memcmp(method->selector.data, "run", 3u) == 0) {
            artifacts->run_index = index;
        }
    }
    if (artifacts->count != 0u)
        artifacts->items[0].flags = ST_METHOD_PRIMITIVE;
    if (artifacts->run_index == SIZE_MAX) {
        free(artifacts->items);
        memset(artifacts, 0, sizeof(*artifacts));
        return false;
    }
    artifacts->run_bitmaps[0] = UINT64_C(0x15);
    artifacts->run_bitmaps[1] = UINT64_C(0x5);
    artifacts->run_maps[0].safepoint_id = 1u;
    artifacts->run_maps[0].root_count = 5u;
    artifacts->run_maps[0].bitmap_word_count = 1u;
    artifacts->run_maps[0].live_root_bitmap = &artifacts->run_bitmaps[0];
    artifacts->run_maps[1].safepoint_id = 9u;
    artifacts->run_maps[1].root_count = 3u;
    artifacts->run_maps[1].bitmap_word_count = 1u;
    artifacts->run_maps[1].live_root_bitmap = &artifacts->run_bitmaps[1];
    artifacts->items[artifacts->run_index].frame_root_capacity = 5u;
    artifacts->items[artifacts->run_index].root_maps = artifacts->run_maps;
    artifacts->items[artifacts->run_index].root_map_count = 2u;
    return true;
fail:
    for (index = 0u; index < artifacts->count; index++)
        free(artifacts->symbols[index]);
    free(artifacts->symbols);
    free(artifacts->items);
    memset(artifacts, 0, sizeof(*artifacts));
    return false;
}

static void artifact_fixture_destroy(artifact_fixture_t *artifacts)
{
    size_t index;
    for (index = 0u; index < artifacts->count; index++)
        free(artifacts->symbols[index]);
    free(artifacts->symbols);
    free(artifacts->items);
    memset(artifacts, 0, sizeof(*artifacts));
}

static bool write_file(const char *path, const char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (ok && length != 0u) ok = fwrite(bytes, 1u, length, file) == length;
    if (file != NULL && fclose(file) != 0) ok = false;
    return ok;
}

static void test_native_object_and_link(const char *assembly, size_t length,
                                        const st_image_emit_result_t *result,
                                        uint32_t application_class_id)
{
    char assembly_path[128], object_path[128], harness_path[128];
    char executable_path[128], command[1024], harness[65536];
    long process = (long)getpid();
    int harness_length;
    snprintf(assembly_path, sizeof(assembly_path),
             "/tmp/anvil-st-image-%ld.s", process);
    snprintf(object_path, sizeof(object_path),
             "/tmp/anvil-st-image-%ld.o", process);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-image-%ld.c", process);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-image-%ld", process);
    harness_length = snprintf(harness, sizeof(harness),
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "#include <string.h>\n"
        "typedef struct { uint32_t id, kind; uint64_t hash; uint32_t arity; "
        "const char *spelling; } Selector;\n"
        "typedef struct StFrame StFrame;\n"
        "typedef uint64_t (*Code)(StFrame *);\n"
        "typedef struct { uint32_t id, owner, instance_class, lexical_super; "
        "uint32_t selector_id, arity, flags, origin_unit, origin_line, "
        "origin_column, frame_root_capacity, aot_flags; Code code; "
        "const void *root_maps; size_t root_map_count; "
        "const char *selector, *source_name; const void *runtime_descriptor; } Method;\n"
        "typedef struct { uint32_t id, kind, namespace_id, superclass_id; "
        "uint32_t metaclass_id, instance_class_id, instance_slot_offset; "
        "uint32_t instance_slot_count, class_variable_offset; "
        "uint32_t class_variable_count, method_offset, method_count; "
        "uint32_t origin_unit, origin_line, origin_column; const char *name; "
        "} Entity;\n"
        "typedef struct { uint64_t magic; uint32_t abi, flags, ptr_size, endian; "
        "uint32_t image_sources, app_sources, entities, classes, metas, spaces; "
        "uint32_t methods, selectors, instance_slots, class_variables, strings; "
        "uint32_t blocks, captures, globals, literals, literal_bytes; "
        "uint32_t runtime_classes, runtime_shapes, runtime_layouts; "
        "const void *entity_table, *method_table, *selector_table; "
        "const void *instance_table, *class_table, *string_table; "
        "const void *runtime_methods, *block_descriptors; "
        "const void *global_table, *literal_table; "
        "const void *entity_runtime_ids, *layout_table, *runtime_descriptors; "
        "} Header;\n"
        "extern const Header st_image_descriptor;\n"
        "int main(void) { const Header *h = &st_image_descriptor; "
        "const Entity *e = h->entity_table; const Method *m = h->method_table; "
        "const Selector *s = h->selector_table; size_t i; int saw_run = 0; "
        "int metadata_clean = 1; for (i = 0; i < h->methods; ++i) { "
        "if (m[i].code || m[i].root_maps || m[i].root_map_count || "
        "m[i].frame_root_capacity || m[i].aot_flags || "
        "m[i].runtime_descriptor) metadata_clean = 0; "
        "if (m[i].owner == %u && strcmp(m[i].selector, \"run\") == 0) "
        "saw_run = 1; } "
        "return !(h->magic == UINT64_C(0x%llx) && h->abi == %u && "
        "(h->flags & %u) != 0 && "
        "h->ptr_size == sizeof(void *) && h->image_sources == %zu && "
        "h->app_sources == %zu && h->entities == %zu && "
        "h->methods == %zu && h->selectors == %zu && e && m && s && "
        "h->string_table && e[0].id == 1 && strcmp(e[0].name, \"Object\") == 0 "
        "&& e[%u].id == %u && strcmp(e[%u].name, \"HelloApplication\") == 0 "
        "&& s[0].id == 1 && s[0].spelling && saw_run && metadata_clean); }\n",
        (unsigned)application_class_id,
        (unsigned long long)ST_IMAGE_METADATA_MAGIC,
        (unsigned)ST_IMAGE_METADATA_ABI_VERSION,
        (unsigned)ST_IMAGE_METADATA_FLAG_METADATA_ONLY,
        result->image_source_count,
        result->source_count - result->image_source_count,
        result->entity_count, result->method_count, result->selector_count,
        (unsigned)(application_class_id - 1u),
        (unsigned)application_class_id,
        (unsigned)(application_class_id - 1u));
    CHECK(harness_length > 0 && (size_t)harness_length < sizeof(harness));
    CHECK(write_file(assembly_path, assembly, length));
    CHECK(write_file(harness_path, harness, (size_t)harness_length));
    snprintf(command, sizeof(command),
             "/usr/bin/cc -c %s -o %s && /usr/bin/cc %s %s -o %s && %s",
             assembly_path, object_path, harness_path, object_path,
             executable_path, executable_path);
    CHECK(command_succeeded("image metadata harness", command));
    unlink(executable_path);
    unlink(harness_path);
    unlink(object_path);
    unlink(assembly_path);
}

static void test_native_aot_object_and_link(const char *assembly, size_t length,
                                             size_t method_count,
                                             uint32_t run_method_id)
{
    char assembly_path[128], object_path[128], harness_path[128];
    char executable_path[128], command[1024], harness[65536];
    long process = (long)getpid();
    int harness_length;
    snprintf(assembly_path, sizeof(assembly_path),
             "/tmp/anvil-st-image-aot-%ld.s", process);
    snprintf(object_path, sizeof(object_path),
             "/tmp/anvil-st-image-aot-%ld.o", process);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-st-image-aot-%ld.c", process);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-st-image-aot-%ld", process);
    harness_length = snprintf(harness, sizeof(harness),
        "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n"
        "typedef uint64_t Value; typedef struct StFrame { uint64_t marker; } StFrame;\n"
        "typedef Value (*Code)(StFrame *);\n"
        "typedef struct { uint32_t safepoint_id, root_count; size_t words; "
        "const uint64_t *bitmap; } RootMap;\n"
        "typedef struct { uint32_t id, owner, instance_class, lexical_super; "
        "uint32_t selector_id, arity, flags, origin_unit, origin_line, "
        "origin_column, frame_root_capacity, aot_flags; Code code; "
        "const RootMap *root_maps; size_t root_map_count; "
        "const char *selector, *source_name; const void *runtime_descriptor; } Method;\n"
        "typedef struct { uint64_t magic; uint32_t abi, flags, ptr_size, endian; "
        "uint32_t image_sources, app_sources, entities, classes, metas, spaces; "
        "uint32_t methods, selectors, instance_slots, class_variables, strings; "
        "uint32_t blocks, captures, globals, literals, literal_bytes; "
        "uint32_t runtime_classes, runtime_shapes, runtime_layouts; "
        "const void *entity_table; const Method *method_table; "
        "const void *selector_table, *instance_table, *class_table, *string_table; "
        "const void *runtime_methods, *block_descriptors; "
        "const void *global_table, *literal_table; "
        "const void *entity_runtime_ids, *layout_table, *runtime_descriptors; "
        "} Header;\n"
        "typedef struct { uint32_t abi, selector, owner, arity, roots, flags, code_size; "
        "const char *source; size_t source_length, begin, end; const RootMap *maps; "
        "size_t map_count; const void *unwind; size_t unwind_count; } RuntimeMethod;\n");
    CHECK(harness_length > 0 && (size_t)harness_length < sizeof(harness));
    for (size_t method_index = 0u;
         harness_length > 0 && method_index < method_count; method_index++) {
        int added = snprintf(harness + harness_length,
            sizeof(harness) - (size_t)harness_length,
            "Value st_test_method_%08zu(StFrame *f) { return f && f->marker == "
            "UINT64_C(0x1234) ? UINT64_C(0x%04x) : 0; }\n",
            method_index + 1u, ((method_index + 1u) & 1u) ? 0x1111 : 0x2222);
        CHECK(added > 0 && (size_t)added
              < sizeof(harness) - (size_t)harness_length);
        if (added <= 0 || (size_t)added
                >= sizeof(harness) - (size_t)harness_length) {
            harness_length = -1;
            break;
        }
        harness_length += added;
    }
    if (harness_length > 0) {
        int added = snprintf(harness + harness_length,
            sizeof(harness) - (size_t)harness_length,
        "extern const Header st_image_descriptor;\n"
        "int main(void) { const Header *h = &st_image_descriptor; size_t i; "
        "int saw_run = 0; StFrame frame = { UINT64_C(0x1234) }; "
        "if (h->methods != %zu || !(h->flags & %u) || !(h->flags & %u) || "
        "!(h->flags & %u) || (h->flags & %u) || h->blocks || h->captures || "
        "!h->runtime_methods || h->block_descriptors) return 1; "
        "for (i = 0; i < h->methods; ++i) { const Method *m = &h->method_table[i]; "
        "const RuntimeMethod *r = m->runtime_descriptor; "
        "Value expected = (m->id & 1) ? UINT64_C(0x1111) : UINT64_C(0x2222); "
        "if (!m->code || m->code(&frame) != expected) return 2; "
        "if (!r || r->abi != 2 || r->selector != m->selector_id || "
        "r->owner != m->owner || r->arity != m->arity || r->roots != "
        "m->frame_root_capacity || r->flags != m->aot_flags || r->code_size || "
        "r->maps != m->root_maps || r->map_count != m->root_map_count || "
        "r->unwind || r->unwind_count) return 6; "
        "if ((m->id == 1 && m->aot_flags != %u) || "
        "(m->id != 1 && m->aot_flags != 0)) return 5; "
        "if (m->id == %u) { const RootMap *r = m->root_maps; saw_run = 1; "
        "if (m->frame_root_capacity != 5 || m->root_map_count != 2 || !r || "
        "r[0].safepoint_id != 1 || r[0].root_count != 5 || r[0].words != 1 || "
        "!r[0].bitmap || r[0].bitmap[0] != UINT64_C(0x15) || "
        "r[1].safepoint_id != 9 || r[1].root_count != 3 || r[1].words != 1 || "
        "!r[1].bitmap || r[1].bitmap[0] != UINT64_C(0x5)) return 3; "
        "} else if (m->root_maps || m->root_map_count || "
        "m->frame_root_capacity) return 4; } return !saw_run; }\n",
        method_count, (unsigned)ST_IMAGE_METADATA_FLAG_METHOD_CODE,
        (unsigned)ST_IMAGE_METADATA_FLAG_ROOT_MAPS,
        (unsigned)ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS,
        (unsigned)ST_IMAGE_METADATA_FLAG_METADATA_ONLY,
        (unsigned)ST_METHOD_PRIMITIVE,
        (unsigned)run_method_id);
        CHECK(added > 0 && (size_t)added
              < sizeof(harness) - (size_t)harness_length);
        if (added > 0 && (size_t)added
                < sizeof(harness) - (size_t)harness_length)
            harness_length += added;
        else harness_length = -1;
    }
    CHECK(harness_length > 0 && (size_t)harness_length < sizeof(harness));
    CHECK(write_file(assembly_path, assembly, length));
    CHECK(write_file(harness_path, harness, (size_t)harness_length));
    snprintf(command, sizeof(command),
             "/usr/bin/cc -c %s -o %s && /usr/bin/cc %s %s -o %s && %s",
             assembly_path, object_path, harness_path, object_path,
             executable_path, executable_path);
    CHECK(command_succeeded("image AOT harness", command));
    unlink(executable_path);
    unlink(harness_path);
    unlink(object_path);
    unlink(assembly_path);
}

static void test_cross_assemble_arm64(const char *assembly, size_t length)
{
    char assembly_path[128], object_path[128], command[512];
    long process = (long)getpid();
    if (access("/usr/bin/clang", X_OK) != 0) return;
    snprintf(assembly_path, sizeof(assembly_path),
             "/tmp/anvil-st-image-arm64-%ld.s", process);
    snprintf(object_path, sizeof(object_path),
             "/tmp/anvil-st-image-arm64-%ld.o", process);
    CHECK(write_file(assembly_path, assembly, length));
    snprintf(command, sizeof(command),
             "/usr/bin/clang --target=aarch64-linux-gnu -c %s -o %s",
             assembly_path, object_path);
    CHECK(command_succeeded("image ARM64 assembler", command));
    unlink(object_path);
    unlink(assembly_path);
}

static void test_target(const image_fixture_t *fixture, anvil_arch_t arch)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(arch);
    st_image_emit_options_t options;
    st_image_emit_result_t result;
    char error[256];
    char *assembly = NULL;
    size_t length = 0u;
    uint32_t application_class_id = 0u;
    size_t entity_index;
    CHECK(context != NULL);
    if (context == NULL) return;
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    st_image_emit_result_init(&result);
    CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                                 &fixture->graph, &options) == ST_IMAGE_EMIT_OK);
    CHECK(result.module != NULL);
    CHECK(result.source_count == fixture->bundle.count);
    CHECK(result.image_source_count == fixture->bundle.image_count);
    CHECK(result.entity_count == fixture->graph.entity_count);
    CHECK(result.method_count == fixture->graph.method_count);
    CHECK(result.instance_slot_count == fixture->graph.instance_slot_count);
    CHECK(result.class_variable_count == fixture->graph.class_variable_count);
    CHECK(result.selector_count == st_selector_count(&fixture->selectors));
    CHECK(result.string_bytes != 0u);
    for (entity_index = 0u; entity_index < fixture->graph.entity_count;
         entity_index++) {
        const st_class_graph_entity_t *entity =
            &fixture->graph.entities[entity_index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && entity->name.length == sizeof("HelloApplication") - 1u
                && memcmp(entity->name.data, "HelloApplication",
                          sizeof("HelloApplication") - 1u) == 0) {
            application_class_id = entity->id;
            break;
        }
    }
    CHECK(application_class_id != 0u);
    CHECK(anvil_module_lookup_symbol(result.module, "st_image_descriptor")
          != NULL);
    CHECK(anvil_module_lookup_symbol(result.module, "st_image_entities")
          != NULL);
    CHECK(anvil_module_lookup_symbol(result.module, "st_image_methods")
          != NULL);
    CHECK(anvil_module_lookup_symbol(result.module, "st_image_selectors")
          != NULL);
    CHECK(anvil_module_verify(result.module, error, sizeof(error)));
    CHECK(anvil_module_codegen(result.module, &assembly, &length) == ANVIL_OK);
    CHECK(assembly != NULL && length != 0u);
    if (assembly != NULL) {
        bool hlasm = arch == ANVIL_ARCH_S370 || arch == ANVIL_ARCH_S370_XA
            || arch == ANVIL_ARCH_S390 || arch == ANVIL_ARCH_ZARCH;
        if (!hlasm) {
            CHECK(strstr(assembly, "st_image_descriptor:") != NULL);
            CHECK(strstr(assembly, "st_image_entities") != NULL);
            CHECK(strstr(assembly, "st_image_methods") != NULL);
            CHECK(strstr(assembly, "st_image_selectors") != NULL);
            CHECK(strstr(assembly, "st_image_strings+") != NULL);
        }
        if (arch == ANVIL_ARCH_X86_64)
            test_native_object_and_link(assembly, length, &result,
                                        application_class_id);
        else if (arch == ANVIL_ARCH_ARM64)
            test_cross_assemble_arm64(assembly, length);
        free(assembly);
    }
    st_image_emit_result_destroy(&result);
    anvil_ctx_destroy(context);
}

static void test_aot_target(const image_fixture_t *fixture,
                            const artifact_fixture_t *artifacts,
                            anvil_arch_t arch)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(arch);
    st_image_emit_options_t options;
    st_image_emit_result_t result;
    char error[256];
    char *assembly = NULL;
    size_t length = 0u;
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    options.method_artifacts = artifacts->items;
    options.method_artifact_count = artifacts->count;
    options.require_method_code = true;
    CHECK(context != NULL);
    if (!context) return;
    st_image_emit_result_init(&result);
    if (arch == ANVIL_ARCH_X86 || arch == ANVIL_ARCH_PPC32
            || arch == ANVIL_ARCH_S370 || arch == ANVIL_ARCH_S370_XA
            || arch == ANVIL_ARCH_S390) {
        CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                                     &fixture->graph, &options)
              == ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET);
        CHECK(result.module == NULL && !result.has_method_code
              && result.method_count == 0u && result.block_count == 0u);
        st_image_emit_result_destroy(&result);
        anvil_ctx_destroy(context);
        return;
    }
    CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                                 &fixture->graph, &options)
          == ST_IMAGE_EMIT_OK);
    CHECK(result.module != NULL && result.has_method_code);
    CHECK(result.root_map_count == 2u && result.root_bitmap_word_count == 2u);
    CHECK(anvil_module_lookup_symbol(result.module, "st_image_root_maps")
          != NULL);
    CHECK(anvil_module_lookup_symbol(result.module,
                                     "st_image_root_bitmap_words") != NULL);
    CHECK(anvil_module_lookup_symbol(result.module, "st_test_method_00000001")
          != NULL);
    CHECK(anvil_module_lookup_symbol(result.module, "st_test_method_00000002")
          != NULL);
    CHECK(anvil_module_verify(result.module, error, sizeof(error)));
    CHECK(anvil_module_codegen(result.module, &assembly, &length) == ANVIL_OK);
    CHECK(assembly != NULL && length != 0u);
    if (assembly) {
        bool hlasm = arch == ANVIL_ARCH_S370 || arch == ANVIL_ARCH_S370_XA
            || arch == ANVIL_ARCH_S390 || arch == ANVIL_ARCH_ZARCH;
        if (!hlasm) {
            CHECK(strstr(assembly, "st_test_method_00000001") != NULL);
            CHECK(strstr(assembly, "st_test_method_00000002") != NULL);
            CHECK(strstr(assembly, "st_image_root_maps") != NULL);
            CHECK(strstr(assembly, "st_image_root_bitmap_words") != NULL);
        }
        if (arch == ANVIL_ARCH_X86_64)
            test_native_aot_object_and_link(assembly, length,
                fixture->graph.method_count,
                fixture->graph.methods[artifacts->run_index].id);
        else if (arch == ANVIL_ARCH_ARM64)
            test_cross_assemble_arm64(assembly, length);
        free(assembly);
    }
    st_image_emit_result_destroy(&result);
    anvil_ctx_destroy(context);
}

static void test_explicit_unavailable_code(const image_fixture_t *fixture)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    st_image_emit_options_t options;
    st_image_emit_result_t result;
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    options.require_method_code = true;
    st_image_emit_result_init(&result);
    CHECK(context != NULL);
    CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
              &fixture->graph, &options)
          == ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE);
    CHECK(result.module == NULL && result.entity_count == 0u);
    st_image_emit_result_destroy(&result);
    anvil_ctx_destroy(context);
}

static void expect_artifact_status(const image_fixture_t *fixture,
                                   const st_image_aot_method_artifact_t *items,
                                   size_t count, bool require_code,
                                   st_image_emit_status_t expected)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    st_image_emit_options_t options;
    st_image_emit_result_t result;
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    options.method_artifacts = items;
    options.method_artifact_count = count;
    options.require_method_code = require_code;
    st_image_emit_result_init(&result);
    CHECK(context != NULL);
    if (context) {
        CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                                     &fixture->graph, &options) == expected);
        if (expected == ST_IMAGE_EMIT_OK)
            CHECK(result.module != NULL && result.has_method_code);
        else
            CHECK(result.module == NULL && result.source_count == 0u &&
                  result.entity_count == 0u && result.method_count == 0u &&
                  result.root_map_count == 0u && !result.has_method_code);
    }
    st_image_emit_result_destroy(&result);
    anvil_ctx_destroy(context);
}

static void test_artifact_validation(const image_fixture_t *fixture,
                                     const artifact_fixture_t *base)
{
    st_image_aot_method_artifact_t *items = malloc(
        base->count * sizeof(*items));
    st_image_aot_method_artifact_t saved;
    st_image_root_map_metadata_t maps[2];
    uint64_t bitmap;
    st_aot_capture_descriptor_t cell_capture = { 0u, ST_AOT_CAPTURE_CELL };
    st_image_aot_block_artifact_t block = {
        .lexical_ordinal = 0u,
        .code_symbol = "st_test_block_code",
        .code_symbol_length = sizeof("st_test_block_code") - 1u,
        .descriptor_symbol = "st_test_block_descriptor",
        .descriptor_symbol_length = sizeof("st_test_block_descriptor") - 1u,
        .method_descriptor_symbol = "st_test_block_method_descriptor",
        .method_descriptor_symbol_length =
            sizeof("st_test_block_method_descriptor") - 1u,
        .captures = &cell_capture,
        .capture_count = 1u
    };
    size_t run = base->run_index;
    CHECK(items != NULL && base->count >= 2u);
    if (!items || base->count < 2u) { free(items); return; }
#define RESET_ITEMS() memcpy(items, base->items, base->count * sizeof(*items))
#define EXPECT_INVALID() expect_artifact_status(fixture, items, base->count, \
        true, ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT)
    RESET_ITEMS();
    expect_artifact_status(fixture, items, base->count - 1u, true,
                           ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT);
    RESET_ITEMS(); items[0].method_id = ST_CLASS_GRAPH_INVALID_ID;
    EXPECT_INVALID();
    RESET_ITEMS(); items[1].method_id = items[0].method_id; EXPECT_INVALID();
    RESET_ITEMS(); items[0].owner = ST_CLASS_GRAPH_INVALID_ID; EXPECT_INVALID();
    RESET_ITEMS(); items[0].selector_length++; EXPECT_INVALID();
    RESET_ITEMS(); items[0].arity ^= 1u; EXPECT_INVALID();
    RESET_ITEMS(); items[0].symbol = "bad@name";
    items[0].symbol_length = sizeof("bad@name") - 1u; EXPECT_INVALID();
    RESET_ITEMS(); items[0].symbol = "st_image_methods";
    items[0].symbol_length = sizeof("st_image_methods") - 1u; EXPECT_INVALID();
    RESET_ITEMS(); items[0].flags = UINT32_C(0x80000000); EXPECT_INVALID();
    RESET_ITEMS(); items[0].flags = ST_METHOD_HAS_NON_LOCAL_RETURN;
    EXPECT_INVALID();

    RESET_ITEMS(); block.flags = ST_AOT_BLOCK_HAS_CELLS;
    items[0].blocks = &block; items[0].block_count = 1u;
    expect_artifact_status(fixture, items, base->count, true,
                           ST_IMAGE_EMIT_OK);
    block.flags = 0u;

    RESET_ITEMS(); items[run].frame_root_capacity = 0u; EXPECT_INVALID();
    RESET_ITEMS(); items[run].root_maps = NULL; EXPECT_INVALID();
    RESET_ITEMS(); maps[0] = base->run_maps[0]; maps[0].safepoint_id = 0u;
    items[run].root_maps = maps; items[run].root_map_count = 1u;
    EXPECT_INVALID();
    RESET_ITEMS(); memcpy(maps, base->run_maps, sizeof(maps));
    maps[1].safepoint_id = maps[0].safepoint_id;
    items[run].root_maps = maps; EXPECT_INVALID();
    RESET_ITEMS(); maps[0] = base->run_maps[0]; maps[0].root_count = 6u;
    items[run].root_maps = maps; items[run].root_map_count = 1u;
    EXPECT_INVALID();
    RESET_ITEMS(); maps[0] = base->run_maps[0]; maps[0].bitmap_word_count = 0u;
    items[run].root_maps = maps; items[run].root_map_count = 1u;
    EXPECT_INVALID();
    RESET_ITEMS(); maps[0] = base->run_maps[0]; maps[0].live_root_bitmap = NULL;
    items[run].root_maps = maps; items[run].root_map_count = 1u;
    EXPECT_INVALID();
    RESET_ITEMS(); bitmap = UINT64_C(1) << 63; maps[0] = base->run_maps[1];
    maps[0].live_root_bitmap = &bitmap; items[run].root_maps = maps;
    items[run].root_map_count = 1u; EXPECT_INVALID();

    RESET_ITEMS(); maps[0].safepoint_id = 3u; maps[0].root_count = 0u;
    maps[0].bitmap_word_count = 0u; maps[0].live_root_bitmap = NULL;
    items[run].frame_root_capacity = 1u; items[run].root_maps = maps;
    items[run].root_map_count = 1u;
    items[0].flags = ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN;
    expect_artifact_status(fixture, items, base->count, true, ST_IMAGE_EMIT_OK);

    RESET_ITEMS(); saved = items[run];
    items[run].root_maps = &base->run_maps[0];
    items[run].root_map_count = SIZE_MAX / sizeof(base->run_maps[0]) + 1u;
    expect_artifact_status(fixture, items, base->count, true,
                           ST_IMAGE_EMIT_ERR_OVERFLOW);
    items[run] = saved;
    expect_artifact_status(fixture, NULL, 1u, true,
                           ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT);
    expect_artifact_status(fixture, items, 0u, false,
                           ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT);
#undef EXPECT_INVALID
#undef RESET_ITEMS
    free(items);
}

static st_image_emit_status_t emit_runtime_artifacts_once(
    const image_fixture_t *fixture,
    const st_image_aot_method_artifact_t *methods, size_t method_count,
    const st_image_global_artifact_t *globals, size_t global_count,
    st_image_emit_result_t *result_out)
{
    anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    st_image_emit_options_t options;
    st_image_emit_status_t status;
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    options.method_artifacts = methods;
    options.method_artifact_count = method_count;
    options.globals = globals;
    options.global_count = global_count;
    options.require_method_code = true;
    st_image_emit_result_init(result_out);
    CHECK(context != NULL);
    if (!context) return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
    status = st_image_emit_metadata(result_out, context, &fixture->bundle,
                                    &fixture->graph, &options);
    st_image_emit_result_destroy(result_out);
    anvil_ctx_destroy(context);
    return status;
}

static void test_native_runtime_metadata(
    const image_fixture_t *fixture,
    const st_image_aot_method_artifact_t *methods, size_t method_count,
    const st_image_global_artifact_t *globals, size_t global_count)
{
#if defined(__x86_64__) && !defined(_WIN32)
    static const char harness[] =
        "#include \"st_image_emit.h\"\n#include <string.h>\n"
        "extern const st_image_metadata_descriptor_t st_image_descriptor;\n"
        "st_value_t st_test_method_00000001(StFrame*f){(void)f;return 0;}\n"
        "st_value_t st_test_method_00000002(StFrame*f){(void)f;return 0;}\n"
        "int main(void){static const unsigned char p[]={97,0,98,255};"
        "const st_image_metadata_descriptor_t*d=&st_image_descriptor;"
        "if(!(d->abi_version==ST_IMAGE_METADATA_ABI_VERSION "
        "&& (d->flags&ST_IMAGE_METADATA_FLAG_IMAGE_RUNTIME_TABLES)"
        "&& (d->flags&ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS)"
        "&& d->runtime_descriptors && d->runtime_class_count>0 "
        "&& d->runtime_shape_count>=d->runtime_class_count "
        "))return 90;"
        "if(st_runtime_descriptors_validate(d->runtime_descriptors)!=ST_RUNTIME_OK)return 91;"
        "uint32_t ce=0,se=0,ye=0,bke=0,lpe=0,lne=0;size_t classSlots=0;"
        "for(size_t i=0;i<d->entity_count;i++){const st_image_entity_metadata_t*e=&d->entities[i];"
        "if(!strcmp(e->name,\"Class\")&&e->kind==ST_CLASS_GRAPH_CLASS){ce=e->id;classSlots=e->instance_slot_count;}"
        "else if(!strcmp(e->name,\"String\")&&e->kind==ST_CLASS_GRAPH_CLASS)se=e->id;"
        "else if(!strcmp(e->name,\"Symbol\")&&e->kind==ST_CLASS_GRAPH_CLASS)ye=e->id;"
        "else if(!strcmp(e->name,\"Block\")&&e->kind==ST_CLASS_GRAPH_CLASS)bke=e->id;"
        "else if(!strcmp(e->name,\"LargePositiveInteger\")&&e->kind==ST_CLASS_GRAPH_CLASS)lpe=e->id;"
        "else if(!strcmp(e->name,\"LargeNegativeInteger\")&&e->kind==ST_CLASS_GRAPH_CLASS)lne=e->id;}"
        "if(!ce||!se||!ye||!bke||!lpe||!lne)return 92;size_t sc=0,yc=0;"
        "for(size_t i=0;i<d->runtime_layout_count;i++){const st_image_runtime_layout_metadata_t*l=&d->runtime_layouts[i];"
        "const StShapeDescriptor*s=st_runtime_shape(d->runtime_descriptors,l->runtime_shape_id);if(!s)return 93;"
        "if(l->graph_entity_id==se)sc++;if(l->graph_entity_id==ye)yc++;"
        "if(l->graph_entity_id==bke&&(l->recipe!=ST_IMAGE_LAYOUT_CLOSURE||s->fixed_word_count!=4||"
        "s->indexed_format!=ST_INDEXED_VALUES||s->fixed_pointer_bitmap_word_count!=1||s->fixed_pointer_bitmap[0]))return 94;"
        "if((l->graph_entity_id==lpe||l->graph_entity_id==lne)&&(l->recipe!=ST_IMAGE_LAYOUT_LARGE_INTEGER||"
        "s->fixed_word_count!=1||s->indexed_format!=ST_INDEXED_UINT32||s->fixed_pointer_bitmap_word_count!=1||"
        "s->fixed_pointer_bitmap[0]))return 95;}if(sc!=3||yc!=3)return 96;"
        "uint32_t mc=0;for(size_t i=0;i<d->entity_count;i++)if(d->entities[i].kind==ST_CLASS_GRAPH_METACLASS){"
        "uint32_t c=d->entity_runtime_class_ids[i];const StClassDescriptor*k=st_runtime_class(d->runtime_descriptors,c);"
        "const StShapeDescriptor*s=k?st_runtime_shape(d->runtime_descriptors,k->default_shape_id):0;"
        "if(!s||s->fixed_word_count!=classSlots)return 97;if(!mc)mc=c;}"
        "const StClassDescriptor*mk=st_runtime_class(d->runtime_descriptors,mc);st_object_extent_t ex={0};"
        "st_value_t cv=ST_VALUE_INVALID;st_object_view_t view={0};if(!mk||st_object_allocate(d->runtime_descriptors,mc,"
        "mk->default_shape_id,0,0,0,(st_runtime_allocator_t){0},&ex,&cv)!=ST_RUNTIME_OK||"
        "st_object_validate(d->runtime_descriptors,cv,ex,&view)!=ST_RUNTIME_OK||view.shape_descriptor->fixed_word_count"
        "!=classSlots)return 98;for(size_t i=0;i<classSlots;i++)if(((st_value_t*)view.fixed_words)[i]!=st_value_nil())"
        "return 99;st_object_deallocate((st_runtime_allocator_t){0},ex);"
        "return !(d->global_count==2 && d->string_literal_count==1 && d->string_literal_bytes==4"
        "&& d->globals && d->globals[0].semantic_external_id==0xf0000001u"
        "&& d->globals[0].runtime_index==1 && strcmp(d->globals[0].name,\"Transcript\")==0"
        "&& d->globals[1].runtime_index==0 && strcmp(d->globals[1].name,\"Arguments\")==0"
        "&& d->string_literals && d->string_literals[0].literal_id==0"
        "&& d->string_literals[0].method_id!=0 && d->string_literals[0].length==4"
        "&& memcmp(d->string_literals[0].bytes,p,4)==0);}\n";
    anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    st_image_emit_options_t options;
    st_image_emit_result_t result;
    char *assembly = NULL;
    size_t assembly_length = 0u;
    char asm_path[160], object_path[160], source_path[160], exe_path[160];
    char command[2048];
    memset(&options, 0, sizeof(options));
    options.selectors = &fixture->selectors;
    options.method_artifacts = methods;
    options.method_artifact_count = method_count;
    options.globals = globals;
    options.global_count = global_count;
    options.require_method_code = true;
    st_image_emit_result_init(&result);
    CHECK(context != NULL);
    if (!context) return;
    CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
              &fixture->graph, &options) == ST_IMAGE_EMIT_OK);
    CHECK(anvil_module_codegen(result.module, &assembly, &assembly_length)
          == ANVIL_OK);
    snprintf(asm_path, sizeof(asm_path), "/tmp/anvil-st-meta-v4-%ld.s",
             (long)getpid());
    snprintf(object_path, sizeof(object_path), "/tmp/anvil-st-meta-v4-%ld.o",
             (long)getpid());
    snprintf(source_path, sizeof(source_path), "/tmp/anvil-st-meta-v4-%ld.c",
             (long)getpid());
    snprintf(exe_path, sizeof(exe_path), "/tmp/anvil-st-meta-v4-%ld",
             (long)getpid());
    CHECK(assembly && write_file(asm_path, assembly, assembly_length));
    {
        FILE *source = fopen(source_path, "wb");
        bool source_ok = source != NULL
            && fwrite(harness, 1u, sizeof(harness) - 1u, source)
                == sizeof(harness) - 1u;
        for (size_t index = 2u; source_ok && index < method_count; index++)
            source_ok = fprintf(source,
                "st_value_t st_test_method_%08zu(StFrame*f){(void)f;return 0;}\n",
                index + 1u) > 0;
        if (source != NULL && fclose(source) != 0) source_ok = false;
        CHECK(source_ok);
    }
    snprintf(command, sizeof(command),
        "/usr/bin/cc -c %s -o %s && /usr/bin/cc -Iinclude -Isamples/smalltalk/include "
        "%s %s samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c "
        "-o %s && %s", asm_path, object_path, source_path, object_path,
        exe_path, exe_path);
    CHECK(command_succeeded("image layout harness", command));
    free(assembly);
    unlink(exe_path); unlink(source_path); unlink(object_path); unlink(asm_path);
    st_image_emit_result_destroy(&result);
    anvil_ctx_destroy(context);
#else
    (void)fixture; (void)methods; (void)method_count;
    (void)globals; (void)global_count;
#endif
}

static void test_image_runtime_artifact_validation(
    const image_fixture_t *fixture, const artifact_fixture_t *base)
{
    static const unsigned char payload[] = { 'a', 0u, 'b', 0xffu };
    st_image_aot_method_artifact_t *methods = malloc(
        base->count * sizeof(*methods));
    st_image_string_literal_artifact_t literal;
    st_image_global_artifact_t globals[2] = {
        { UINT32_C(0xf0000001), 1u, "Transcript", 10u },
        { UINT32_C(0xf0000002), 0u, "Arguments", 9u }
    };
    st_image_emit_result_t result;
    CHECK(methods != NULL && base->count != 0u);
    if (!methods || base->count == 0u) { free(methods); return; }
    memcpy(methods, base->items, base->count * sizeof(*methods));
    literal = (st_image_string_literal_artifact_t){
        0u, methods[0].method_id, payload, sizeof(payload)
    };
    methods[0].string_literals = &literal;
    methods[0].string_literal_count = 1u;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result) == ST_IMAGE_EMIT_OK);
    test_native_runtime_metadata(fixture, methods, base->count, globals, 2u);

    globals[0].semantic_external_id = 0u;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT);
    globals[0].semantic_external_id = UINT32_C(0xf0000001);
    globals[1].semantic_external_id = globals[0].semantic_external_id;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT);
    globals[1].semantic_external_id = UINT32_C(0xf0000002);
    globals[1].runtime_index = 1u;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT);
    globals[1].runtime_index = 0u;
    globals[1].name = "Transcript";
    globals[1].name_length = 10u;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT);
    globals[1].name = "Arguments";
    globals[1].name_length = 9u;

    literal.literal_id = 1u;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT);
    literal.literal_id = 0u;
    literal.method_id++;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT);
    literal.method_id = methods[0].method_id;
    literal.bytes = NULL;
    CHECK(emit_runtime_artifacts_once(fixture, methods, base->count,
              globals, 2u, &result)
          == ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT);
    free(methods);
}

static void test_fault_injection(const image_fixture_t *fixture,
                                 const artifact_fixture_t *artifacts)
{
    static const unsigned char literal_bytes[] = { 0u, 1u, 0xffu };
    st_image_aot_method_artifact_t *methods = malloc(
        artifacts->count * sizeof(*methods));
    st_image_string_literal_artifact_t literal;
    st_image_global_artifact_t globals[2] = {
        { UINT32_C(0xf0000001), 1u, "Transcript", 10u },
        { UINT32_C(0xf0000002), 0u, "Arguments", 9u }
    };
    size_t fail_after;
    bool observed_success = false;
    CHECK(methods != NULL && artifacts->count != 0u);
    if (!methods || artifacts->count == 0u) { free(methods); return; }
    memcpy(methods, artifacts->items, artifacts->count * sizeof(*methods));
    literal = (st_image_string_literal_artifact_t){
        0u, methods[0].method_id, literal_bytes, sizeof(literal_bytes)
    };
    methods[0].string_literals = &literal;
    methods[0].string_literal_count = 1u;
    for (fail_after = 0u; fail_after < 2048u && !observed_success;
         fail_after++) {
        anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        failing_allocator_t state;
        st_image_emit_options_t options;
        st_image_emit_result_t result;
        st_image_emit_status_t status;
        memset(&state, 0, sizeof(state));
        state.fail_after = fail_after;
        memset(&options, 0, sizeof(options));
        options.selectors = &fixture->selectors;
        options.allocator.allocate = failing_allocate;
        options.allocator.deallocate = failing_deallocate;
        options.allocator.user = &state;
        options.method_artifacts = methods;
        options.method_artifact_count = artifacts->count;
        options.globals = globals;
        options.global_count = 2u;
        options.require_method_code = true;
        st_image_emit_result_init(&result);
        CHECK(context != NULL);
        status = st_image_emit_metadata(&result, context, &fixture->bundle,
                                        &fixture->graph, &options);
        if (status == ST_IMAGE_EMIT_OK) {
            observed_success = true;
            CHECK(result.module != NULL);
        } else {
            CHECK(status == ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY);
            CHECK(result.module == NULL && result.entity_count == 0u
                  && result.method_count == 0u);
        }
        st_image_emit_result_destroy(&result);
        CHECK(state.live == 0u);
        anvil_ctx_destroy(context);
    }
    CHECK(observed_success);
    free(methods);
}

static void test_anvil_fault_transaction(const image_fixture_t *fixture,
                                         const artifact_fixture_t *artifacts)
{
    static const size_t fail_points[] = { 0u, 32u, 128u };
    size_t index;
    for (index = 0u; index < sizeof(fail_points) / sizeof(fail_points[0]);
         index++) {
        anvil_ctx_t *context = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        st_image_emit_options_t options;
        st_image_emit_result_t result;
        memset(&options, 0, sizeof(options));
        options.selectors = &fixture->selectors;
        options.method_artifacts = artifacts->items;
        options.method_artifact_count = artifacts->count;
        options.require_method_code = true;
        CHECK(context != NULL);
        if (context == NULL) continue;
        st_image_emit_result_init(&result);
        anvil_test_fail_alloc_after(context, fail_points[index]);
        CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                  &fixture->graph, &options) == ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY);
        CHECK(result.module == NULL && result.source_count == 0u
              && result.entity_count == 0u && result.method_count == 0u);
        st_image_emit_result_destroy(&result);
        anvil_test_disable_alloc_fail(context);
        CHECK(st_image_emit_metadata(&result, context, &fixture->bundle,
                                     &fixture->graph, &options)
              == ST_IMAGE_EMIT_OK);
        CHECK(result.module != NULL);
        st_image_emit_result_destroy(&result);
        anvil_ctx_destroy(context);
    }
}

int main(void)
{
    image_fixture_t fixture;
    artifact_fixture_t artifacts;
    CHECK(fixture_init(&fixture));
    if (!st_class_graph_succeeded(&fixture.graph)) {
        fixture_destroy(&fixture);
        return EXIT_FAILURE;
    }
    CHECK(artifact_fixture_init(&artifacts, &fixture));
    if (artifacts.items == NULL) {
        fixture_destroy(&fixture);
        return EXIT_FAILURE;
    }
    test_target(&fixture, ANVIL_ARCH_X86_64);
    test_target(&fixture, ANVIL_ARCH_X86);
    test_target(&fixture, ANVIL_ARCH_ARM64);
    test_target(&fixture, ANVIL_ARCH_PPC32);
    test_target(&fixture, ANVIL_ARCH_PPC64);
    test_target(&fixture, ANVIL_ARCH_PPC64LE);
    test_target(&fixture, ANVIL_ARCH_S370);
    test_target(&fixture, ANVIL_ARCH_S370_XA);
    test_target(&fixture, ANVIL_ARCH_S390);
    test_target(&fixture, ANVIL_ARCH_ZARCH);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_X86_64);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_X86);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_ARM64);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_PPC32);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_PPC64);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_PPC64LE);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_S370);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_S370_XA);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_S390);
    test_aot_target(&fixture, &artifacts, ANVIL_ARCH_ZARCH);
    test_explicit_unavailable_code(&fixture);
    test_artifact_validation(&fixture, &artifacts);
    test_image_runtime_artifact_validation(&fixture, &artifacts);
    test_fault_injection(&fixture, &artifacts);
    test_anvil_fault_transaction(&fixture, &artifacts);
    artifact_fixture_destroy(&artifacts);
    fixture_destroy(&fixture);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk image emitter: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("smalltalk image emitter: PASS");
    return EXIT_SUCCESS;
}
