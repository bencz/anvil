#include "st_application_aot.h"

#include "st_image_runtime.h"
#include "st_product_primitives.h"
#include "st_source_bundle.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SELECTOR_HASH_SEED UINT64_C(0x53544150504c4943)

typedef struct {
    anvil_arch_t target;
    anvil_abi_t abi;
    bool supported;
} profile_definition_t;

static const profile_definition_t profile_definitions[] = {
    {ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_ARM64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_PPC64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_PPC64LE, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS, true},
    {ANVIL_ARCH_X86, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S370, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S370_XA, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S390, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_PPC32, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_X86_64, ANVIL_ABI_WIN64, true}
};

_Static_assert(
    sizeof(profile_definitions) / sizeof(profile_definitions[0])
        == ST_APPLICATION_AOT_PROFILE_COUNT,
    "application target matrix changed");

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static bool normalize_allocator(
    st_application_aot_allocator_t input,
    st_application_aot_allocator_t *output)
{
    if (output == NULL
            || ((input.allocate == NULL) != (input.deallocate == NULL))) {
        return false;
    }
    if (input.allocate == NULL) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static bool portable_name(const char *name, size_t maximum)
{
    const unsigned char *cursor = (const unsigned char *)name;

    if (name == NULL || name[0] == '\0' || strlen(name) > maximum
            || !((*cursor >= 'A' && *cursor <= 'Z')
                || (*cursor >= 'a' && *cursor <= 'z'))) {
        return false;
    }
    for (cursor++; *cursor != '\0'; cursor++) {
        if (!((*cursor >= 'A' && *cursor <= 'Z')
                || (*cursor >= 'a' && *cursor <= 'z')
                || (*cursor >= '0' && *cursor <= '9')
                || *cursor == '_')) {
            return false;
        }
    }
    return true;
}

static bool result_is_empty(const st_application_aot_result_t *result)
{
    return result != NULL && result->status == ST_APPLICATION_AOT_OK
        && result->profile_count == 0u && result->matrix_manifest == NULL
        && result->matrix_manifest_length == 0u;
}

void st_application_aot_result_init(st_application_aot_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        for (size_t index = 0u;
             index < ST_APPLICATION_AOT_PROFILE_COUNT; index++) {
            st_artifact_bundle_init(&result->profiles[index].bundle);
        }
    }
}

static void destroy_payload(st_application_aot_result_t *result)
{
    if (result == NULL) {
        return;
    }
    for (size_t index = 0u;
         index < ST_APPLICATION_AOT_PROFILE_COUNT; index++) {
        st_artifact_bundle_destroy(&result->profiles[index].bundle);
        memset(&result->profiles[index], 0, sizeof(result->profiles[index]));
    }
    if (result->matrix_manifest != NULL
            && result->allocator.deallocate != NULL) {
        result->allocator.deallocate(
            result->allocator.user, result->matrix_manifest);
    }
    result->matrix_manifest = NULL;
    result->matrix_manifest_length = 0u;
    result->profile_count = 0u;
}

void st_application_aot_result_destroy(st_application_aot_result_t *result)
{
    if (result == NULL) {
        return;
    }
    destroy_payload(result);
    memset(result, 0, sizeof(*result));
}

static st_application_aot_status_t fail(
    st_application_aot_result_t *result,
    st_application_aot_status_t status,
    st_application_aot_stage_t stage,
    anvil_arch_t target,
    const char *format, ...)
{
    va_list arguments;

    destroy_payload(result);
    result->status = status;
    result->failed_stage = stage;
    result->failed_target = target;
    va_start(arguments, format);
    (void)vsnprintf(
        result->diagnostic, sizeof(result->diagnostic), format, arguments);
    va_end(arguments);
    return status;
}

static bool ast_string_equals(st_ast_string_t string, const char *name)
{
    size_t length = strlen(name);
    return string.data != NULL && string.length == length
        && memcmp(string.data, name, length) == 0;
}

static st_class_graph_id_t find_class(
    const st_class_graph_result_t *graph, const char *name)
{
    for (size_t index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->kind == ST_CLASS_GRAPH_CLASS
                && ast_string_equals(entity->name, name)) {
            return entity->id;
        }
    }
    return ST_CLASS_GRAPH_INVALID_ID;
}

