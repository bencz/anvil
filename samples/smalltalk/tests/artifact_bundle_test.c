#define _POSIX_C_SOURCE 200809L

#include "st_artifact_bundle.h"
#include "st_core_primitives.h"

#include <anvil/anvil_mainframe_mir.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                        \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct allocation_header {
    size_t size;
} allocation_header_t;

typedef struct {
    size_t fail_after;
    size_t successes;
    size_t live;
} failing_allocator_t;

typedef struct {
    char directory[160];
    char image_file[192];
    char manifest_file[192];
    char application_file[192];
    st_source_bundle_t sources;
    const st_ast_unit_t **units;
    st_class_graph_result_t graph;
    st_selector_table_t selectors;
    st_primitive_catalog_t catalog;
    st_primitive_result_t primitives;
} fixture_t;

static void *failing_allocate(void *user, size_t size)
{
    failing_allocator_t *allocator = user;
    allocation_header_t *header;
    if (allocator->successes == allocator->fail_after
            || size > SIZE_MAX - sizeof(*header))
        return NULL;
    header = malloc(sizeof(*header) + size);
    if (header == NULL) return NULL;
    header->size = size;
    allocator->successes++;
    allocator->live++;
    return header + 1;
}

static void failing_deallocate(void *user, void *pointer)
{
    failing_allocator_t *allocator = user;
    allocation_header_t *header;
    if (pointer == NULL) return;
    header = (allocation_header_t *)pointer - 1;
    (void)header->size;
    CHECK(allocator->live != 0u);
    allocator->live--;
    free(header);
}

static bool write_file(const char *path, const char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (ok && length != 0u) ok = fwrite(bytes, 1u, length, file) == length;
    if (file != NULL && fclose(file) != 0) ok = false;
    return ok;
}

static bool register_core_catalog(st_primitive_catalog_t *catalog)
{
    const st_primitive_spec_t *specifications;
    size_t count, index;
    if (!st_primitive_catalog_init(catalog,
                                   (st_primitive_allocator_t){0}))
        return false;
    specifications = st_core_primitive_specs(&count);
    for (index = 0u; index < count; index++)
        if (st_primitive_catalog_register(catalog, &specifications[index],
                                          NULL) != ST_PRIMITIVE_OK)
            return false;
    return true;
}

static bool fixture_init(fixture_t *fixture)
{
    static const char image_source[] =
        "Object := nil [\n"
        "  yourself [ ^self ]\n"
        "  = other [ <primitive: IdentityPrimitive> ]\n"
        "]\n"
        "Class := Object [ <classObjectLayout: true> ]\n";
    static const char application_source[] =
        "BundleApplication := Object [\n"
        "  run [ true ifTrue: [ ^42 ]. ^7 ]\n"
        "  generalSend [ ^self yourself ]\n"
        "  makeBlock: x [ ^[ x ] ]\n"
        "]\n";
    const char *applications[1];
    st_selector_id_t yourself_id = ST_SELECTOR_INVALID_ID;
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->directory, sizeof(fixture->directory),
             "/tmp/anvil-st-artifacts-%ld", (long)getpid());
    snprintf(fixture->image_file, sizeof(fixture->image_file),
             "%s/Object.st", fixture->directory);
    snprintf(fixture->manifest_file, sizeof(fixture->manifest_file),
             "%s/manifest.txt", fixture->directory);
    snprintf(fixture->application_file, sizeof(fixture->application_file),
             "/tmp/anvil-st-artifacts-app-%ld.st", (long)getpid());
    (void)unlink(fixture->image_file);
    (void)unlink(fixture->manifest_file);
    (void)unlink(fixture->application_file);
    (void)rmdir(fixture->directory);
    if (mkdir(fixture->directory, 0700) != 0
            || !write_file(fixture->manifest_file, "Object.st\n", 10u)
            || !write_file(fixture->image_file, image_source,
                           sizeof(image_source) - 1u)
            || !write_file(fixture->application_file, application_source,
                           sizeof(application_source) - 1u))
        return false;
    applications[0] = fixture->application_file;
    if (st_source_bundle_load(&fixture->sources, fixture->directory,
            applications, 1u, NULL) != ST_SOURCE_LOAD_OK)
        return false;
    fixture->units = malloc(fixture->sources.count * sizeof(*fixture->units));
    if (fixture->units == NULL) return false;
    for (index = 0u; index < fixture->sources.count; index++)
        fixture->units[index] = &fixture->sources.files[index].ast;
    st_class_graph_result_init(&fixture->graph);
    if (st_class_graph_build(&fixture->graph, fixture->units,
            fixture->sources.count, NULL) != ST_CLASS_GRAPH_OK
            || !st_class_graph_succeeded(&fixture->graph))
        return false;
    if (st_selector_table_build_for_units(
            &fixture->selectors, fixture->units, fixture->sources.count,
            (st_selector_allocator_t){0},
            UINT64_C(0x4152544946414354)) != ST_SELECTOR_OK
            || !st_selector_lookup(&fixture->selectors, "yourself", 8u,
                                   &yourself_id)
            || yourself_id == ST_SELECTOR_INVALID_ID
            || !register_core_catalog(&fixture->catalog))
        return false;
    st_primitive_result_init(&fixture->primitives);
    return st_primitive_resolve(&fixture->primitives, fixture->units,
               fixture->sources.count, &fixture->catalog, NULL)
               == ST_PRIMITIVE_OK
        && st_primitive_result_succeeded(&fixture->primitives)
        && fixture->graph.method_count == 5u;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_primitive_result_destroy(&fixture->primitives);
    st_primitive_catalog_destroy(&fixture->catalog);
    st_selector_table_destroy(&fixture->selectors);
    st_class_graph_result_destroy(&fixture->graph);
    free(fixture->units);
    st_source_bundle_destroy(&fixture->sources);
    (void)unlink(fixture->image_file);
    (void)unlink(fixture->manifest_file);
    (void)unlink(fixture->application_file);
    (void)rmdir(fixture->directory);
    memset(fixture, 0, sizeof(*fixture));
}

