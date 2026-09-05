#define _POSIX_C_SOURCE 200809L

#include "st_aot_compile.h"
#include "st_block_primitives.h"
#include "st_core_primitives.h"
#include "st_exception_primitives.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_integer_primitives.h"
#include "st_reflection_primitives.h"
#include "st_stream_primitives.h"
#include "st_string_primitives.h"

#include <anvil/anvil.h>

#include <errno.h>
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
    char manifest[192];
    char application[192];
    st_source_bundle_t bundle;
    const st_ast_unit_t **units;
    st_class_graph_result_t graph;
    st_selector_table_t selectors;
    st_primitive_catalog_t catalog;
    st_primitive_result_t primitives;
} fixture_t;

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

static bool write_file(const char *path, const char *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (ok && length != 0u) ok = fwrite(bytes, 1u, length, file) == length;
    if (file != NULL && fclose(file) != 0) ok = false;
    return ok;
}

static void test_native_aot_image_runtime(const st_aot_compile_result_t *result)
{
#if defined(__x86_64__) && !defined(_WIN32)
    static const char harness[] =
        "#include \"st_image_emit.h\"\n#include \"st_image_runtime.h\"\n"
        "#include \"st_lookup.h\"\n#include \"st_send_bridge.h\"\n"
        "#include <stdlib.h>\n#include <string.h>\n"
        "extern const st_image_metadata_descriptor_t mini_descriptor;\n"
        "enum{O=1,S=2,E=3,N=4,F=5,T=6,I=7,C=8,M=9,Z=9};\n"
        "static const st_image_method_metadata_t*fm(const char*n){for(size_t i=0;i<mini_descriptor.method_count;i++)"
        "if(strcmp(mini_descriptor.methods[i].selector,n)==0)return &mini_descriptor.methods[i];return 0;}\n"
        "static st_value_t call(const st_image_method_metadata_t*m,st_aot_thread_t*t){st_value_t r[64];"
        "if(!m||!m->code||!m->runtime_descriptor||m->frame_root_capacity>64)exit(90);"
        "for(size_t i=0;i<64;i++)r[i]=st_value_nil();StFrame f={.thread=t,.method=m->runtime_descriptor,"
        ".receiver=st_value_nil(),.roots=m->frame_root_capacity?r:0,.root_count=m->frame_root_capacity};"
        "return m->code(&f);}\n"
        "int main(void){static const char*nm[Z]={\"Object\",\"ByteString\",\"ExternalStream\","
        "\"Nil\",\"False\",\"True\",\"SmallInteger\",\"Character\",\"Metaclass\"};"
        "StClassDescriptor c[Z];StShapeDescriptor s[Z];const StClassDescriptor*cp[Z];"
        "const StShapeDescriptor*sp[Z];uint64_t bm=1;for(uint32_t i=0;i<Z;i++){c[i]=(StClassDescriptor)"
        "{i+1,(i==0||i==8)?0:1,9,i+1,i==8?ST_CLASS_METACLASS:0,nm[i],strlen(nm[i]),0,0};"
        "s[i]=(StShapeDescriptor){i+1,i+1,8,24,0,ST_INDEXED_NONE,0,0};cp[i]=&c[i];sp[i]=&s[i];}"
        "s[S-1].indexed_format=ST_INDEXED_UINT8;s[E-1].minimum_allocation_size=32;s[E-1].fixed_word_count=1;"
        "s[E-1].fixed_pointer_bitmap=&bm;s[E-1].fixed_pointer_bitmap_word_count=1;"
        "st_runtime_descriptors_t d={cp,Z,sp,Z};if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;"
        "st_heap_t h={0};if(st_heap_init(&h,&d,(st_runtime_allocator_t){0})!=ST_HEAP_OK)return 2;"
        "st_image_runtime_entry_t*g=calloc(mini_descriptor.global_count,sizeof(*g));"
        "st_image_runtime_entry_t*l=calloc(mini_descriptor.string_literal_count,sizeof(*l));if(!g||!l)return 3;"
        "for(size_t i=0;i<mini_descriptor.global_count;i++)g[i]=(st_image_runtime_entry_t){i+1,ST_VALUE_INVALID};"
        "for(size_t i=0;i<mini_descriptor.string_literal_count;i++)l[i]=(st_image_runtime_entry_t){i+1,ST_VALUE_INVALID};"
        "st_image_runtime_options_t o={.descriptors=&d,.borrowed_heap=&h,.globals=g,.global_count=mini_descriptor.global_count,"
        ".literals=l,.literal_count=mini_descriptor.string_literal_count,.string_layout={S,S},"
        ".external_stream_layout={E,E,0}};st_image_runtime_t image={0};"
        "if(st_image_runtime_init(&image,&o)!=ST_IMAGE_RUNTIME_OK)return 4;st_value_t transcript=ST_VALUE_INVALID;"
        "size_t ti=mini_descriptor.global_count;for(size_t i=0;i<mini_descriptor.global_count;i++)"
        "if(mini_descriptor.globals[i].semantic_external_id==0xf0000001u)ti=mini_descriptor.globals[i].runtime_index;"
        "if(ti>=mini_descriptor.global_count||st_image_runtime_bootstrap_transcript(&image,ti,&transcript)"
        "!=ST_IMAGE_RUNTIME_OK)return 5;for(size_t i=0;i<mini_descriptor.string_literal_count;i++){"
        "const st_image_string_literal_metadata_t*x=&mini_descriptor.string_literals[i];st_value_t v;"
        "if(x->literal_id!=i||st_image_runtime_bootstrap_string_literal(&image,x->literal_id,x->bytes,x->length,&v)"
        "!=ST_IMAGE_RUNTIME_OK)return 6;}st_lookup_context_t lookup={0};if(st_lookup_context_init(&lookup,&d,"
        "(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 7;uint32_t ids[5]={N,F,T,I,C};st_aot_thread_t t={0};"
        "if(!st_aot_thread_init(&t,&lookup,ids,0,0,0,0,0,0,0)||!st_aot_thread_image_attach(&t,&image))return 8;"
        "st_value_t a=call(fm(\"first\"),&t),r=call(fm(\"run\"),&t),z=call(fm(\"last\"),&t);"
        "st_object_view_t av,zv;if(r!=transcript||st_heap_object_view(&h,a,&av)!=ST_HEAP_OK||"
        "st_heap_object_view(&h,z,&zv)!=ST_HEAP_OK||av.indexed_length!=5||zv.indexed_length!=5||"
        "memcmp(av.indexed_elements,\"alpha\",5)||memcmp(zv.indexed_elements,\"omega\",5))return 9;"
        "if(!st_aot_thread_image_detach(&t,&image))return 10;st_aot_thread_destroy(&t);"
        "st_lookup_context_destroy(&lookup);st_image_runtime_destroy(&image);st_heap_destroy(&h);free(l);free(g);return 0;}\n";
    char stem[160], meta_path[192], source_path[192], exe_path[192];
    char method_paths[3][192], command[8192];
    size_t selected[3], selected_count = 0u, used;
    char *assembly = NULL;
    size_t length = 0u;
    for (size_t index = 0u; index < result->method_count; index++)
        if (strcmp(result->methods[index].selector, "first") == 0
                || strcmp(result->methods[index].selector, "run") == 0
                || strcmp(result->methods[index].selector, "last") == 0)
            selected[selected_count++] = index;
    CHECK(selected_count == 3u);
    if (selected_count != 3u) return;
    snprintf(stem, sizeof(stem), "/tmp/anvil-st-aot-image-%ld",
             (long)getpid());
    snprintf(meta_path, sizeof(meta_path), "%s-meta.s", stem);
    snprintf(source_path, sizeof(source_path), "%s.c", stem);
    snprintf(exe_path, sizeof(exe_path), "%s-exe", stem);
    CHECK(anvil_module_codegen(result->metadata.module, &assembly, &length)
          == ANVIL_OK && write_file(meta_path, assembly, length));
    free(assembly); assembly = NULL;
    used = (size_t)snprintf(command, sizeof(command),
        "/usr/bin/cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include %s ",
        meta_path);
    for (size_t item = 0u; item < selected_count; item++) {
        size_t index = selected[item];
        snprintf(method_paths[item], sizeof(method_paths[item]), "%s-m%zu.s",
                 stem, item);
        CHECK(anvil_module_codegen(result->methods[index].lowering.module,
                                   &assembly, &length) == ANVIL_OK);
        CHECK(assembly && write_file(method_paths[item], assembly, length));
        free(assembly); assembly = NULL;
        used += (size_t)snprintf(command + used, sizeof(command) - used,
                                 "%s ", method_paths[item]);
    }
    {
        FILE *source = fopen(source_path, "wb");
        bool ok = source && fwrite(harness, 1u, sizeof(harness) - 1u, source)
            == sizeof(harness) - 1u;
        for (size_t index = 0u; ok && index < result->method_count; index++) {
            bool real = false;
            for (size_t item = 0u; item < selected_count; item++)
                if (selected[item] == index) real = true;
            if (!real)
                ok = fprintf(source, "st_value_t %s(StFrame*f){(void)f;return 0;}\n",
                             result->methods[index].symbol) > 0;
        }
        if (source && fclose(source) != 0) ok = false;
        CHECK(ok);
    }
    used += (size_t)snprintf(command + used, sizeof(command) - used,
        "%s samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/posix/runtime.c "
        "samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/send_bridge.c "
        "samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c "
        "samples/smalltalk/src/runtime/heap.c samples/smalltalk/src/runtime/image_runtime.c "
        "-o %s -pthread && %s", source_path, exe_path, exe_path);
    CHECK(used < sizeof(command));
    if (used < sizeof(command)) CHECK(system(command) == 0);
    for (size_t item = 0u; item < selected_count; item++) unlink(method_paths[item]);
    unlink(exe_path); unlink(source_path); unlink(meta_path);
#else
    (void)result;
#endif
}