static bool find_declared_slot(
    const st_class_graph_result_t *graph, st_class_graph_id_t owner,
    const char *name, uint32_t *slot_out)
{
    *slot_out = UINT32_MAX;
    for (size_t index = 0u; index < graph->instance_slot_count; index++) {
        const st_class_graph_slot_t *slot = &graph->instance_slots[index];
        if (slot->declaring_class == owner
                && ast_string_equals(slot->name, name)) {
            *slot_out = slot->slot;
            return true;
        }
    }
    return false;
}

static bool build_launch_plan(
    const st_class_graph_result_t *graph,
    const st_selector_table_t *selectors,
    const st_application_aot_options_t *options,
    st_application_launch_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));

#define FIND_ROLE(field, name)                                                \
    do {                                                                       \
        plan->field = find_class(graph, (name));                               \
        if (plan->field == ST_CLASS_GRAPH_INVALID_ID) {                        \
            return false;                                                      \
        }                                                                      \
    } while (0)

    FIND_ROLE(entry_entity_id, options->entry_class_name);
    FIND_ROLE(nil_entity_id, "UndefinedObject");
    FIND_ROLE(false_entity_id, "False");
    FIND_ROLE(true_entity_id, "True");
    FIND_ROLE(character_entity_id, "Character");
    FIND_ROLE(object_entity_id, "Object");
    FIND_ROLE(class_entity_id, "Class");
    FIND_ROLE(metaclass_entity_id, "MetaClass");
    FIND_ROLE(integer_entity_id, "Integer");
    FIND_ROLE(small_integer_entity_id, "SmallInteger");
    FIND_ROLE(array_entity_id, "Array");
    FIND_ROLE(method_dictionary_entity_id, "MethodDictionary");
    FIND_ROLE(symbol_entity_id, "Symbol");
    FIND_ROLE(compiled_method_entity_id, "CompiledMethod");
    FIND_ROLE(string_entity_id, "String");
    FIND_ROLE(external_stream_entity_id, "ExternalStream");
    FIND_ROLE(block_entity_id, "Block");
    FIND_ROLE(closure_cell_entity_id, "ClosureCell");
    FIND_ROLE(message_entity_id, "Message");
    FIND_ROLE(large_positive_entity_id, "LargePositiveInteger");
    FIND_ROLE(large_negative_entity_id, "LargeNegativeInteger");
    FIND_ROLE(boxed_float64_entity_id, "BoxedFloat64");
#undef FIND_ROLE

    if (!st_selector_lookup(
            selectors, options->entry_selector,
            strlen(options->entry_selector), &plan->entry_selector_id)
            || !st_selector_lookup(
                selectors, "doesNotUnderstand:",
                sizeof("doesNotUnderstand:") - 1u,
                &plan->does_not_understand_selector_id)
            || !find_declared_slot(
                graph, plan->external_stream_entity_id, "descriptor",
                &plan->external_stream_descriptor_slot)
            || !find_declared_slot(
                graph, plan->message_entity_id, "selector",
                &plan->message_selector_slot)
            || !find_declared_slot(
                graph, plan->message_entity_id, "arguments",
                &plan->message_arguments_slot)) {
        return false;
    }
    plan->transcript_runtime_index = 0u;
    return true;
}

static const char *target_name(anvil_arch_t target)
{
    switch (target) {
    case ANVIL_ARCH_X86_64: return "x86_64";
    case ANVIL_ARCH_ARM64: return "arm64";
    case ANVIL_ARCH_PPC64: return "ppc64";
    case ANVIL_ARCH_PPC64LE: return "ppc64le";
    case ANVIL_ARCH_ZARCH: return "zarch";
    case ANVIL_ARCH_X86: return "x86";
    case ANVIL_ARCH_S370: return "s370";
    case ANVIL_ARCH_S370_XA: return "s370_xa";
    case ANVIL_ARCH_S390: return "s390";
    case ANVIL_ARCH_PPC32: return "ppc32";
    default: return "invalid";
    }
}

static const char *abi_name(anvil_abi_t abi)
{
    switch (abi) {
    case ANVIL_ABI_SYSV: return "sysv";
    case ANVIL_ABI_WIN64: return "win64";
    case ANVIL_ABI_MVS: return "mvs";
    case ANVIL_ABI_DEFAULT: return "default";
    default: return "invalid";
    }
}