static st_aot_compile_options_t compile_options(
    const fixture_t *fixture, anvil_arch_t target)
{
    st_aot_compile_options_t options;
    memset(&options, 0, sizeof(options));
    options.bundle = &fixture->sources;
    options.graph = &fixture->graph;
    options.selectors = &fixture->selectors;
    options.primitives = &fixture->primitives;
    options.target = target;
    options.abi = ANVIL_ABI_DEFAULT;
    options.syntax = ANVIL_SYNTAX_DEFAULT;
    options.optimization = ANVIL_OPT_STANDARD;
    options.symbol_prefix = "bundle";
    options.symbol_prefix_length = 6u;
    return options;
}

static st_artifact_bundle_options_t bundle_options(void)
{
    st_artifact_bundle_options_t options;
    memset(&options, 0, sizeof(options));
    return options;
}

static bool bundles_equal(const st_artifact_bundle_t *left,
                          const st_artifact_bundle_t *right)
{
    size_t index;
    if (left->target != right->target || left->abi != right->abi
            || left->syntax != right->syntax
            || left->optimization != right->optimization
            || left->artifact_count != right->artifact_count
            || left->block_count != right->block_count
            || left->manifest_length != right->manifest_length
            || memcmp(left->bundle_sha256, right->bundle_sha256,
                      ST_ARTIFACT_SHA256_SIZE) != 0
            || memcmp(left->manifest, right->manifest,
                      left->manifest_length) != 0)
        return false;
    for (index = 0u; index < left->artifact_count; index++) {
        const st_artifact_blob_t *a = &left->artifacts[index];
        const st_artifact_blob_t *b = &right->artifacts[index];
        if (a->kind != b->kind || a->method_id != b->method_id
                || a->name_length != b->name_length
                || a->symbol_length != b->symbol_length || a->size != b->size
                || strcmp(a->name, b->name) != 0
                || strcmp(a->symbol, b->symbol) != 0
                || memcmp(a->sha256, b->sha256,
                          ST_ARTIFACT_SHA256_SIZE) != 0
                || memcmp(a->bytes, b->bytes, a->size) != 0)
            return false;
    }
    return true;
}

static bool hexadecimal_hash(const char *bytes)
{
    size_t index;
    for (index = 0u; index < 64u; index++)
        if (!((bytes[index] >= '0' && bytes[index] <= '9')
                || (bytes[index] >= 'a' && bytes[index] <= 'f')))
            return false;
    return true;
}

