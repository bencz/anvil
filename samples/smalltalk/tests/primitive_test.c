#include "st_parser.h"
#include "st_primitive.h"
#include "st_source_bundle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
} fixture_t;

typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live;
} fault_allocator_t;

static bool text_is(st_ast_string_t value, const char *expected)
{
    size_t length = strlen(expected);
    return value.length == length &&
        (length == 0u || memcmp(value.data, expected, length) == 0);
}

static bool fixture_init(fixture_t *fixture, const char *name,
                         const char *source)
{
    memset(fixture, 0, sizeof(*fixture));
    if (!st_ast_unit_init(&fixture->unit, name)) return false;
    if (!st_parser_init_cstr(&fixture->parser, &fixture->unit, source) ||
        !st_parse_compilation_unit(&fixture->parser) ||
        !st_parser_at_end(&fixture->parser)) {
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

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *pointer;
    fault->calls++;
    if (fault->calls == fault->fail_at) return NULL;
    pointer = malloc(size);
    if (pointer) fault->live++;
    return pointer;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (!pointer) return;
    CHECK(fault->live != 0u);
    fault->live--;
    free(pointer);
}

static st_primitive_spec_t intrinsic(const char *name, uint32_t arity,
                                    st_primitive_receiver_policy_t receiver,
                                    st_primitive_failure_policy_t failure,
                                    uint32_t intrinsic_id)
{
    st_primitive_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = name;
    spec.name_length = strlen(name);
    spec.method_arity = arity;
    spec.receiver_policy = receiver;
    spec.failure_policy = failure;
    spec.implementation_kind = ST_PRIMITIVE_INTRINSIC;
    spec.intrinsic_id = intrinsic_id;
    return spec;
}

static st_primitive_spec_t runtime_primitive(
    const char *name, uint32_t arity,
    st_primitive_receiver_policy_t receiver,
    st_primitive_failure_policy_t failure, const char *symbol)
{
    st_primitive_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = name;
    spec.name_length = strlen(name);
    spec.method_arity = arity;
    spec.receiver_policy = receiver;
    spec.failure_policy = failure;
    spec.implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL;
    spec.runtime_symbol = symbol;
    spec.runtime_symbol_length = strlen(symbol);
    return spec;
}

static void test_catalog_contracts(void)
{
    st_primitive_catalog_t catalog = {0};
    st_primitive_spec_t identity = intrinsic(
        "IdentityPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 7u);
    st_primitive_spec_t incompatible = identity;
    st_primitive_spec_t runtime = runtime_primitive(
        "AtPrimitive", 1u, ST_PRIMITIVE_INSTANCE_OR_CLASS,
        ST_PRIMITIVE_FALL_THROUGH, "st_rt_at");
    const st_primitive_t *registered = NULL;

    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    CHECK(st_primitive_catalog_register(&catalog, &identity, &registered)
          == ST_PRIMITIVE_OK);
    CHECK(registered != NULL && registered->intrinsic_id == 7u);
    CHECK(text_is(registered->name, "IdentityPrimitive"));
    CHECK(st_primitive_catalog_register(&catalog, &runtime, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(st_primitive_catalog_count(&catalog) == 2u);
    CHECK(st_primitive_catalog_get(&catalog, 0u) == registered);
    CHECK(text_is(st_primitive_catalog_get(&catalog, 1u)->runtime_symbol,
                  "st_rt_at"));
    CHECK(st_primitive_catalog_get(&catalog, 1u)->intrinsic_id
          == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
    CHECK(st_primitive_catalog_lookup(&catalog, "AtPrimitive", 11u)
          == st_primitive_catalog_get(&catalog, 1u));
    CHECK(st_primitive_catalog_lookup(&catalog, "Absent", 6u) == NULL);

    CHECK(st_primitive_catalog_register(&catalog, &identity, NULL)
          == ST_PRIMITIVE_ERR_DUPLICATE);
    incompatible.method_arity = 2u;
    CHECK(st_primitive_catalog_register(&catalog, &incompatible, NULL)
          == ST_PRIMITIVE_ERR_INCOMPATIBLE);
    CHECK(st_primitive_catalog_count(&catalog) == 2u);

    identity.name = "";
    identity.name_length = 0u;
    CHECK(st_primitive_catalog_register(&catalog, &identity, NULL)
          == ST_PRIMITIVE_ERR_INVALID_NAME);
    identity = intrinsic("Broken", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
                         ST_PRIMITIVE_CANNOT_FAIL, 0u);
    CHECK(st_primitive_catalog_register(&catalog, &identity, NULL)
          == ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION);
    identity = intrinsic("Broken", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
                         ST_PRIMITIVE_CANNOT_FAIL, 2u);
    identity.runtime_symbol = "both";
    identity.runtime_symbol_length = 4u;
    CHECK(st_primitive_catalog_register(&catalog, &identity, NULL)
          == ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION);
    runtime = runtime_primitive("BadRuntime", 0u,
        ST_PRIMITIVE_INSTANCE_ONLY, ST_PRIMITIVE_CANNOT_FAIL,
        "not-a-c-symbol");
    CHECK(st_primitive_catalog_register(&catalog, &runtime, NULL)
          == ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION);
    runtime = runtime_primitive("BadRuntimeId", 0u,
        ST_PRIMITIVE_INSTANCE_ONLY, ST_PRIMITIVE_FALL_THROUGH,
        "st_rt_bad_id");
    runtime.intrinsic_id = 47u;
    CHECK(st_primitive_catalog_register(&catalog, &runtime, NULL)
          == ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION);
    CHECK(st_primitive_catalog_count(&catalog) == 2u);
    st_primitive_catalog_destroy(&catalog);
}

static void test_resolution_and_nested_namespaces(void)
{
    fixture_t image;
    fixture_t application;
    const st_ast_unit_t *units[2];
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    st_primitive_spec_t spec;

    CHECK(fixture_init(&image, "image.st",
        "Object := nil [ "
        "same: value [ <primitive: IdentityPrimitive> ] "
        "at: index [ <primitive: AtPrimitive> ^OutOfRangeError signal ] ] "
        "Kernel := Namespace [ Factory := Object [ "
        "class new [ <primitive: NewPrimitive> ] ] ]"));
    CHECK(fixture_init(&application, "application.st",
        "Application := Object [ run [ ^1 ] ]"));
    units[0] = &image.unit;
    units[1] = &application.unit;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    spec = intrinsic("IdentityPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
                     ST_PRIMITIVE_CANNOT_FAIL, 1u);
    CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
          == ST_PRIMITIVE_OK);
    spec = runtime_primitive("AtPrimitive", 1u,
        ST_PRIMITIVE_INSTANCE_ONLY, ST_PRIMITIVE_FALL_THROUGH, "st_rt_at");
    CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
          == ST_PRIMITIVE_OK);
    spec = runtime_primitive("NewPrimitive", 0u,
        ST_PRIMITIVE_CLASS_ONLY, ST_PRIMITIVE_CANNOT_FAIL, "st_rt_new");
    CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
          == ST_PRIMITIVE_OK);

    st_primitive_result_init(&result);
    CHECK(!st_primitive_result_succeeded(&result));
    CHECK(st_primitive_resolve(&result, units, 2u, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(st_primitive_result_succeeded(&result));
    CHECK(result.binding_count == 3u && result.diagnostic_count == 0u);
    CHECK(text_is(result.bindings[0].primitive->name,
                  "IdentityPrimitive"));
    CHECK(result.bindings[1].primitive->implementation_kind ==
          ST_PRIMITIVE_RUNTIME_SYMBOL);
    CHECK(result.bindings[2].method->as.method.class_side);
    CHECK(result.bindings[2].unit_index == 0u);
    CHECK(text_is(result.bindings[2].source_name, "image.st"));
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    fixture_destroy(&application);
    fixture_destroy(&image);
}

static void expect_single_diagnostic(
    const char *source, const st_primitive_spec_t *spec,
    st_primitive_diagnostic_code_t expected)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    CHECK(fixture_init(&fixture, "bad.st", source));
    units[0] = &fixture.unit;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    if (spec)
        CHECK(st_primitive_catalog_register(&catalog, spec, NULL)
              == ST_PRIMITIVE_OK);
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, 1u, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(!st_primitive_result_succeeded(&result));
    CHECK(result.binding_count == 0u);
    CHECK(result.diagnostic_count == 1u);
    if (result.diagnostic_count == 1u)
        CHECK(result.diagnostics[0].code == expected);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    fixture_destroy(&fixture);
}

static void test_resolution_diagnostics(void)
{
    st_primitive_spec_t exact = intrinsic(
        "ExactPrimitive", 1u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 9u);
    st_primitive_spec_t class_only = intrinsic(
        "ClassPrimitive", 0u, ST_PRIMITIVE_CLASS_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 10u);
    st_primitive_spec_t fallthrough = runtime_primitive(
        "FallPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_FALL_THROUGH, "st_rt_fall");

    expect_single_diagnostic(
        "Object := nil [ x [ <primitive: MissingPrimitive> ] ]", NULL,
        ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION);
    expect_single_diagnostic(
        "Object := nil [ x [ <primitive: ExactPrimitive> ] ]", &exact,
        ST_PRIMITIVE_DIAG_ARITY_MISMATCH);
    expect_single_diagnostic(
        "Object := nil [ x [ <primitive: ClassPrimitive> ] ]", &class_only,
        ST_PRIMITIVE_DIAG_RECEIVER_MISMATCH);
    expect_single_diagnostic(
        "Object := nil [ x [ <primitive: FallPrimitive> ] ]", &fallthrough,
        ST_PRIMITIVE_DIAG_MISSING_FALLBACK);
    expect_single_diagnostic(
        "Object := nil [ x: y [ <primitive: ExactPrimitive> "
        "<primitive: ExactPrimitive> ] ]", &exact,
        ST_PRIMITIVE_DIAG_DUPLICATE_PRAGMA);
    expect_single_diagnostic(
        "Object := nil [ x [ <primitive: #notAnIdentifier> ] ]", NULL,
        ST_PRIMITIVE_DIAG_MALFORMED_PRAGMA);
}

static void test_catalog_fault_atomicity(void)
{
    fault_allocator_t fault = { .fail_at = SIZE_MAX };
    st_primitive_allocator_t allocator = {
        fault_allocate, fault_deallocate, &fault
    };
    st_primitive_catalog_t catalog = {0};
    st_primitive_spec_t first = intrinsic(
        "FirstPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 1u);
    st_primitive_spec_t second = intrinsic(
        "SecondPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 2u);
    const st_primitive_t *stable;
    CHECK(st_primitive_catalog_init(&catalog, allocator));
    CHECK(st_primitive_catalog_register(&catalog, &first, &stable)
          == ST_PRIMITIVE_OK);
    CHECK(stable != NULL && fault.live == 3u);
    fault.fail_at = fault.calls + 1u;
    CHECK(st_primitive_catalog_register(&catalog, &second, NULL)
          == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(st_primitive_catalog_count(&catalog) == 1u);
    CHECK(st_primitive_catalog_get(&catalog, 0u) == stable);
    CHECK(st_primitive_catalog_lookup(&catalog, "SecondPrimitive", 15u)
          == NULL);
    fault.fail_at = SIZE_MAX;
    CHECK(st_primitive_catalog_register(&catalog, &second, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(st_primitive_catalog_count(&catalog) == 2u && fault.live == 4u);
    st_primitive_catalog_destroy(&catalog);
    CHECK(fault.live == 0u);
}

static void test_catalog_growth_fault_atomicity(void)
{
    size_t fail_offset;
    for (fail_offset = 1u; fail_offset <= 3u; fail_offset++) {
        fault_allocator_t fault = { .fail_at = fail_offset };
        st_primitive_allocator_t allocator = {
            fault_allocate, fault_deallocate, &fault
        };
        st_primitive_catalog_t catalog = {0};
        st_primitive_spec_t spec = intrinsic(
            "InitialPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
            ST_PRIMITIVE_CANNOT_FAIL, 1u);
        CHECK(st_primitive_catalog_init(&catalog, allocator));
        CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
              == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(st_primitive_catalog_count(&catalog) == 0u);
        CHECK(catalog.entry_capacity == 0u && catalog.table_capacity == 0u);
        CHECK(fault.live == 0u);
        fault.fail_at = SIZE_MAX;
        CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
              == ST_PRIMITIVE_OK);
        CHECK(st_primitive_catalog_count(&catalog) == 1u);
        st_primitive_catalog_destroy(&catalog);
        CHECK(fault.live == 0u);
    }

    /* Adding item nine grows the dense index.  Exercise failure of both the
     * stable record and replacement index without disturbing eight records. */
    for (fail_offset = 1u; fail_offset <= 2u; fail_offset++) {
        fault_allocator_t fault = { .fail_at = SIZE_MAX };
        st_primitive_allocator_t allocator = {
            fault_allocate, fault_deallocate, &fault
        };
        st_primitive_catalog_t catalog = {0};
        char names[9][32];
        const st_primitive_t *stable;
        size_t index;
        CHECK(st_primitive_catalog_init(&catalog, allocator));
        for (index = 0u; index < 8u; index++) {
            st_primitive_spec_t spec;
            (void)snprintf(names[index], sizeof(names[index]),
                           "VectorPrimitive%zu", index);
            spec = intrinsic(names[index], 0u, ST_PRIMITIVE_INSTANCE_ONLY,
                             ST_PRIMITIVE_CANNOT_FAIL,
                             (uint32_t)index + 1u);
            CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
                  == ST_PRIMITIVE_OK);
        }
        stable = st_primitive_catalog_get(&catalog, 0u);
        (void)snprintf(names[8], sizeof(names[8]), "VectorPrimitive8");
        st_primitive_spec_t ninth = intrinsic(
            names[8], 0u, ST_PRIMITIVE_INSTANCE_ONLY,
            ST_PRIMITIVE_CANNOT_FAIL, 9u);
        fault.fail_at = fault.calls + fail_offset;
        CHECK(st_primitive_catalog_register(&catalog, &ninth, NULL)
              == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(st_primitive_catalog_count(&catalog) == 8u);
        CHECK(st_primitive_catalog_get(&catalog, 0u) == stable);
        CHECK(st_primitive_catalog_lookup(&catalog, names[8],
                                          strlen(names[8])) == NULL);
        for (index = 0u; index < 8u; index++)
            CHECK(st_primitive_catalog_lookup(&catalog, names[index],
                  strlen(names[index])) == st_primitive_catalog_get(&catalog,
                                                                     index));
        st_primitive_catalog_destroy(&catalog);
        CHECK(fault.live == 0u);
    }

    /* Adding item thirteen grows/rebuilds the Robin Hood table. */
    for (fail_offset = 1u; fail_offset <= 2u; fail_offset++) {
        fault_allocator_t fault = { .fail_at = SIZE_MAX };
        st_primitive_allocator_t allocator = {
            fault_allocate, fault_deallocate, &fault
        };
        st_primitive_catalog_t catalog = {0};
        char names[13][32];
        size_t index;
        CHECK(st_primitive_catalog_init(&catalog, allocator));
        for (index = 0u; index < 12u; index++) {
            st_primitive_spec_t spec;
            (void)snprintf(names[index], sizeof(names[index]),
                           "TablePrimitive%zu", index);
            spec = intrinsic(names[index], 0u, ST_PRIMITIVE_INSTANCE_ONLY,
                             ST_PRIMITIVE_CANNOT_FAIL,
                             (uint32_t)index + 1u);
            CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
                  == ST_PRIMITIVE_OK);
        }
        (void)snprintf(names[12], sizeof(names[12]), "TablePrimitive12");
        st_primitive_spec_t thirteenth = intrinsic(
            names[12], 0u, ST_PRIMITIVE_INSTANCE_ONLY,
            ST_PRIMITIVE_CANNOT_FAIL, 13u);
        fault.fail_at = fault.calls + fail_offset;
        CHECK(st_primitive_catalog_register(&catalog, &thirteenth, NULL)
              == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(st_primitive_catalog_count(&catalog) == 12u);
        CHECK(st_primitive_catalog_lookup(&catalog, names[12],
                                          strlen(names[12])) == NULL);
        for (index = 0u; index < 12u; index++)
            CHECK(st_primitive_catalog_lookup(&catalog, names[index],
                  strlen(names[index])) != NULL);
        st_primitive_catalog_destroy(&catalog);
        CHECK(fault.live == 0u);
    }
}

static void test_catalog_masked_robin_hood_growth(void)
{
    st_primitive_catalog_t catalog = {0};
    char name[48];
    size_t index;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    for (index = 0u; index < 10000u; index++) {
        int length = snprintf(name, sizeof(name), "StressPrimitive%zu", index);
        st_primitive_spec_t spec;
        CHECK(length > 0 && (size_t)length < sizeof(name));
        spec = intrinsic(name, (uint32_t)(index % 4u),
                         ST_PRIMITIVE_INSTANCE_OR_CLASS,
                         ST_PRIMITIVE_CANNOT_FAIL, (uint32_t)index + 1u);
        CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
              == ST_PRIMITIVE_OK);
    }
    CHECK(st_primitive_catalog_count(&catalog) == 10000u);
    CHECK(catalog.table_capacity != 0u &&
          (catalog.table_capacity & (catalog.table_capacity - 1u)) == 0u);
    CHECK(catalog.count * 4u <= catalog.table_capacity * 3u);
    for (index = 0u; index < 10000u; index += 17u) {
        int length = snprintf(name, sizeof(name), "StressPrimitive%zu", index);
        const st_primitive_t *primitive = st_primitive_catalog_lookup(
            &catalog, name, (size_t)length);
        CHECK(primitive == st_primitive_catalog_get(&catalog, index));
        if (primitive) {
            CHECK(primitive->intrinsic_id == (uint32_t)index + 1u);
            CHECK(primitive->method_arity == (uint32_t)(index % 4u));
        }
    }
    st_primitive_catalog_destroy(&catalog);
}

static const char *first_existing(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) return local;
    if (access(root, R_OK) == 0) return root;
    return NULL;
}

static void test_real_image_requires_real_implementations(void)
{
    const char *image = first_existing("st-image",
                                       "samples/smalltalk/st-image");
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    const st_ast_unit_t **units;
    size_t index;
    CHECK(image != NULL);
    if (!image) return;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL)
          == ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    CHECK(bundle.image_count == 50u && bundle.count == 50u);
    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (!units) {
        st_source_bundle_destroy(&bundle);
        return;
    }
    for (index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, units, bundle.count, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(!st_primitive_result_succeeded(&result));
    CHECK(result.binding_count == 0u);
    /* Current image contract: 69 pragma uses / 64 distinct names. Until a
     * lowering or RT symbol is genuinely implemented and registered, every
     * use must remain a hard missing-implementation diagnostic. */
    CHECK(result.diagnostic_count == 69u);
    for (index = 0u; index < result.diagnostic_count; index++)
        CHECK(result.diagnostics[index].code ==
              ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION);
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    free(units);
    st_source_bundle_destroy(&bundle);
}

static void test_resolution_fault_atomicity(void)
{
    fixture_t old_fixture;
    fixture_t new_fixture;
    const st_ast_unit_t *old_units[1];
    const st_ast_unit_t *new_units[1];
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    st_primitive_spec_t spec = intrinsic(
        "ExactPrimitive", 0u, ST_PRIMITIVE_INSTANCE_ONLY,
        ST_PRIMITIVE_CANNOT_FAIL, 4u);
    fault_allocator_t fault = { .fail_at = 1u };
    st_primitive_resolve_options_t options = {{
        fault_allocate, fault_deallocate, &fault
    }};
    st_primitive_binding_t *old_bindings;

    CHECK(fixture_init(&old_fixture, "old.st",
        "Object := nil [ old [ ^1 ] ]"));
    CHECK(fixture_init(&new_fixture, "new.st",
        "Object := nil [ new [ <primitive: ExactPrimitive> ] ]"));
    old_units[0] = &old_fixture.unit;
    new_units[0] = &new_fixture.unit;
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    CHECK(st_primitive_catalog_register(&catalog, &spec, NULL)
          == ST_PRIMITIVE_OK);
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(&result, old_units, 1u, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    old_bindings = result.bindings;

    CHECK(st_primitive_resolve(&result, new_units, 1u, &catalog, &options)
          == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result.bindings == old_bindings && result.binding_count == 0u);
    CHECK(fault.live == 0u);
    fault.fail_at = 2u;
    fault.calls = 0u;
    CHECK(st_primitive_resolve(&result, new_units, 1u, &catalog, &options)
          == ST_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result.bindings == old_bindings && result.binding_count == 0u);
    CHECK(fault.live == 0u);
    fault.fail_at = SIZE_MAX;
    fault.calls = 0u;
    CHECK(st_primitive_resolve(&result, new_units, 1u, &catalog, &options)
          == ST_PRIMITIVE_OK);
    CHECK(result.bindings != old_bindings && result.binding_count == 1u);
    CHECK(st_primitive_result_succeeded(&result));
    CHECK(fault.live == 2u);
    st_primitive_result_destroy(&result);
    CHECK(fault.live == 0u);
    st_primitive_catalog_destroy(&catalog);
    fixture_destroy(&new_fixture);
    fixture_destroy(&old_fixture);
}

static void test_invalid_arguments_and_malformed_ast(void)
{
    fixture_t fixture;
    const st_ast_unit_t *units[1];
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t result;
    st_ast_node_t *saved;
    CHECK(!st_primitive_catalog_init(NULL, (st_primitive_allocator_t){0}));
    CHECK(!st_primitive_catalog_init(&catalog,
        (st_primitive_allocator_t){ fault_allocate, NULL, NULL }));
    memset(&catalog, 0, sizeof(catalog));
    CHECK(st_primitive_catalog_init(&catalog,
          (st_primitive_allocator_t){0}));
    st_primitive_result_init(&result);
    CHECK(st_primitive_resolve(NULL, NULL, 0u, &catalog, NULL)
          == ST_PRIMITIVE_ERR_INVALID_ARGUMENT);
    CHECK(st_primitive_resolve(&result, NULL, 1u, &catalog, NULL)
          == ST_PRIMITIVE_ERR_INVALID_ARGUMENT);

    CHECK(fixture_init(&fixture, "malformed.st",
        "Object := nil [ valid [ ^1 ] ]"));
    units[0] = &fixture.unit;
    saved = fixture.unit.declarations.items[0]
        ->as.class_decl.methods.items[0];
    fixture.unit.declarations.items[0]->as.class_decl.methods.items[0] = NULL;
    CHECK(st_primitive_resolve(&result, units, 1u, &catalog, NULL)
          == ST_PRIMITIVE_OK);
    CHECK(result.diagnostic_count == 1u);
    CHECK(result.diagnostics[0].code == ST_PRIMITIVE_DIAG_MALFORMED_AST);
    fixture.unit.declarations.items[0]->as.class_decl.methods.items[0] = saved;
    st_primitive_result_destroy(&result);
    st_primitive_catalog_destroy(&catalog);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_catalog_contracts();
    test_resolution_and_nested_namespaces();
    test_resolution_diagnostics();
    test_catalog_fault_atomicity();
    test_catalog_growth_fault_atomicity();
    test_catalog_masked_robin_hood_growth();
    test_resolution_fault_atomicity();
    test_invalid_arguments_and_malformed_ast();
    test_real_image_requires_real_implementations();
    if (failures != 0u) {
        fprintf(stderr, "%u primitive test(s) failed\n", failures);
        return 1;
    }
    puts("smalltalk primitive contract tests: ok");
    return 0;
}