static bool register_core_catalog(st_primitive_catalog_t *catalog)
{
    const st_primitive_spec_t *specs;
    size_t count, index;
    if (!st_primitive_catalog_init(catalog,
                                   (st_primitive_allocator_t){0}))
        return false;
    specs = st_core_primitive_specs(&count);
    for (index = 0u; index < count; index++)
        if (st_primitive_catalog_register(catalog, &specs[index], NULL)
                != ST_PRIMITIVE_OK)
            return false;
    return true;
}

typedef const st_primitive_spec_t *(*spec_provider_t)(size_t *count_out);

static bool register_complete_catalog(st_primitive_catalog_t *catalog)
{
    static const spec_provider_t providers[] = {
        st_core_primitive_specs,
        st_heap_primitive_specs,
        st_float_primitive_specs,
        st_integer_primitive_specs,
        st_stream_primitive_specs,
        st_string_primitive_specs,
        st_block_primitive_specs,
        st_exception_primitive_specs,
        st_reflection_primitive_specs
    };
    if (!st_primitive_catalog_init(
            catalog, (st_primitive_allocator_t) {0})) {
        return false;
    }
    for (size_t provider_index = 0u;
         provider_index < sizeof(providers) / sizeof(providers[0]);
         provider_index++) {
        size_t count = 0u;
        const st_primitive_spec_t *specs = providers[provider_index](&count);
        if (specs == NULL || count == 0u) {
            return false;
        }
        for (size_t spec_index = 0u; spec_index < count; spec_index++) {
            if (st_primitive_catalog_register(
                    catalog, &specs[spec_index], NULL) != ST_PRIMITIVE_OK) {
                return false;
            }
        }
    }
    return true;
}

static bool build_graph_and_primitives(
    fixture_t *fixture, bool complete_catalog)
{
    size_t index;
    fixture->units = malloc(fixture->bundle.count * sizeof(*fixture->units));
    if (fixture->units == NULL) return false;
    for (index = 0u; index < fixture->bundle.count; index++)
        fixture->units[index] = &fixture->bundle.files[index].ast;
    st_class_graph_result_init(&fixture->graph);
    if (st_class_graph_build(&fixture->graph, fixture->units,
            fixture->bundle.count, NULL) != ST_CLASS_GRAPH_OK
            || !st_class_graph_succeeded(&fixture->graph))
        return false;
    if (st_selector_table_build_for_units(
            &fixture->selectors, fixture->units, fixture->bundle.count,
            (st_selector_allocator_t){0},
            UINT64_C(0x414f545445535431)) != ST_SELECTOR_OK)
        return false;
    if (complete_catalog) {
        if (!register_complete_catalog(&fixture->catalog)) {
            return false;
        }
    } else if (!register_core_catalog(&fixture->catalog)) {
        return false;
    }
    st_primitive_result_init(&fixture->primitives);
    return st_primitive_resolve(&fixture->primitives, fixture->units,
               fixture->bundle.count, &fixture->catalog, NULL)
               == ST_PRIMITIVE_OK;
}

static bool fixture_init_application(fixture_t *fixture,
                                     const char *application_source,
                                     size_t expected_methods)
{
    static const char image_source[] =
        "Object := nil [\n"
        "  yourself [ ^self ]\n"
        "  = other [ <primitive: IdentityPrimitive> ]\n"
        "]\n"
        "Class := Object [ <classObjectLayout: true> ]\n";
    size_t application_length = strlen(application_source);
    const char *applications[1];
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->directory, sizeof(fixture->directory),
             "/tmp/anvil-st-aot-%ld", (long)getpid());
    snprintf(fixture->image_file, sizeof(fixture->image_file),
             "%s/Object.st", fixture->directory);
    snprintf(fixture->manifest, sizeof(fixture->manifest),
             "%s/manifest.txt", fixture->directory);
    snprintf(fixture->application, sizeof(fixture->application),
             "/tmp/anvil-st-aot-app-%ld.st", (long)getpid());
    (void)unlink(fixture->image_file);
    (void)unlink(fixture->manifest);
    (void)unlink(fixture->application);
    (void)rmdir(fixture->directory);
    if (mkdir(fixture->directory, 0700) != 0) return false;
    if (!write_file(fixture->manifest, "Object.st\n", 10u)
            || !write_file(fixture->image_file, image_source,
                           sizeof(image_source) - 1u)
            || !write_file(fixture->application, application_source,
                           application_length))
        return false;
    applications[0] = fixture->application;
    if (st_source_bundle_load(&fixture->bundle, fixture->directory,
            applications, 1u, NULL) != ST_SOURCE_LOAD_OK)
        return false;
    return build_graph_and_primitives(fixture, false)
        && st_primitive_result_succeeded(&fixture->primitives)
        && fixture->graph.method_count == expected_methods;
}

static bool fixture_init_mini(fixture_t *fixture, bool with_control)
{
    static const char application_source[] =
        "MiniApplication := Object [ run [ ^true ] ]\n";
    static const char control_application_source[] =
        "MiniApplication := Object [\n"
        "  run [ ^true ]\n"
        "  nonLocalReturn [ true ifTrue: [ ^42 ]. ^7 ]\n"
        "]\n";
    return fixture_init_application(fixture,
        with_control ? control_application_source : application_source,
        with_control ? 4u : 3u);
}