static const char *syntax_name(anvil_syntax_t syntax)
{
    switch (syntax) {
    case ANVIL_SYNTAX_GAS: return "gas";
    case ANVIL_SYNTAX_HLASM: return "hlasm";
    case ANVIL_SYNTAX_DEFAULT: return "default";
    default: return "invalid";
    }
}

static const char *optimization_name(anvil_opt_level_t optimization)
{
    switch (optimization) {
    case ANVIL_OPT_NONE: return "O0";
    case ANVIL_OPT_BASIC: return "O1";
    case ANVIL_OPT_STANDARD: return "O2";
    case ANVIL_OPT_AGGRESSIVE: return "O3";
    default: return "invalid";
    }
}

static void hash_hex(
    const uint8_t hash[ST_ARTIFACT_SHA256_SIZE],
    char output[ST_ARTIFACT_SHA256_SIZE * 2u + 1u])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        output[index * 2u] = digits[hash[index] >> 4u];
        output[index * 2u + 1u] = digits[hash[index] & UINT8_C(0x0f)];
    }
    output[ST_ARTIFACT_SHA256_SIZE * 2u] = '\0';
}

static bool build_matrix_manifest(
    st_application_aot_result_t *result, const char *application_name)
{
    size_t capacity = strlen(application_name) + 2048u;
    char *manifest = result->allocator.allocate(
        result->allocator.user, capacity);
    size_t used = 0u;
    int amount;

    if (manifest == NULL) {
        return false;
    }
    amount = snprintf(
        manifest, capacity,
        "anvil-smalltalk-application-matrix-v%" PRIu32 "\n"
        "application=%s\nprofile-count=%u\n",
        ST_APPLICATION_AOT_MATRIX_VERSION, application_name,
        ST_APPLICATION_AOT_PROFILE_COUNT);
    if (amount < 0 || (size_t)amount >= capacity) {
        result->allocator.deallocate(result->allocator.user, manifest);
        return false;
    }
    used = (size_t)amount;
    for (size_t index = 0u; index < result->profile_count; index++) {
        const st_application_aot_profile_t *profile =
            &result->profiles[index];
        char hexadecimal[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];
        const char *state;
        const char *payload;

        if (profile->state == ST_APPLICATION_PROFILE_READY) {
            hash_hex(profile->bundle.bundle_sha256, hexadecimal);
            state = "ready";
            payload = hexadecimal;
        } else {
            state = "unsupported";
            payload = profile->reason;
        }
        amount = snprintf(
            manifest + used, capacity - used,
            "profile=%s|%s|%s|%s|%s|%s\n",
            target_name(profile->target), abi_name(profile->abi),
            syntax_name(profile->syntax),
            optimization_name(profile->optimization), state, payload);
        if (amount < 0 || (size_t)amount >= capacity - used) {
            result->allocator.deallocate(result->allocator.user, manifest);
            return false;
        }
        used += (size_t)amount;
    }
    result->matrix_manifest = manifest;
    result->matrix_manifest_length = used;
    return true;
}