static void test_sha256_known_answer(void)
{
    static const uint8_t expected[ST_ARTIFACT_SHA256_SIZE] = {
        UINT8_C(0xba), UINT8_C(0x78), UINT8_C(0x16), UINT8_C(0xbf),
        UINT8_C(0x8f), UINT8_C(0x01), UINT8_C(0xcf), UINT8_C(0xea),
        UINT8_C(0x41), UINT8_C(0x41), UINT8_C(0x40), UINT8_C(0xde),
        UINT8_C(0x5d), UINT8_C(0xae), UINT8_C(0x22), UINT8_C(0x23),
        UINT8_C(0xb0), UINT8_C(0x03), UINT8_C(0x61), UINT8_C(0xa3),
        UINT8_C(0x96), UINT8_C(0x17), UINT8_C(0x7a), UINT8_C(0x9c),
        UINT8_C(0xb4), UINT8_C(0x10), UINT8_C(0xff), UINT8_C(0x61),
        UINT8_C(0xf2), UINT8_C(0x00), UINT8_C(0x15), UINT8_C(0xad)
    };
    uint8_t actual[ST_ARTIFACT_SHA256_SIZE];
    CHECK(st_artifact_sha256("abc", 3u, actual));
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
    CHECK(!st_artifact_sha256(NULL, 1u, actual));
    CHECK(!st_artifact_sha256("abc", 3u, NULL));
    if (SIZE_MAX > UINT64_MAX / UINT64_C(8))
        CHECK(!st_artifact_sha256((const void *)(uintptr_t)1u,
                                  (size_t)(UINT64_MAX / UINT64_C(8) + 1u),
                                  actual));
}