static const char *existing_path(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static bool fixture_init_real(fixture_t *fixture)
{
    const char *image = existing_path(
        "st-image", "samples/smalltalk/st-image");
    const char *application = existing_path(
        "tests/fixtures/HelloApplication.st",
        "samples/smalltalk/tests/fixtures/HelloApplication.st");
    const char *applications[1];
    memset(fixture, 0, sizeof(*fixture));
    if (image == NULL || application == NULL) return false;
    applications[0] = application;
    if (st_source_bundle_load(&fixture->bundle, image, applications, 1u, NULL)
            != ST_SOURCE_LOAD_OK)
        return false;
    return build_graph_and_primitives(fixture, true)
        && st_primitive_result_succeeded(&fixture->primitives)
        && fixture->primitives.diagnostic_count == 0u
        && fixture->primitives.binding_count == 69u;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_primitive_result_destroy(&fixture->primitives);
    st_primitive_catalog_destroy(&fixture->catalog);
    st_selector_table_destroy(&fixture->selectors);
    st_class_graph_result_destroy(&fixture->graph);
    free(fixture->units);
    st_source_bundle_destroy(&fixture->bundle);
    if (fixture->directory[0] != '\0') {
        (void)unlink(fixture->image_file);
        (void)unlink(fixture->manifest);
        (void)unlink(fixture->application);
        (void)rmdir(fixture->directory);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static st_aot_compile_options_t options_for(
    const fixture_t *fixture, anvil_arch_t target)
{
    st_aot_compile_options_t options;
    memset(&options, 0, sizeof(options));
    options.bundle = &fixture->bundle;
    options.graph = &fixture->graph;
    options.selectors = &fixture->selectors;
    options.primitives = &fixture->primitives;
    options.target = target;
    options.abi = ANVIL_ABI_DEFAULT;
    options.syntax = ANVIL_SYNTAX_DEFAULT;
    options.optimization = ANVIL_OPT_STANDARD;
    options.symbol_prefix = "mini";
    options.symbol_prefix_length = 4u;
    return options;
}

static bool provenance_is_empty(const st_aot_compile_result_t *result)
{
    return result->provenance.target == ANVIL_ARCH_NONE
        && result->provenance.abi == ANVIL_ABI_DEFAULT
        && result->provenance.syntax == ANVIL_SYNTAX_DEFAULT
        && result->provenance.optimization == ANVIL_OPT_NONE
        && result->provenance.symbol_prefix == NULL
        && result->provenance.symbol_prefix_length == 0u;
}

static void test_supported_targets(const fixture_t *fixture)
{
    static const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64,
        ANVIL_ARCH_PPC64LE, ANVIL_ARCH_ZARCH
    };
    size_t target_index;
    for (target_index = 0u;
         target_index < sizeof(targets) / sizeof(targets[0]); target_index++) {
        st_aot_compile_options_t options = options_for(
            fixture, targets[target_index]);
        st_aot_compile_result_t result;
        char *assembly = NULL;
        size_t length = 0u, method_index;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
        CHECK(result.context != NULL && result.methods != NULL);
        CHECK(result.provenance.target == targets[target_index]);
        CHECK(result.provenance.abi == anvil_ctx_get_abi(result.context));
        CHECK(result.provenance.syntax == (targets[target_index]
                  == ANVIL_ARCH_ZARCH ? ANVIL_SYNTAX_HLASM
                                      : ANVIL_SYNTAX_GAS));
        CHECK(result.provenance.optimization == options.optimization);
        CHECK(result.provenance.symbol_prefix_length
              == options.symbol_prefix_length);
        CHECK(result.provenance.symbol_prefix != options.symbol_prefix);
        CHECK(strcmp(result.provenance.symbol_prefix,
                     options.symbol_prefix) == 0);
        CHECK(result.method_count == fixture->graph.method_count);
        CHECK(result.metadata.module != NULL && result.metadata.has_method_code);
        CHECK(result.metadata.method_count == result.method_count);
        CHECK(result.diagnostic_count == 0u);
        for (method_index = 0u; method_index < result.method_count;
             method_index++) {
            const st_aot_method_result_t *method = &result.methods[method_index];
            CHECK(method->method_id == method_index + 1u);
            CHECK(method->owner == fixture->graph.methods[method_index].owner);
            CHECK(method->arity == fixture->graph.methods[method_index]
                  .node->as.method.arguments.count);
            CHECK(method->lowering.module != NULL);
            CHECK(method->symbol != NULL && method->selector != NULL);
            CHECK(method->artifact.root_maps == method->root_maps);
            CHECK(method->artifact.flags
                  == (method->lowering.method_flags
                      | (method->lowering.has_primitive
                             ? ST_METHOD_PRIMITIVE : 0u)));
            CHECK((method->artifact.flags & ~(uint32_t)ST_METHOD_FLAGS_MASK)
                  == 0u);
            if (strcmp(method->selector, "nonLocalReturn") == 0) {
                size_t map_index;
                CHECK(method->lowering.method_flags
                      == (ST_METHOD_CAN_UNWIND
                          | ST_METHOD_HAS_NON_LOCAL_RETURN));
                CHECK(!method->lowering.has_primitive);
                CHECK(method->root_maps != NULL
                      && method->artifact.root_map_count != 0u);
                for (map_index = 0u;
                     map_index < method->artifact.root_map_count; map_index++) {
                    CHECK(method->root_maps[map_index].live_root_bitmap
                          == method->lowering.root_maps[map_index]
                                 .live_root_bitmap);
                    CHECK(method->root_maps[map_index].safepoint_id
                          == method->lowering.root_maps[map_index]
                                 .safepoint_id);
                }
            }
            anvil_error_t codegen_status = anvil_module_codegen(
                method->lowering.module, &assembly, &length);
            if (codegen_status != ANVIL_OK)
                fprintf(stderr, "codegen target %d method %zu: %s\n",
                        (int)targets[target_index], method_index,
                        anvil_ctx_get_error(result.context));
            CHECK(codegen_status == ANVIL_OK);
            CHECK(assembly != NULL && length != 0u);
            free(assembly);
            assembly = NULL;
            length = 0u;
        }
        CHECK(anvil_module_codegen(result.metadata.module,
                                   &assembly, &length) == ANVIL_OK);
        CHECK(assembly != NULL && length != 0u);
        free(assembly);
        st_aot_compile_result_destroy(&result);
        CHECK(provenance_is_empty(&result));
    }
}

static void test_unsupported_targets(const fixture_t *fixture)
{
    static const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86, ANVIL_ARCH_S370, ANVIL_ARCH_S370_XA,
        ANVIL_ARCH_S390, ANVIL_ARCH_PPC32
    };
    size_t index;
    for (index = 0u; index < sizeof(targets) / sizeof(targets[0]); index++) {
        st_aot_compile_options_t options = options_for(fixture, targets[index]);
        st_aot_compile_result_t result;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options)
              == ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET);
        CHECK(result.context == NULL && result.methods == NULL
              && result.method_count == 0u && result.metadata.module == NULL);
        CHECK(provenance_is_empty(&result));
        CHECK(result.diagnostic_count == 1u);
        CHECK(result.diagnostics != NULL && result.diagnostics[0].detail
              && strstr(result.diagnostics[0].detail, "64-bit") != NULL);
        st_aot_compile_result_destroy(&result);
    }
}