st_application_aot_status_t st_application_aot_compile(
    st_application_aot_result_t *result,
    const st_application_aot_options_t *options)
{
    st_source_bundle_t sources;
    const st_ast_unit_t **units = NULL;
    st_class_graph_result_t graph;
    st_selector_table_t selectors;
    st_primitive_catalog_t catalog;
    st_primitive_result_t primitives;
    st_application_launch_plan_t launch_plan;
    st_application_aot_allocator_t allocator;
    char symbol_prefix[ST_AOT_SYMBOL_PREFIX_MAX + 1u];
    st_aot_external_global_t transcript;
    size_t source_count = 0u;
    int prefix_length;

    memset(&sources, 0, sizeof(sources));
    memset(&selectors, 0, sizeof(selectors));
    memset(&catalog, 0, sizeof(catalog));
    st_class_graph_result_init(&graph);
    st_primitive_result_init(&primitives);
    if (!result_is_empty(result) || options == NULL
            || options->image_directory == NULL
            || options->application_directory == NULL
            || !portable_name(options->application_name, 64u)
            || !portable_name(options->entry_class_name, 128u)
            || options->entry_selector == NULL
            || options->entry_selector[0] == '\0'
            || options->optimization != ANVIL_OPT_STANDARD
            || !normalize_allocator(options->allocator, &allocator)) {
        if (result != NULL) {
            result->status = ST_APPLICATION_AOT_ERR_INVALID_ARGUMENT;
        }
        return ST_APPLICATION_AOT_ERR_INVALID_ARGUMENT;
    }
    result->allocator = allocator;
    prefix_length = snprintf(
        symbol_prefix, sizeof(symbol_prefix), "st_app_%s",
        options->application_name);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(symbol_prefix)) {
        return fail(
            result, ST_APPLICATION_AOT_ERR_OVERFLOW,
            ST_APPLICATION_AOT_STAGE_NONE, ANVIL_ARCH_NONE,
            "application symbol prefix exceeds the AOT ABI");
    }

    result->source_status = st_source_bundle_load_manifests(
        &sources, options->image_directory, options->application_directory,
        &(st_source_allocator_t) {
            allocator.allocate, allocator.deallocate, allocator.user
        });
    if (result->source_status != ST_SOURCE_LOAD_OK) {
        st_application_aot_status_t status = fail(
            result, ST_APPLICATION_AOT_ERR_SOURCE,
            ST_APPLICATION_AOT_STAGE_SOURCE, ANVIL_ARCH_NONE,
            "%s during %s: %s",
            st_source_load_status_string(result->source_status),
            st_source_load_phase_string(sources.diagnostic.phase),
            sources.diagnostic.path);
        st_source_bundle_destroy(&sources);
        return status;
    }
    source_count = sources.count;
    if (source_count > SIZE_MAX / sizeof(*units)) {
        result->status = ST_APPLICATION_AOT_ERR_OVERFLOW;
        goto failure;
    }
    units = allocator.allocate(allocator.user, source_count * sizeof(*units));
    if (source_count != 0u && units == NULL) {
        result->status = ST_APPLICATION_AOT_ERR_OUT_OF_MEMORY;
        goto failure;
    }
    for (size_t index = 0u; index < source_count; index++) {
        units[index] = &sources.files[index].ast;
    }

    result->graph_status = st_class_graph_build(
        &graph, units, source_count,
        &(st_class_graph_options_t) {{
            allocator.allocate, allocator.deallocate, allocator.user
        }});
    if (result->graph_status != ST_CLASS_GRAPH_OK
            || !st_class_graph_succeeded(&graph)) {
        result->status = ST_APPLICATION_AOT_ERR_CLASS_GRAPH;
        goto failure;
    }
    result->selector_status = st_selector_table_build_for_units(
        &selectors, units, source_count,
        (st_selector_allocator_t) {
            allocator.allocate, allocator.deallocate, allocator.user
        }, SELECTOR_HASH_SEED);
    if (result->selector_status != ST_SELECTOR_OK) {
        result->status = ST_APPLICATION_AOT_ERR_SELECTORS;
        goto failure;
    }
    result->primitive_status = st_product_primitive_catalog_build(
        &catalog, (st_primitive_allocator_t) {
            allocator.allocate, allocator.deallocate, allocator.user
        });
    if (result->primitive_status != ST_PRIMITIVE_OK) {
        result->status = ST_APPLICATION_AOT_ERR_PRIMITIVES;
        goto failure;
    }
    result->primitive_status = st_primitive_resolve(
        &primitives, units, source_count, &catalog, NULL);
    if (result->primitive_status != ST_PRIMITIVE_OK
            || !st_primitive_result_succeeded(&primitives)) {
        result->status = ST_APPLICATION_AOT_ERR_PRIMITIVES;
        goto failure;
    }
    if (!build_launch_plan(&graph, &selectors, options, &launch_plan)) {
        result->status = ST_APPLICATION_AOT_ERR_ROLE;
        goto failure;
    }

    transcript = (st_aot_external_global_t) {
        .name = "Transcript",
        .name_length = sizeof("Transcript") - 1u,
        .semantic_external_id = ST_IMAGE_EXTERNAL_ID_TRANSCRIPT,
        .runtime_index = 0u
    };
    result->profile_count = ST_APPLICATION_AOT_PROFILE_COUNT;
    for (size_t index = 0u;
         index < ST_APPLICATION_AOT_PROFILE_COUNT; index++) {
        st_application_aot_profile_t *profile = &result->profiles[index];
        profile->target = profile_definitions[index].target;
        profile->optimization = options->optimization;
        if (!profile_definitions[index].supported) {
            profile->state = ST_APPLICATION_PROFILE_UNSUPPORTED;
            profile->abi = ANVIL_ABI_DEFAULT;
            profile->syntax = ANVIL_SYNTAX_DEFAULT;
            profile->reason = "tagged32-abi-unimplemented";
            continue;
        }

        st_aot_compile_result_t compiled;
        st_application_launch_result_t launch;
        st_aot_compile_options_t compile_options = {
            .bundle = &sources,
            .graph = &graph,
            .selectors = &selectors,
            .primitives = &primitives,
            .external_globals = &transcript,
            .external_global_count = 1u,
            .target = profile->target,
            .abi = profile_definitions[index].abi,
            .syntax = ANVIL_SYNTAX_DEFAULT,
            .optimization = options->optimization,
            .symbol_prefix = symbol_prefix,
            .symbol_prefix_length = (size_t)prefix_length,
            .allocator = {
                allocator.allocate, allocator.deallocate, allocator.user
            }
        };
        st_artifact_bundle_options_t bundle_options = {
            .allocator = {
                allocator.allocate, allocator.deallocate, allocator.user
            }
        };

        st_aot_compile_result_init(&compiled);
        st_application_launch_result_init(&launch);
        result->compile_status = st_aot_compile(&compiled, &compile_options);
        if (result->compile_status != ST_AOT_COMPILE_OK) {
            if (compiled.diagnostic_count != 0u
                    && compiled.diagnostics[0].detail != NULL) {
                (void)snprintf(
                    result->diagnostic, sizeof(result->diagnostic),
                    "%s", compiled.diagnostics[0].detail);
            }
            st_aot_compile_result_destroy(&compiled);
            result->failed_target = profile->target;
            result->status = ST_APPLICATION_AOT_ERR_COMPILE;
            goto failure;
        }
        result->launch_status = st_application_launch_emit(
            &launch, &compiled, &graph, &selectors, &launch_plan);
        if (result->launch_status != ST_APPLICATION_LAUNCH_OK) {
            st_application_launch_result_destroy(&launch);
            st_aot_compile_result_destroy(&compiled);
            result->failed_target = profile->target;
            result->status = ST_APPLICATION_AOT_ERR_LAUNCH;
            goto failure;
        }
        bundle_options.launch_module = launch.module;
        bundle_options.launch_symbol = launch.symbol;
        bundle_options.launch_symbol_length = launch.symbol_length;
        result->artifact_status = st_artifact_bundle_render(
            &profile->bundle, &compiled, &bundle_options);
        if (result->artifact_status != ST_ARTIFACT_BUNDLE_OK) {
            st_application_launch_result_destroy(&launch);
            st_aot_compile_result_destroy(&compiled);
            result->failed_target = profile->target;
            result->status = ST_APPLICATION_AOT_ERR_ARTIFACT;
            goto failure;
        }
        profile->state = ST_APPLICATION_PROFILE_READY;
        profile->abi = profile->bundle.abi;
        profile->syntax = profile->bundle.syntax;
        st_application_launch_result_destroy(&launch);
        st_aot_compile_result_destroy(&compiled);
    }
    if (!build_matrix_manifest(result, options->application_name)) {
        result->status = ST_APPLICATION_AOT_ERR_OUT_OF_MEMORY;
        goto failure;
    }

    allocator.deallocate(allocator.user, units);
    st_primitive_result_destroy(&primitives);
    st_primitive_catalog_destroy(&catalog);
    st_selector_table_destroy(&selectors);
    st_class_graph_result_destroy(&graph);
    st_source_bundle_destroy(&sources);
    result->status = ST_APPLICATION_AOT_OK;
    result->failed_stage = ST_APPLICATION_AOT_STAGE_NONE;
    return result->status;

