#include "st_product_primitives.h"
#include "st_source_bundle.h"

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

static const char *image_directory(void)
{
    if (access("st-image/manifest.txt", R_OK) == 0) return "st-image";
    if (access("samples/smalltalk/st-image/manifest.txt", R_OK) == 0)
        return "samples/smalltalk/st-image";
    return NULL;
}

int main(void)
{
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
    /* The product catalog is the one the shipped compiler uses; every image
     * pragma must bind against it with no diagnostic. */
    CHECK(st_product_primitive_catalog_build(
              &catalog, (st_primitive_allocator_t) {0}) == ST_PRIMITIVE_OK);

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
    CHECK(resolution.binding_count == 84u);
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
    puts("image primitive catalog: 84 uses, 84 complete bindings");
    return EXIT_SUCCESS;
}