static void test_control_flags_and_maps(void)
{
    fixture_t fixture;
    st_aot_compile_options_t options;
    st_aot_compile_result_t result;
    const st_aot_method_result_t *control_method = NULL;
    size_t method_index;
    CHECK(fixture_init_mini(&fixture, true));
    if (fixture.graph.method_count != 4u) {
        fixture_destroy(&fixture);
        return;
    }
    options = options_for(&fixture, ANVIL_ARCH_X86_64);
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    for (method_index = 0u; method_index < result.method_count;
         method_index++) {
        const st_aot_method_result_t *method = &result.methods[method_index];
        CHECK(method->artifact.flags
              == (method->lowering.method_flags
                  | (method->lowering.has_primitive
                         ? ST_METHOD_PRIMITIVE : 0u)));
        if (strcmp(method->selector, "nonLocalReturn") == 0)
            control_method = method;
    }
    CHECK(control_method != NULL);
    if (control_method != NULL) {
        char *assembly = NULL;
        size_t assembly_length = 0u;
        CHECK(control_method->lowering.method_flags
              == (ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN));
        CHECK(control_method->artifact.flags
              == (ST_METHOD_CAN_UNWIND | ST_METHOD_HAS_NON_LOCAL_RETURN));
        CHECK(control_method->root_maps != NULL
              && control_method->artifact.root_map_count
                  == control_method->lowering.root_map_count
              && control_method->artifact.root_map_count != 0u);
        for (method_index = 0u;
             method_index < control_method->artifact.root_map_count;
             method_index++) {
            const st_image_root_map_metadata_t *adapter =
                &control_method->root_maps[method_index];
            const st_lower_root_map_t *canonical =
                &control_method->lowering.root_maps[method_index];
            CHECK(adapter->safepoint_id == canonical->safepoint_id);
            CHECK(adapter->root_count == canonical->root_count);
            CHECK(adapter->bitmap_word_count == canonical->bitmap_word_count);
            CHECK(adapter->live_root_bitmap == canonical->live_root_bitmap);
        }
        CHECK(anvil_module_codegen(control_method->lowering.module,
                                   &assembly, &assembly_length) == ANVIL_OK);
        CHECK(assembly != NULL && assembly_length != 0u);
        free(assembly);
    }
    CHECK(result.metadata.module != NULL && result.metadata.has_method_code);
    st_aot_compile_result_destroy(&result);

    /* Keep the high-pressure NLR method in the end-to-end z/Architecture
     * matrix.  Its control calls require enough live arguments to exercise
     * spill reloads inside the mainframe CALL_STACK_ARG bundle. */
    options = options_for(&fixture, ANVIL_ARCH_ZARCH);
    options.abi = ANVIL_ABI_MVS;
    options.syntax = ANVIL_SYNTAX_HLASM;
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    control_method = NULL;
    for (method_index = 0u; method_index < result.method_count;
         method_index++) {
        if (strcmp(result.methods[method_index].selector,
                   "nonLocalReturn") == 0) {
            control_method = &result.methods[method_index];
            break;
        }
    }
    CHECK(control_method != NULL);
    if (control_method != NULL) {
        char *assembly = NULL;
        size_t assembly_length = 0u;
        CHECK(anvil_module_codegen(control_method->lowering.module,
                                   &assembly, &assembly_length) == ANVIL_OK);
        CHECK(assembly != NULL && assembly_length != 0u);
        free(assembly);
    }
    st_aot_compile_result_destroy(&result);
    fixture_destroy(&fixture);
}

static void test_block_artifact_handoff(void)
{
    static const char application[] =
        "MiniApplication := Object [\n"
        "  run [ ^true ]\n"
        "  makeValue: x [ ^[ x ] ]\n"
        "  makeSelf [ ^[ self ] ]\n"
        "  makeNlr [ ^[ ^42 ] ]\n"
        "  arity2 [ ^[ :x :y | y ] value: 1 value: 2 ]\n"
        "  arity3 [ ^[ :x :y :z | z ] value: 1 value: 2 value: 3 ]\n"
        "  nested: x [ ^[ [ x ] value ] value ]\n"
        "  cells [ | x | x := 1. [ x := 2 ] value. ^[ x ] value ]\n"
        "]\n";
    static const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64,
        ANVIL_ARCH_PPC64LE, ANVIL_ARCH_ZARCH
    };
    fixture_t fixture;
    size_t target_index;
    CHECK(fixture_init_application(&fixture, application, 10u));
    if (fixture.graph.method_count != 10u) {
        fixture_destroy(&fixture);
        return;
    }
    for (target_index = 0u;
         target_index < sizeof(targets) / sizeof(targets[0]); target_index++) {
        st_aot_compile_options_t options = options_for(&fixture,
                                                       targets[target_index]);
        st_aot_compile_result_t result;
        size_t method_index, blocks = 0u, captures = 0u, block_maps = 0u;
        char *assembly = NULL;
        size_t assembly_length = 0u;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
        if (result.status != ST_AOT_COMPILE_OK) {
            st_aot_compile_result_destroy(&result);
            continue;
        }
        for (method_index = 0u; method_index < result.method_count;
             method_index++) {
            const st_aot_method_result_t *method = &result.methods[method_index];
            size_t block_index;
            CHECK(method->artifact.blocks == method->block_artifacts);
            CHECK(method->artifact.block_count == method->lowering.block_count);
            for (block_index = 0u; block_index < method->lowering.block_count;
                 block_index++) {
                const st_lower_block_artifact_t *lower =
                    &method->lowering.blocks[block_index];
                const st_image_aot_block_artifact_t *image =
                    &method->block_artifacts[block_index];
                CHECK(image->code_symbol == lower->code_symbol.bytes);
                CHECK(image->descriptor_symbol
                      == lower->descriptor_symbol.bytes);
                CHECK(image->method_descriptor_symbol
                      == lower->method_descriptor_symbol.bytes);
                CHECK(image->capture_count == 0u
                      || (image->captures != lower->captures
                          && memcmp(image->captures, lower->captures,
                              image->capture_count
                                  * sizeof(*image->captures)) == 0));
                CHECK(image->capture_count == lower->capture_count);
                CHECK(image->root_map_count == lower->root_map_count);
                blocks++;
                captures += lower->capture_count;
                block_maps += lower->root_map_count;
            }
            CHECK(anvil_module_codegen(method->lowering.module, &assembly,
                                       &assembly_length) == ANVIL_OK);
            CHECK(assembly != NULL && assembly_length != 0u);
            if (assembly != NULL && targets[target_index] != ANVIL_ARCH_ZARCH)
                for (block_index = 0u;
                     block_index < method->lowering.block_count; block_index++)
                    CHECK(strstr(assembly, method->lowering.blocks[block_index]
                                           .code_symbol.bytes) != NULL);
            free(assembly);
            assembly = NULL;
            assembly_length = 0u;
        }
        CHECK(blocks == 9u);
        CHECK(result.metadata.block_count == blocks);
        CHECK(result.metadata.block_capture_count == captures);
        CHECK(result.metadata.block_root_map_count == block_maps);
        CHECK(anvil_module_lookup_symbol(result.metadata.module,
                                         "mini_runtime_methods") != NULL);
        CHECK(anvil_module_lookup_symbol(result.metadata.module,
                                         "mini_block_descriptors") != NULL);
        CHECK(anvil_module_codegen(result.metadata.module, &assembly,
                                   &assembly_length) == ANVIL_OK);
        CHECK(assembly != NULL && assembly_length != 0u);
        free(assembly);
        st_aot_compile_result_destroy(&result);
    }
    fixture_destroy(&fixture);
}