failure:
    {
        st_application_aot_status_t status = result->status;
        st_application_aot_stage_t stage = ST_APPLICATION_AOT_STAGE_NONE;
        char saved[ST_APPLICATION_AOT_DIAGNOSTIC_CAPACITY];

        memcpy(saved, result->diagnostic, sizeof(saved));
        switch (status) {
        case ST_APPLICATION_AOT_ERR_CLASS_GRAPH:
            stage = ST_APPLICATION_AOT_STAGE_CLASS_GRAPH;
            break;
        case ST_APPLICATION_AOT_ERR_SELECTORS:
            stage = ST_APPLICATION_AOT_STAGE_SELECTORS;
            break;
        case ST_APPLICATION_AOT_ERR_PRIMITIVES:
            stage = ST_APPLICATION_AOT_STAGE_PRIMITIVES;
            break;
        case ST_APPLICATION_AOT_ERR_ROLE:
            stage = ST_APPLICATION_AOT_STAGE_ROLES;
            break;
        case ST_APPLICATION_AOT_ERR_COMPILE:
            stage = ST_APPLICATION_AOT_STAGE_COMPILE;
            break;
        case ST_APPLICATION_AOT_ERR_LAUNCH:
            stage = ST_APPLICATION_AOT_STAGE_LAUNCH;
            break;
        case ST_APPLICATION_AOT_ERR_ARTIFACT:
            stage = ST_APPLICATION_AOT_STAGE_ARTIFACT;
            break;
        case ST_APPLICATION_AOT_ERR_OUT_OF_MEMORY:
        case ST_APPLICATION_AOT_ERR_OVERFLOW:
        default:
            break;
        }
        allocator.deallocate(allocator.user, units);
        st_primitive_result_destroy(&primitives);
        st_primitive_catalog_destroy(&catalog);
        st_selector_table_destroy(&selectors);
        st_class_graph_result_destroy(&graph);
        st_source_bundle_destroy(&sources);
        destroy_payload(result);
        result->status = status;
        result->failed_stage = stage;
        if (saved[0] != '\0') {
            memcpy(result->diagnostic, saved, sizeof(saved));
        } else {
            (void)snprintf(
                result->diagnostic, sizeof(result->diagnostic),
                "%s failed during %s",
                st_application_aot_status_string(status),
                st_application_aot_stage_string(stage));
        }
        return status;
    }
}

