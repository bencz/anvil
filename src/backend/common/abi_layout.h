/* Target-independent view of an aggregate's scalar layout, used by target
 * ABI modules to classify structs and arrays passed or returned by value. */
#ifndef ANVIL_BACKEND_ABI_LAYOUT_H
#define ANVIL_BACKEND_ABI_LAYOUT_H

#include "anvil/anvil_internal.h"

/* Called for each scalar leaf (integer, pointer or floating) in increasing
 * offset order.  Return false to stop the walk. */
typedef bool (*anvil_abi_leaf_fn)(void *user, anvil_type_t *leaf, size_t offset);

/* Visits every scalar leaf of a struct or array type.  Returns false when
 * the walk is stopped or the type contains something that is neither a
 * scalar nor an aggregate of scalars (vectors, functions). */
bool anvil_abi_visit_leaves(anvil_type_t *type, size_t base,
                            anvil_abi_leaf_fn visit, void *user);

/* Floating base type and member count when `type` is a homogeneous
 * floating aggregate (every leaf the same floating type), else NULL. */
anvil_type_t *anvil_abi_homogeneous_float(anvil_type_t *type, size_t *count);

/* True when some scalar leaf is not naturally aligned (packed layouts). */
bool anvil_abi_has_unaligned_leaf(anvil_type_t *type);

#endif