static void test_native_emitted_block_runtime(void)
{
#if defined(__x86_64__) && !defined(_WIN32)
    static const char application[] =
        "MiniApplication := Object [\n"
        "  run [ ^true ]\n"
        "  makeValue: x [ ^[ x ] ]\n"
        "  makeSelf [ ^[ self ] ]\n"
        "  makeNlr [ ^[ ^42 ] ]\n"
        "]\n";
    static const char harness_prefix[] =
        "#include \"st_image_emit.h\"\n"
        "#include \"st_closure_bridge.h\"\n"
        "#include \"st_control_bridge.h\"\n"
        "#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "extern const st_image_metadata_descriptor_t mini_descriptor;\n"
        "static uint64_t si(int64_t n){uint64_t v=0;return st_value_from_small_integer(n,&v)?v:0;}\n"
        "static const st_image_method_metadata_t*find(const char*n){for(size_t i=0;i<mini_descriptor.method_count;"
        "i++)if(strcmp(mini_descriptor.methods[i].selector,n)==0)return &mini_descriptor.methods[i];return 0;}\n"
        "static uint64_t run(const st_image_method_metadata_t*m,uint64_t recv,uint32_t argc,uint64_t*argv,"
        "st_aot_thread_t*t){uint64_t roots[32];if(!m||!m->runtime_descriptor||"
        "m->runtime_descriptor->frame_root_capacity>32)exit(80);"
        "for(uint32_t i=0;i<m->runtime_descriptor->frame_root_capacity;"
        "i++)roots[i]=st_value_nil();StFrame f={.thread=t,.method=m->runtime_descriptor,.receiver=recv,.argv=argv,"
        ".roots=m->runtime_descriptor->frame_root_capacity?roots:0,.argc=argc,"
        ".root_count=m->runtime_descriptor->frame_root_capacity};return m->code(&f);}\n"
        "static uint64_t invoke(st_aot_thread_t*t,const StMethodDescriptor*caller,uint64_t closure){uint64_t "
        "caller_roots[32];if(caller->frame_root_capacity>32)exit(80);for(uint32_t i=0;"
        "i<caller->frame_root_capacity;i++)caller_roots[i]=st_value_nil();"
        "if(caller->frame_root_capacity)caller_roots[0]=closure;StFrame c={.thread=t,.method=caller,"
        ".receiver=st_value_true(),.roots=caller->frame_root_capacity?caller_roots:0,"
        ".root_count=caller->frame_root_capacity};st_aot_closure_target_t x;st_aot_closure_status_t "
        "s=st_aot_closure_resolve(&c,closure,0,&x);if(s!=ST_AOT_CLOSURE_OK)exit(80+(int)s);uint64_t roots[16];"
        "if(x.frame_root_capacity>16)exit(82);for(uint32_t i=0;i<x.frame_root_capacity;"
        "i++)roots[i]=st_value_nil();if(x.frame_root_capacity)roots[0]=closure;StFrame f={.thread=t,.caller=&c,"
        ".method=x.method,.home=x.home,.receiver=closure,.roots=x.frame_root_capacity?roots:0,"
        ".root_count=x.frame_root_capacity};return x.code(&f);}\n";
    static const char harness_main[] =
        "int main(void){enum{O=1,B=2,N=3,F=4,T=5,I=6,C=7,M=8,Z=8};const char*n[Z]={\"Object\",\"Block\",\"Nil\","
        "\"False\",\"True\",\"SmallInteger\",\"Character\",\"Metaclass\"};uint64_t bb=0;StClassDescriptor cs[Z];"
        "StShapeDescriptor ss[Z];const StClassDescriptor*cp[Z];const StShapeDescriptor*sp[Z];for(uint32_t i=0;i<Z;"
        "i++){cs[i]=(StClassDescriptor){i+1,(i==0||i==7)?0:1,8,i+1,i==7?ST_CLASS_METACLASS:0,n[i],strlen(n[i]),0,"
        "0};ss[i]=(StShapeDescriptor){i+1,i+1,8,24,0,ST_INDEXED_NONE,0,0};cp[i]=&cs[i];sp[i]=&ss[i];}"
        "ss[B-1]=(StShapeDescriptor){B,B,8,56,4,ST_INDEXED_VALUES,&bb,1};st_runtime_descriptors_t d={cp,Z,sp,Z};"
        "if(st_runtime_descriptors_validate(&d)!=ST_RUNTIME_OK)return 1;"
        "if(mini_descriptor.abi_version!=ST_IMAGE_METADATA_ABI_VERSION||mini_descriptor.block_count!=3||!mini_descriptor.runtime_methods||"
        "!mini_descriptor.block_descriptors)return 2;for(size_t i=0;i<mini_descriptor.method_count;"
        "i++)if(!st_method_descriptor_is_valid(mini_descriptor.methods[i].runtime_descriptor))return 3;st_heap_t "
        "h={0};if(st_heap_init(&h,&d,(st_runtime_allocator_t){0})!=ST_HEAP_OK)return 4;st_aot_closure_context_t "
        "closures={0};st_aot_closure_options_t co={.heap=&h,.closure_class_id=B,.closure_shape_id=B,"
        ".descriptors=mini_descriptor.block_descriptors,.descriptor_count=mini_descriptor.block_count};"
        "if(st_aot_closure_context_init(&closures,&co)!=ST_AOT_CLOSURE_OK)return 5;st_lookup_context_t lookup={0};"
        "if(st_lookup_context_init(&lookup,&d,(st_lookup_allocator_t){0})!=ST_LOOKUP_FOUND)return 6;"
        "st_aot_thread_t t={0};st_control_thread_t control={0};if(st_control_thread_init(&control,&t,"
        "(st_control_allocator_t){0})!=ST_CONTROL_OK)return 7;uint32_t ids[5]={N,F,T,I,C};"
        "if(!st_aot_thread_init(&t,&lookup,ids,0,&control,&closures,0,0,0,0))return 8;const "
        "st_image_method_metadata_t*mr=find(\"run\"),*mv=find(\"makeValue:\"),*ms=find(\"makeSelf\"),"
        "*mn=find(\"makeNlr\");if(!mr||!mv||!ms||!mn)return 9;uint64_t captured,argv[1],cl,keep[1];"
        "st_heap_collection_stats_t gs;if(st_heap_allocate(&h,O,O,0,0,0,&captured)!=ST_HEAP_OK)return 10;"
        "argv[0]=captured;cl=run(mv,st_value_true(),1,argv,&t);keep[0]=cl;if(st_heap_collect(&h,0,keep,1,"
        "&gs)!=ST_HEAP_OK||!st_heap_contains(&h,captured))return 11;if(invoke(&t,mr->runtime_descriptor,"
        "cl)!=captured)return 12;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK||st_heap_contains(&h,"
        "captured))return 13;cl=run(ms,st_value_true(),0,0,&t);if(invoke(&t,mr->runtime_descriptor,"
        "cl)!=st_value_true())return 14;if(st_heap_collect(&h,0,0,0,&gs)!=ST_HEAP_OK)return 15;cl=run(mn,"
        "st_value_true(),0,0,&t);StFrame cf={.thread=&t,.method=mr->runtime_descriptor,.receiver=st_value_true()};"
        "st_aot_closure_target_t rt={0};if(st_aot_closure_resolve(&cf,cl,0,"
        "&rt)!=ST_AOT_CLOSURE_OK||!rt.home)return 16;if(st_aot_control_non_local_return(&cf,rt.home,"
        "si(42))!=ST_CONTROL_ERR_BLOCK_RETURNED)return 17;if(st_heap_collect(&h,0,0,0,"
        "&gs)!=ST_HEAP_OK||st_control_live_token_count(&control)!=0)return 18;st_aot_thread_destroy(&t);"
        "if(st_aot_closure_context_destroy(&closures)!=ST_AOT_CLOSURE_OK)return 19;"
        "if(st_control_thread_destroy(&control)!=ST_CONTROL_OK)return 20;st_lookup_context_destroy(&lookup);"
        "st_heap_destroy(&h);return 0;}\n";
    fixture_t fixture;
    st_aot_compile_options_t options;
    st_aot_compile_result_t result;
    char stem[160], metadata_path[192], harness_path[192];
    char executable[192], command[8192], paths[6][192];
    size_t index, used;
    char *assembly = NULL;
    size_t length = 0u;
    CHECK(fixture_init_application(&fixture, application, 6u));
    options = options_for(&fixture, ANVIL_ARCH_X86_64);
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    if (result.status != ST_AOT_COMPILE_OK) {
        fixture_destroy(&fixture);
        return;
    }
    snprintf(stem, sizeof(stem), "/tmp/anvil-st-aot-block-%ld",
             (long)getpid());
    snprintf(metadata_path, sizeof(metadata_path), "%s-meta.s", stem);
    snprintf(harness_path, sizeof(harness_path), "%s.c", stem);
    snprintf(executable, sizeof(executable), "%s-exe", stem);
    used = (size_t)snprintf(command, sizeof(command),
                           "/usr/bin/cc -std=c11 -no-pie -Iinclude "
                           "-Isamples/smalltalk/include ");
    for (index = 0u; index < result.method_count; index++) {
        snprintf(paths[index], sizeof(paths[index]), "%s-m%zu.s", stem,
                 index);
        CHECK(anvil_module_codegen(result.methods[index].lowering.module,
                                   &assembly, &length) == ANVIL_OK);
        CHECK(assembly && write_file(paths[index], assembly, length));
        free(assembly);
        assembly = NULL;
        used += (size_t)snprintf(command + used, sizeof(command) - used,
                                 "%s ", paths[index]);
    }
    CHECK(anvil_module_codegen(result.metadata.module, &assembly, &length)
          == ANVIL_OK);
    CHECK(assembly && write_file(metadata_path, assembly, length));
    free(assembly);
    {
        FILE *harness_file = fopen(harness_path, "wb");
        bool harness_ok = harness_file != NULL;
        if (harness_ok)
            harness_ok = fwrite(harness_prefix, 1u,
                sizeof(harness_prefix) - 1u, harness_file)
                    == sizeof(harness_prefix) - 1u;
        if (harness_ok)
            harness_ok = fwrite(harness_main, 1u,
                sizeof(harness_main) - 1u, harness_file)
                    == sizeof(harness_main) - 1u;
        if (harness_file != NULL && fclose(harness_file) != 0)
            harness_ok = false;
        CHECK(harness_ok);
    }
    used += (size_t)snprintf(command + used, sizeof(command) - used,
        "%s %s samples/smalltalk/src/runtime/value.c samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/posix/runtime.c "
        "samples/smalltalk/src/runtime/primitives/core_primitives.c "
        "samples/smalltalk/src/runtime/primitives/primitive_bridge.c "
        "samples/smalltalk/src/runtime/lookup.c samples/smalltalk/src/runtime/heap.c "
        "samples/smalltalk/src/runtime/control/control.c samples/smalltalk/src/runtime/control/control_roots.c "
        "samples/smalltalk/src/runtime/send_bridge.c samples/smalltalk/src/runtime/control/control_bridge.c "
        "samples/smalltalk/src/runtime/closure_bridge.c -o %s -pthread && %s",
        metadata_path, harness_path, executable, executable);
    CHECK(used < sizeof(command));
    if (used < sizeof(command)) {
        int status = system(command);
        if (status != 0) fprintf(stderr, "emitted block runtime status=%d\n",
                                 status);
        CHECK(status == 0);
    }
    for (index = 0u; index < result.method_count; index++)
        (void)unlink(paths[index]);
    (void)unlink(metadata_path);
    (void)unlink(harness_path);
    (void)unlink(executable);
    st_aot_compile_result_destroy(&result);
    fixture_destroy(&fixture);
#endif
}