const char *st_application_aot_status_string(
    st_application_aot_status_t status)
{
    switch (status) {
    case ST_APPLICATION_AOT_OK: return "ok";
    case ST_APPLICATION_AOT_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_APPLICATION_AOT_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_APPLICATION_AOT_ERR_OVERFLOW: return "overflow";
    case ST_APPLICATION_AOT_ERR_SOURCE: return "source load failed";
    case ST_APPLICATION_AOT_ERR_CLASS_GRAPH: return "class graph failed";
    case ST_APPLICATION_AOT_ERR_SELECTORS: return "selector build failed";
    case ST_APPLICATION_AOT_ERR_PRIMITIVES: return "primitive resolution failed";
    case ST_APPLICATION_AOT_ERR_ROLE: return "launch role resolution failed";
    case ST_APPLICATION_AOT_ERR_COMPILE: return "AOT compile failed";
    case ST_APPLICATION_AOT_ERR_LAUNCH: return "launch emission failed";
    case ST_APPLICATION_AOT_ERR_ARTIFACT: return "artifact rendering failed";
    default: return "unknown application AOT status";
    }
}

const char *st_application_aot_stage_string(st_application_aot_stage_t stage)
{
    switch (stage) {
    case ST_APPLICATION_AOT_STAGE_NONE: return "none";
    case ST_APPLICATION_AOT_STAGE_SOURCE: return "source";
    case ST_APPLICATION_AOT_STAGE_CLASS_GRAPH: return "class graph";
    case ST_APPLICATION_AOT_STAGE_SELECTORS: return "selectors";
    case ST_APPLICATION_AOT_STAGE_PRIMITIVES: return "primitives";
    case ST_APPLICATION_AOT_STAGE_ROLES: return "launch roles";
    case ST_APPLICATION_AOT_STAGE_COMPILE: return "compile";
    case ST_APPLICATION_AOT_STAGE_LAUNCH: return "launch";
    case ST_APPLICATION_AOT_STAGE_ARTIFACT: return "artifact";
    case ST_APPLICATION_AOT_STAGE_MATRIX: return "matrix";
    default: return "unknown application AOT stage";
    }
}
