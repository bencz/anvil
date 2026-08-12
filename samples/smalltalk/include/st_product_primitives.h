#ifndef ANVIL_SMALLTALK_PRODUCT_PRIMITIVES_H
#define ANVIL_SMALLTALK_PRODUCT_PRIMITIVES_H

#include "st_primitive.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the complete primitive catalog shipped by the Smalltalk product.
 * This belongs to the language runtime layer, not Anvil's generic IR core.
 * Construction is transactional: `catalog` must be zero-initialized and
 * remains empty on failure. */
st_primitive_status_t st_product_primitive_catalog_build(
    st_primitive_catalog_t *catalog,
    st_primitive_allocator_t allocator);

#ifdef __cplusplus
}
#endif

#endif