static void test_abi_syntax_matrix(const fixture_t *fixture)
{
    typedef struct {
        anvil_arch_t target;
        anvil_abi_t abi;
        anvil_syntax_t syntax;
    } configuration_t;
    static const configuration_t supported[] = {
        { ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_X86_64, ANVIL_ABI_DARWIN, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_X86_64, ANVIL_ABI_WIN64, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_ARM64, ANVIL_ABI_SYSV, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_ARM64, ANVIL_ABI_DARWIN, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS, ANVIL_SYNTAX_HLASM }
    };
    static const configuration_t rejected[] = {
        { ANVIL_ARCH_X86_64, ANVIL_ABI_MVS, ANVIL_SYNTAX_DEFAULT },
        { ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV, ANVIL_SYNTAX_HLASM },
        { ANVIL_ARCH_ARM64, ANVIL_ABI_WIN64, ANVIL_SYNTAX_GAS },
        { ANVIL_ARCH_ZARCH, ANVIL_ABI_SYSV, ANVIL_SYNTAX_HLASM },
        { ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS, ANVIL_SYNTAX_GAS }
    };
    size_t configuration_index;
    for (configuration_index = 0u;
         configuration_index < sizeof(supported) / sizeof(supported[0]);
         configuration_index++) {
        st_aot_compile_options_t options = options_for(
            fixture, supported[configuration_index].target);
        st_aot_compile_result_t result;
        size_t method_index;
        options.abi = supported[configuration_index].abi;
        options.syntax = supported[configuration_index].syntax;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
        for (method_index = 0u; method_index < result.method_count;
             method_index++) {
            char *assembly = NULL;
            size_t length = 0u;
            CHECK(anvil_module_codegen(result.methods[method_index]
                                           .lowering.module,
                                       &assembly, &length) == ANVIL_OK);
            CHECK(assembly != NULL && length != 0u);
            free(assembly);
        }
        {
            char *assembly = NULL;
            size_t length = 0u;
            CHECK(anvil_module_codegen(result.metadata.module,
                                       &assembly, &length) == ANVIL_OK);
            CHECK(assembly != NULL && length != 0u);
            free(assembly);
        }
        st_aot_compile_result_destroy(&result);
    }
    for (configuration_index = 0u;
         configuration_index < sizeof(rejected) / sizeof(rejected[0]);
         configuration_index++) {
        st_aot_compile_options_t options = options_for(
            fixture, rejected[configuration_index].target);
        st_aot_compile_result_t result;
        options.abi = rejected[configuration_index].abi;
        options.syntax = rejected[configuration_index].syntax;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options)
              == ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET);
        CHECK(result.context == NULL && result.methods == NULL
              && result.method_count == 0u && result.metadata.module == NULL);
        CHECK(provenance_is_empty(&result));
        CHECK(result.diagnostic_count == 1u);
        st_aot_compile_result_destroy(&result);
    }
}

static bool append_word(char *buffer, size_t capacity, size_t *used,
                        const char *word)
{
    int written;
    if (*used >= capacity) return false;
    written = snprintf(buffer + *used, capacity - *used, "%s", word);
    if (written < 0 || (size_t)written >= capacity - *used) return false;
    *used += (size_t)written;
    return true;
}

