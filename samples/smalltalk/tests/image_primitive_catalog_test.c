#include "st_block_primitives.h"
#include "st_core_primitives.h"
#include "st_exception_primitives.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_integer_primitives.h"
#include "st_reflection_primitives.h"
#include "st_source_bundle.h"
#include "st_stream_primitives.h"
#include "st_string_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

typedef const st_primitive_spec_t *(*spec_provider_t)(size_t *count_out);

static const char *image_directory(void)
{
    if (access("st-image/manifest.txt", R_OK) == 0) return "st-image";
    if (access("samples/smalltalk/st-image/manifest.txt", R_OK) == 0)
        return "samples/smalltalk/st-image";
    return NULL;
}

static void register_provider(
    st_primitive_catalog_t *catalog, spec_provider_t provider)
{
    size_t count = 0u;
    const st_primitive_spec_t *specs = provider(&count);
    CHECK(specs != NULL && count != 0u);
    if (specs == NULL) return;
    for (size_t index = 0u; index < count; index++)
        CHECK(st_primitive_catalog_register(
                  catalog, &specs[index], NULL) == ST_PRIMITIVE_OK);
}

int main(void)
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
    const char *image = image_directory();
    st_source_bundle_t bundle;
    st_primitive_catalog_t catalog = {0};
    st_primitive_result_t resolution;
    const st_ast_unit_t **units = NULL;

    CHECK(image != NULL);
    if (image == NULL) return EXIT_FAILURE;
    CHECK(st_source_bundle_load(&bundle, image, NULL, 0u, NULL) ==
          ST_SOURCE_LOAD_OK);
    if (bundle.diagnostic.status != ST_SOURCE_LOAD_OK) return EXIT_FAILURE;
    CHECK(st_primitive_catalog_init(
        &catalog, (st_primitive_allocator_t) {0}));
    for (size_t index = 0u;
         index < sizeof(providers) / sizeof(providers[0]); index++)
        register_provider(&catalog, providers[index]);

    units = malloc(bundle.count * sizeof(*units));
    CHECK(units != NULL);
    if (units == NULL) goto done;
    for (size_t index = 0u; index < bundle.count; index++)
        units[index] = &bundle.files[index].ast;

    st_primitive_result_init(&resolution);
    CHECK(st_primitive_resolve(
              &resolution, units, bundle.count, &catalog, NULL) ==
          ST_PRIMITIVE_OK);
    CHECK(resolution.diagnostic_count == 0u);
    CHECK(resolution.binding_count == 69u);
    for (size_t index = 0u; index < resolution.diagnostic_count; index++)
        CHECK(resolution.diagnostics[index].code !=
              ST_PRIMITIVE_DIAG_MISSING_FALLBACK);
    st_primitive_result_destroy(&resolution);

done:
    free(units);
    st_primitive_catalog_destroy(&catalog);
    st_source_bundle_destroy(&bundle);
    if (failures != 0u) {
        fprintf(stderr, "image primitive catalog: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("image primitive catalog: 69 uses, 69 complete bindings");
    return EXIT_SUCCESS;
}