static void test_determinism_and_layout(const fixture_t *fixture)
{
    st_aot_compile_options_t options = compile_options(
        fixture, ANVIL_ARCH_X86_64);
    st_artifact_bundle_options_t render_options = bundle_options();
    st_aot_compile_result_t compiled;
    st_artifact_bundle_t first, second;
    size_t index;
    st_aot_compile_result_init(&compiled);
    st_artifact_bundle_init(&first);
    st_artifact_bundle_init(&second);
    CHECK(st_aot_compile(&compiled, &options) == ST_AOT_COMPILE_OK);
    CHECK(st_artifact_bundle_render(&first, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_OK);
    CHECK(st_artifact_bundle_render(&second, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_OK);
    CHECK(bundles_equal(&first, &second));
    CHECK(first.target == ANVIL_ARCH_X86_64);
    CHECK(first.optimization == ANVIL_OPT_STANDARD);
    CHECK(first.artifact_count == compiled.method_count + 1u);
    CHECK(first.block_count == 1u);
    CHECK(first.manifest != NULL
          && first.manifest[first.manifest_length] == '\0');
    CHECK(strstr(first.manifest,
          "anvil-smalltalk-artifact-bundle-v2\ntarget=x86_64\n")
          == first.manifest);
    CHECK(strstr(first.manifest, "optimization=O2\n") != NULL);
    CHECK(strstr(first.manifest, "metadata-abi=5\n") != NULL);
    CHECK(strstr(first.manifest, "launch=absent\n") != NULL);
    CHECK(strstr(first.manifest, "block-count=1\n") != NULL);
    CHECK(strstr(first.manifest, "block=") != NULL);
    {
        const char *hash = strstr(first.manifest, "bundle-sha256=");
        CHECK(hash != NULL && hexadecimal_hash(hash + 14u)
              && hash[14u + 64u] == '\n');
    }
    for (index = 0u; index < compiled.method_count; index++) {
        const st_artifact_blob_t *artifact = &first.artifacts[index];
        CHECK(artifact->kind == ST_ARTIFACT_METHOD_ASSEMBLY);
        CHECK(artifact->method_id == index + 1u);
        CHECK(strchr(artifact->name, '/') == NULL);
        CHECK(artifact->name_length > artifact->symbol_length);
        CHECK(strcmp(artifact->symbol, compiled.methods[index].symbol) == 0);
        CHECK(artifact->bytes != NULL && artifact->size != 0u);
    }
    CHECK(first.artifacts[compiled.method_count].kind
          == ST_ARTIFACT_METADATA_ASSEMBLY);
    CHECK(first.artifacts[compiled.method_count].method_id
          == ST_CLASS_GRAPH_INVALID_ID);
    CHECK(strcmp(first.artifacts[compiled.method_count].name, "metadata.s")
          == 0);
    CHECK(strcmp(first.artifacts[compiled.method_count].symbol,
                 "bundle_descriptor") == 0);
    st_artifact_bundle_destroy(&second);
    st_artifact_bundle_destroy(&first);
    st_aot_compile_result_destroy(&compiled);
}

static void test_supported_target_matrix(const fixture_t *fixture)
{
    static const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64,
        ANVIL_ARCH_PPC64LE, ANVIL_ARCH_ZARCH
    };
    size_t index;
    for (index = 0u; index < sizeof(targets) / sizeof(targets[0]); index++) {
        st_aot_compile_options_t options = compile_options(fixture,
                                                           targets[index]);
        st_artifact_bundle_options_t render_options = bundle_options();
        st_aot_compile_result_t compiled;
        st_artifact_bundle_t bundle;
        st_artifact_bundle_status_t status;
        st_aot_compile_result_init(&compiled);
        st_artifact_bundle_init(&bundle);
        CHECK(st_aot_compile(&compiled, &options) == ST_AOT_COMPILE_OK);
        status = st_artifact_bundle_render(&bundle, &compiled,
                                           &render_options);
        if (status != ST_ARTIFACT_BUNDLE_OK) {
            fprintf(stderr, "artifact target %d: %s (anvil=%d method=%" PRIu32
                    " symbol=%s)\n",
                    (int)targets[index],
                    st_artifact_bundle_status_string(status),
                    (int)bundle.codegen_error, bundle.failed_method_id,
                    bundle.failed_method_id != ST_CLASS_GRAPH_INVALID_ID
                            && (size_t)bundle.failed_method_id
                                <= compiled.method_count
                        ? compiled.methods[bundle.failed_method_id - 1u].symbol
                        : "<metadata>");
            if (targets[index] == ANVIL_ARCH_ZARCH
                    && bundle.failed_method_id != ST_CLASS_GRAPH_INVALID_ID
                    && (size_t)bundle.failed_method_id <= compiled.method_count) {
                anvil_mir_func_t *mir;
                char legal_error[256] = {0};
                char *assembly = NULL;
                size_t assembly_length = 0u;
                anvil_func_t *function = compiled.methods[
                    bundle.failed_method_id - 1u].lowering.function;
                anvil_ctx_clear_error(compiled.context);
                mir = anvil_mainframe_lower_func_to_mir(
                    function, ANVIL_MAINFRAME_VARIANT_ZARCH);
                if (mir == NULL) {
                    fprintf(stderr, "zarch repro MIR lowering: %s\n",
                            anvil_ctx_get_error(compiled.context));
                } else if (!anvil_mainframe_verify_mir_legal(
                               mir, ANVIL_MAINFRAME_VARIANT_ZARCH,
                               legal_error, sizeof(legal_error))) {
                    fprintf(stderr, "zarch repro MIR legalization: %s\n",
                            legal_error);
                } else if (!anvil_mainframe_regalloc_mir(
                               mir, ANVIL_MAINFRAME_VARIANT_ZARCH)) {
                    fprintf(stderr, "zarch repro MIR register allocation failed\n");
                } else if (!anvil_mainframe_emit_mir(
                               mir, ANVIL_MAINFRAME_VARIANT_ZARCH,
                               &assembly, &assembly_length)) {
                    fprintf(stderr, "zarch repro HLASM emission failed\n");
                }
                free(assembly);
                anvil_mir_func_destroy(mir);
            }
        }
        CHECK(status == ST_ARTIFACT_BUNDLE_OK);
        CHECK(bundle.target == targets[index]);
        CHECK(bundle.artifact_count == compiled.method_count + 1u);
        CHECK(bundle.block_count == 1u);
        CHECK(strcmp(bundle.artifacts[compiled.method_count].name,
                     targets[index] == ANVIL_ARCH_ZARCH
                        ? "metadata.asm" : "metadata.s") == 0);
        CHECK(strstr(bundle.manifest,
                     targets[index] == ANVIL_ARCH_ZARCH
                        ? "syntax=hlasm\n" : "syntax=gas\n") != NULL);
        st_artifact_bundle_destroy(&bundle);
        st_aot_compile_result_destroy(&compiled);
    }
}

static void test_validation_and_collision(const fixture_t *fixture)
{
    st_aot_compile_options_t options = compile_options(
        fixture, ANVIL_ARCH_X86_64);
    st_artifact_bundle_options_t render_options = bundle_options();
    st_aot_compile_result_t compiled;
    st_artifact_bundle_t bundle;
    char *saved_symbol;
    char *saved_prefix;
    size_t saved_length;
    st_aot_compile_result_init(&compiled);
    CHECK(st_aot_compile(&compiled, &options) == ST_AOT_COMPILE_OK);
    CHECK(compiled.method_count >= 2u);
    saved_prefix = compiled.provenance.symbol_prefix;
    saved_symbol = compiled.methods[1].symbol;
    saved_length = compiled.methods[1].symbol_length;
    compiled.methods[1].symbol = compiled.methods[0].symbol;
    compiled.methods[1].symbol_length = compiled.methods[0].symbol_length;
    compiled.methods[1].artifact.symbol = compiled.methods[0].symbol;
    compiled.methods[1].artifact.symbol_length =
        compiled.methods[0].symbol_length;
    st_artifact_bundle_init(&bundle);
    CHECK(st_artifact_bundle_render(&bundle, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_ERR_COLLISION);
    CHECK(bundle.artifacts == NULL && bundle.manifest == NULL
          && bundle.implementation == NULL);
    st_artifact_bundle_destroy(&bundle);
    compiled.methods[1].symbol = saved_symbol;
    compiled.methods[1].symbol_length = saved_length;
    compiled.methods[1].artifact.symbol = saved_symbol;
    compiled.methods[1].artifact.symbol_length = saved_length;

    compiled.methods[0].artifact.arity++;
    st_artifact_bundle_init(&bundle);
    CHECK(st_artifact_bundle_render(&bundle, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT);
    st_artifact_bundle_destroy(&bundle);
    compiled.methods[0].artifact.arity--;

    compiled.methods[0].lowering.status = ST_LOWER_ERR_VERIFY;
    st_artifact_bundle_init(&bundle);
    CHECK(st_artifact_bundle_render(&bundle, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT);
    st_artifact_bundle_destroy(&bundle);
    compiled.methods[0].lowering.status = ST_LOWER_OK;

    {
        st_aot_method_result_t *rooted = NULL;
        uint32_t saved_safepoint = 0u;
        size_t method_index;
        for (method_index = 0u; method_index < compiled.method_count;
             method_index++)
            if (compiled.methods[method_index].lowering.root_map_count != 0u) {
                rooted = &compiled.methods[method_index];
                break;
            }
        CHECK(rooted != NULL);
        if (rooted != NULL) {
            saved_safepoint = rooted->root_maps[0].safepoint_id;
            rooted->root_maps[0].safepoint_id ^= UINT32_C(0x80000000);
            st_artifact_bundle_init(&bundle);
            CHECK(st_artifact_bundle_render(&bundle, &compiled,
                                            &render_options)
                  == ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT);
            st_artifact_bundle_destroy(&bundle);
            rooted->root_maps[0].safepoint_id = saved_safepoint;
        }
    }

    {
        st_aot_method_result_t *blocked = NULL;
        size_t method_index;
        for (method_index = 0u; method_index < compiled.method_count;
             method_index++)
            if (compiled.methods[method_index].lowering.block_count != 0u) {
                blocked = &compiled.methods[method_index];
                break;
            }
        CHECK(blocked != NULL);
        if (blocked != NULL) {
            st_image_aot_block_artifact_t *adapter =
                &blocked->block_artifacts[0];
            const st_lower_block_artifact_t *canonical =
                &blocked->lowering.blocks[0];
            if (adapter->capture_count != 0u) {
                uint32_t saved_binding =
                    blocked->block_captures[0].binding_id;
                blocked->block_captures[0].binding_id ^= UINT32_C(1);
                st_artifact_bundle_init(&bundle);
                CHECK(st_artifact_bundle_render(&bundle, &compiled,
                                                &render_options)
                      == ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT);
                st_artifact_bundle_destroy(&bundle);
                blocked->block_captures[0].binding_id = saved_binding;
            }
            {
                const char *saved_adapter = adapter->descriptor_symbol;
                size_t saved_adapter_length =
                    adapter->descriptor_symbol_length;
                st_lower_symbol_t *mutable_symbol =
                    (st_lower_symbol_t *)&canonical->descriptor_symbol;
                st_lower_symbol_t saved_canonical = *mutable_symbol;
                adapter->descriptor_symbol = compiled.methods[0].symbol;
                adapter->descriptor_symbol_length =
                    compiled.methods[0].symbol_length;
                mutable_symbol->bytes = compiled.methods[0].symbol;
                mutable_symbol->length = compiled.methods[0].symbol_length;
                st_artifact_bundle_init(&bundle);
                CHECK(st_artifact_bundle_render(&bundle, &compiled,
                                                &render_options)
                      == ST_ARTIFACT_BUNDLE_ERR_COLLISION);
                st_artifact_bundle_destroy(&bundle);
                *mutable_symbol = saved_canonical;
                adapter->descriptor_symbol = saved_adapter;
                adapter->descriptor_symbol_length = saved_adapter_length;
            }
        }
    }

    compiled.provenance.symbol_prefix = "wrong";
    compiled.provenance.symbol_prefix_length = 5u;
    st_artifact_bundle_init(&bundle);
    CHECK(st_artifact_bundle_render(&bundle, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT);
    st_artifact_bundle_destroy(&bundle);
    compiled.provenance.symbol_prefix = saved_prefix;
    compiled.provenance.symbol_prefix_length = options.symbol_prefix_length;

    render_options = bundle_options();
    compiled.provenance.syntax = ANVIL_SYNTAX_HLASM;
    st_artifact_bundle_init(&bundle);
    CHECK(st_artifact_bundle_render(&bundle, &compiled, &render_options)
          == ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET);
    st_artifact_bundle_destroy(&bundle);
    compiled.provenance.syntax = options.syntax;

    {
        st_aot_compile_result_t fake = compiled;
        anvil_ctx_t *narrow = anvil_ctx_create_for_target(ANVIL_ARCH_X86);
        CHECK(narrow != NULL);
        fake.context = narrow;
        fake.provenance.target = ANVIL_ARCH_X86;
        render_options = bundle_options();
        st_artifact_bundle_init(&bundle);
        CHECK(st_artifact_bundle_render(&bundle, &fake, &render_options)
              == ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET);
        CHECK(bundle.artifacts == NULL && bundle.manifest == NULL);
        st_artifact_bundle_destroy(&bundle);
        anvil_ctx_destroy(narrow);
    }

    st_aot_compile_result_destroy(&compiled);
}

static void test_transactional_oom(const fixture_t *fixture)
{
    st_aot_compile_options_t options = compile_options(
        fixture, ANVIL_ARCH_X86_64);
    st_aot_compile_result_t compiled;
    size_t fail_after;
    bool reached_success = false;
    st_aot_compile_result_init(&compiled);
    CHECK(st_aot_compile(&compiled, &options) == ST_AOT_COMPILE_OK);
    for (fail_after = 0u; fail_after < 128u; fail_after++) {
        failing_allocator_t allocator = { fail_after, 0u, 0u };
        st_artifact_bundle_options_t render_options = bundle_options();
        st_artifact_bundle_t bundle;
        st_artifact_bundle_status_t status;
        render_options.allocator = (st_artifact_allocator_t){
            failing_allocate, failing_deallocate, &allocator
        };
        st_artifact_bundle_init(&bundle);
        status = st_artifact_bundle_render(&bundle, &compiled,
                                           &render_options);
        if (status == ST_ARTIFACT_BUNDLE_OK) {
            CHECK(bundle.artifacts != NULL && bundle.manifest != NULL);
            st_artifact_bundle_destroy(&bundle);
            CHECK(allocator.live == 0u);
            reached_success = true;
            break;
        }
        CHECK(status == ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY);
        CHECK(bundle.artifacts == NULL && bundle.artifact_count == 0u
              && bundle.manifest == NULL && bundle.implementation == NULL);
        CHECK(allocator.live == 0u);
        st_artifact_bundle_destroy(&bundle);
    }
    CHECK(reached_success);
    st_aot_compile_result_destroy(&compiled);
}

int main(void)
{
    fixture_t fixture;
    test_sha256_known_answer();
    CHECK(fixture_init(&fixture));
    if (failures == 0u) {
        test_determinism_and_layout(&fixture);
        test_supported_target_matrix(&fixture);
        test_validation_and_collision(&fixture);
        test_transactional_oom(&fixture);
    }
    fixture_destroy(&fixture);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk artifact bundle: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk artifact bundle: PASS (five tagged64 targets, deterministic SHA-256 manifest, transactional OOM)");
    return 0;
}