static void test_native_link(const fixture_t *fixture)
{
    st_aot_compile_options_t options = options_for(fixture,
                                                    ANVIL_ARCH_X86_64);
    st_aot_compile_result_t result;
    char stem[160], metadata_asm[192], metadata_obj[192], harness_path[192];
    char executable[192], command[4096], objects[2048];
    char *assembly = NULL;
    size_t length = 0u, index, used = 0u;
    static const char harness[] =
        "#include \"st_image_emit.h\"\n"
        "#include \"st_value.h\"\n"
        "#include <string.h>\n"
        "extern const st_image_metadata_descriptor_t mini_descriptor;\n"
        "int main(void) { StFrame frame = {0}; size_t i; int found = 0;\n"
        "frame.receiver = st_value_nil();\n"
        "for (i = 0; i < mini_descriptor.method_count; ++i) {\n"
        " const st_image_method_metadata_t *m = &mini_descriptor.methods[i];\n"
        " if (strcmp(m->selector, \"run\") == 0) { found = 1;\n"
        "  if (!m->code || m->code(&frame) != st_value_true()) return 2; } }\n"
        "return !found; }\n";
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    if (result.status != ST_AOT_COMPILE_OK) return;
    snprintf(stem, sizeof(stem), "/tmp/anvil-st-aot-link-%ld",
             (long)getpid());
    snprintf(metadata_asm, sizeof(metadata_asm), "%s-meta.s", stem);
    snprintf(metadata_obj, sizeof(metadata_obj), "%s-meta.o", stem);
    snprintf(harness_path, sizeof(harness_path), "%s-harness.c", stem);
    snprintf(executable, sizeof(executable), "%s-exe", stem);
    objects[0] = '\0';
    for (index = 0u; index < result.method_count; index++) {
        char asm_path[192], obj_path[192], compile[640];
        snprintf(asm_path, sizeof(asm_path), "%s-m%zu.s", stem, index);
        snprintf(obj_path, sizeof(obj_path), "%s-m%zu.o", stem, index);
        CHECK(anvil_module_codegen(result.methods[index].lowering.module,
                                   &assembly, &length) == ANVIL_OK);
        CHECK(assembly != NULL && write_file(asm_path, assembly, length));
        free(assembly);
        assembly = NULL;
        snprintf(compile, sizeof(compile), "/usr/bin/cc -c %s -o %s",
                 asm_path, obj_path);
        CHECK(system(compile) == 0);
        CHECK(append_word(objects, sizeof(objects), &used, obj_path));
        CHECK(append_word(objects, sizeof(objects), &used, " "));
    }
    CHECK(anvil_module_codegen(result.metadata.module,
                               &assembly, &length) == ANVIL_OK);
    CHECK(assembly != NULL && write_file(metadata_asm, assembly, length));
    free(assembly);
    CHECK(write_file(harness_path, harness, sizeof(harness) - 1u));
    snprintf(command, sizeof(command),
        "/usr/bin/cc -c %s -o %s && /usr/bin/cc "
        "-Iinclude -Isamples/smalltalk/include %s %s %s "
        "samples/smalltalk/src/runtime/value.c "
        "samples/smalltalk/src/runtime/primitives/core_primitives.c "
        "samples/smalltalk/src/runtime/primitives/primitive_bridge.c "
        "samples/smalltalk/src/runtime/runtime.c samples/smalltalk/src/platform/posix/runtime.c "
        "samples/smalltalk/src/runtime/lookup.c "
        "samples/smalltalk/src/runtime/send_bridge.c "
        "samples/smalltalk/src/runtime/control/control.c "
        "samples/smalltalk/src/runtime/control/control_bridge.c -o %s -pthread && %s",
        metadata_asm, metadata_obj, harness_path, objects, metadata_obj,
        executable, executable);
    CHECK(system(command) == 0);
    for (index = 0u; index < result.method_count; index++) {
        char path[192];
        snprintf(path, sizeof(path), "%s-m%zu.s", stem, index);
        (void)unlink(path);
        snprintf(path, sizeof(path), "%s-m%zu.o", stem, index);
        (void)unlink(path);
    }
    (void)unlink(metadata_asm);
    (void)unlink(metadata_obj);
    (void)unlink(harness_path);
    (void)unlink(executable);
    st_aot_compile_result_destroy(&result);
}

static void test_real_image_complete_compile(void)
{
    fixture_t fixture;
    st_aot_compile_options_t options;
    st_aot_compile_result_t result;
    bool saw_string_compare = false;
    bool saw_string_concat = false;
    char *assembly = NULL;
    size_t assembly_length = 0u;

    CHECK(fixture_init_real(&fixture));
    if (!fixture.primitives.resolved) {
        return;
    }
    CHECK(st_primitive_result_succeeded(&fixture.primitives));
    CHECK(fixture.primitives.binding_count == 69u);
    CHECK(fixture.primitives.diagnostic_count == 0u);
    options = options_for(&fixture, ANVIL_ARCH_X86_64);
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    CHECK(result.context != NULL && result.methods != NULL);
    CHECK(result.method_count == fixture.graph.method_count);
    CHECK(result.metadata.module != NULL && result.metadata.has_method_code);
    CHECK(result.metadata.method_count == result.method_count);
    CHECK(result.diagnostic_count == 0u);
    for (size_t index = 0u; index < result.method_count; index++) {
        const st_aot_method_result_t *method = &result.methods[index];
        CHECK(method->method_id == index + 1u);
        CHECK(method->lowering.module != NULL);
        CHECK(anvil_module_codegen(
                  method->lowering.module, &assembly, &assembly_length)
              == ANVIL_OK);
        CHECK(assembly != NULL && assembly_length != 0u);
        if (assembly != NULL) {
            saw_string_compare = saw_string_compare
                || strstr(
                    assembly,
                    "st_aot_string_compare_primitive_execute") != NULL;
            saw_string_concat = saw_string_concat
                || strstr(
                    assembly,
                    "st_aot_string_concat_primitive_execute") != NULL;
        }
        free(assembly);
        assembly = NULL;
        assembly_length = 0u;
    }
    CHECK(saw_string_compare && saw_string_concat);
    CHECK(anvil_module_codegen(
              result.metadata.module, &assembly, &assembly_length)
          == ANVIL_OK);
    CHECK(assembly != NULL && assembly_length != 0u);
    free(assembly);
    st_aot_compile_result_destroy(&result);
    CHECK(provenance_is_empty(&result));
    fixture_destroy(&fixture);
}

