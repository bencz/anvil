#include "st_product_primitives.h"

#include "st_block_primitives.h"
#include "st_core_primitives.h"
#include "st_exception_primitives.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_integer_primitives.h"
#include "st_reflection_primitives.h"
#include "st_stream_primitives.h"
#include "st_string_primitives.h"

typedef const st_primitive_spec_t *(*primitive_provider_t)(size_t *count_out);

st_primitive_status_t st_product_primitive_catalog_build(
    st_primitive_catalog_t *catalog,
    st_primitive_allocator_t allocator)
{
    static const primitive_provider_t providers[] = {
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
    st_primitive_catalog_t built = {0};
    size_t provider_index;

    if (catalog == NULL || catalog->initialized) {
        return ST_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    if (!st_primitive_catalog_init(&built, allocator)) {
        return built.status;
    }
    for (provider_index = 0u;
         provider_index < sizeof(providers) / sizeof(providers[0]);
         provider_index++) {
        const st_primitive_spec_t *specifications;
        size_t count = 0u;
        size_t spec_index;

        specifications = providers[provider_index](&count);
        if (specifications == NULL || count == 0u) {
            st_primitive_catalog_destroy(&built);
            return ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION;
        }
        for (spec_index = 0u; spec_index < count; spec_index++) {
            st_primitive_status_t status = st_primitive_catalog_register(
                &built, &specifications[spec_index], NULL);
            if (status != ST_PRIMITIVE_OK) {
                st_primitive_catalog_destroy(&built);
                return status;
            }
        }
    }
    *catalog = built;
    return ST_PRIMITIVE_OK;
}