static void test_global_and_literal_integration(void)
{
    static const char application[] =
        "MiniApplication := Object [\n"
        "  first [ ^'alpha' ]\n"
        "  run [ ^Transcript ]\n"
        "  last [ ^'omega' ]\n"
        "]\n";
    static const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64, ANVIL_ARCH_PPC64,
        ANVIL_ARCH_PPC64LE, ANVIL_ARCH_ZARCH
    };
    st_aot_external_global_t transcript = {
        "Transcript", 10u, UINT32_C(0xf0000001), 1u
    };
    fixture_t fixture;
    size_t target_index;
    CHECK(fixture_init_application(&fixture, application, 5u));
    if (!st_class_graph_succeeded(&fixture.graph)) return;
    for (target_index = 0u;
         target_index < sizeof(targets) / sizeof(targets[0]); target_index++) {
        st_aot_compile_options_t options = options_for(&fixture,
                                                       targets[target_index]);
        st_aot_compile_result_t result;
        uint32_t expected_literal = 0u;
        bool saw_transcript = false;
        char *assembly = NULL;
        size_t length = 0u;
        options.external_globals = &transcript;
        options.external_global_count = 1u;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
        if (result.status != ST_AOT_COMPILE_OK) continue;
        CHECK(result.global_count == fixture.graph.catalog_entry_count + 1u);
        CHECK(result.metadata.global_count == result.global_count);
        CHECK(result.string_literal_count == 2u);
        CHECK(result.metadata.string_literal_count == 2u);
        CHECK(result.metadata.string_literal_bytes == 10u);
        for (size_t global = 0u; global < result.global_count; global++) {
            CHECK(result.globals[global].semantic_external_id != 0u);
            CHECK(result.globals[global].runtime_index < result.global_count);
            if (result.globals[global].semantic_external_id
                    == transcript.semantic_external_id) {
                saw_transcript = true;
                CHECK(result.globals[global].runtime_index == 1u);
                CHECK(result.globals[global].name_length == 10u);
                CHECK(memcmp(result.globals[global].name, "Transcript", 10u)
                      == 0);
            }
        }
        CHECK(saw_transcript);
        for (size_t method = 0u; method < result.method_count; method++) {
            const st_lower_result_t *lowering = &result.methods[method].lowering;
            for (size_t literal = 0u;
                 literal < lowering->string_literal_count; literal++) {
                const st_lower_string_literal_artifact_t *artifact =
                    &lowering->string_literals[literal];
                CHECK(artifact->literal_id == expected_literal++);
                CHECK(artifact->length == 5u);
                CHECK(memcmp(artifact->bytes,
                    artifact->literal_id == 0u ? "alpha" : "omega", 5u) == 0);
            }
        }
        CHECK(expected_literal == 2u);
        CHECK(anvil_module_lookup_symbol(result.metadata.module,
                                         "mini_globals") != NULL);
        CHECK(anvil_module_lookup_symbol(result.metadata.module,
                                         "mini_literal_bytes") != NULL);
        CHECK(anvil_module_lookup_symbol(result.metadata.module,
                                         "mini_string_literals") != NULL);
        CHECK(anvil_module_codegen(result.metadata.module, &assembly, &length)
              == ANVIL_OK);
        CHECK(assembly != NULL && length != 0u);
        free(assembly);
        assembly = NULL;
        for (size_t method = 0u; method < result.method_count; method++) {
            CHECK(anvil_module_codegen(result.methods[method].lowering.module,
                                       &assembly, &length) == ANVIL_OK);
            CHECK(assembly != NULL && length != 0u);
            free(assembly);
            assembly = NULL;
        }
        if (targets[target_index] == ANVIL_ARCH_X86_64)
            test_native_aot_image_runtime(&result);
        st_aot_compile_result_destroy(&result);
    }
    {
        st_aot_compile_options_t options = options_for(&fixture,
                                                       ANVIL_ARCH_X86_64);
        st_aot_compile_result_t result;
        st_aot_external_global_t invalid[2] = { transcript, transcript };
        options.external_globals = invalid;
        options.external_global_count = 2u;
        invalid[0].runtime_index = 0u;
        invalid[1].runtime_index = 0u;
        invalid[1].semantic_external_id++;
        invalid[1].name = "Arguments";
        invalid[1].name_length = 9u;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options)
              == ST_AOT_COMPILE_ERR_INVALID_ARGUMENT);
        CHECK(result.context == NULL && result.globals == NULL
              && result.metadata.module == NULL
              && result.diagnostic_count == 1u);
        st_aot_compile_result_destroy(&result);
        invalid[1].runtime_index = 1u;
        invalid[0].semantic_external_id = fixture.graph.catalog_entries[0]
            .external_id;
        st_aot_compile_result_init(&result);
        CHECK(st_aot_compile(&result, &options)
              == ST_AOT_COMPILE_ERR_INVALID_ARGUMENT);
        CHECK(result.context == NULL && result.globals == NULL
              && result.metadata.module == NULL);
        st_aot_compile_result_destroy(&result);
    }
    fixture_destroy(&fixture);
}

static void test_external_global_scale(void)
{
    enum { EXTERNAL_COUNT = 10000 };
    static const char application[] =
        "MiniApplication := Object [ run [ ^G09999 ] ]\n";
    fixture_t fixture;
    st_aot_external_global_t *externals = NULL;
    char (*names)[8] = NULL;
    st_aot_compile_options_t options;
    st_aot_compile_result_t result;
    CHECK(fixture_init_application(&fixture, application, 3u));
    externals = calloc(EXTERNAL_COUNT, sizeof(*externals));
    names = calloc(EXTERNAL_COUNT, sizeof(*names));
    CHECK(externals != NULL && names != NULL);
    if (!externals || !names) {
        free(names); free(externals); fixture_destroy(&fixture); return;
    }
    for (size_t index = 0u; index < EXTERNAL_COUNT; index++) {
        int length = snprintf(names[index], sizeof(names[index]), "G%05zu",
                              index);
        CHECK(length == 6);
        externals[index] = (st_aot_external_global_t){
            names[index], 6u, UINT32_C(0x70000000) + (uint32_t)index,
            (uint32_t)index
        };
    }
    options = options_for(&fixture, ANVIL_ARCH_X86_64);
    options.external_globals = externals;
    options.external_global_count = EXTERNAL_COUNT;
    st_aot_compile_result_init(&result);
    CHECK(st_aot_compile(&result, &options) == ST_AOT_COMPILE_OK);
    CHECK(result.global_count
          == (size_t)EXTERNAL_COUNT + fixture.graph.catalog_entry_count);
    CHECK(result.metadata.global_count == result.global_count);
    CHECK(result.diagnostic_count == 0u);
    st_aot_compile_result_destroy(&result);
    free(names);
    free(externals);
    fixture_destroy(&fixture);
}

static void test_oom(const fixture_t *fixture)
{
    size_t fail_after;
    bool reached_success = false;
    for (fail_after = 0u; fail_after < 512u; fail_after++) {
        failing_allocator_t allocator = { fail_after, 0u, 0u };
        st_aot_compile_options_t options = options_for(
            fixture, ANVIL_ARCH_X86_64);
        st_aot_compile_result_t result;
        st_aot_compile_status_t status;
        options.allocator = (st_aot_allocator_t){
            failing_allocate, failing_deallocate, &allocator
        };
        st_aot_compile_result_init(&result);
        status = st_aot_compile(&result, &options);
        CHECK(status == ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
              || status == ST_AOT_COMPILE_OK);
        if (status != ST_AOT_COMPILE_OK)
            CHECK(result.context == NULL && result.methods == NULL
                  && result.method_count == 0u
                  && result.metadata.module == NULL);
        if (status != ST_AOT_COMPILE_OK) CHECK(provenance_is_empty(&result));
        st_aot_compile_result_destroy(&result);
        CHECK(allocator.live == 0u);
        if (status == ST_AOT_COMPILE_OK) {
            reached_success = true;
            break;
        }
    }
    CHECK(reached_success);
}

int main(void)
{
    fixture_t fixture;
    CHECK(fixture_init_mini(&fixture, false));
    if (fixture.graph.method_count == 3u) {
        test_supported_targets(&fixture);
        test_unsupported_targets(&fixture);
        test_abi_syntax_matrix(&fixture);
        test_native_link(&fixture);
        test_oom(&fixture);
    }
    fixture_destroy(&fixture);
    test_control_flags_and_maps();
    test_block_artifact_handoff();
    test_native_emitted_block_runtime();
    test_global_and_literal_integration();
    test_external_global_scale();
    test_real_image_complete_compile();
    if (failures != 0u)
        fprintf(stderr, "%u AOT compile test(s) failed\n", failures);
    return failures == 0u ? 0 : 1;
}
