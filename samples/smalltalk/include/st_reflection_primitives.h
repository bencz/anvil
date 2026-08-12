#ifndef ANVIL_SMALLTALK_REFLECTION_PRIMITIVES_H
#define ANVIL_SMALLTALK_REFLECTION_PRIMITIVES_H

#include "st_image_runtime.h"
#include "st_lookup.h"
#include "st_primitive.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_REFLECTION_CONTEXT_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_REFLECTION_PRIMITIVE_OK = 0,
    ST_REFLECTION_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_REFLECTION_PRIMITIVE_ERR_INVALID_STATE,
    ST_REFLECTION_PRIMITIVE_ERR_WRONG_ARITY,
    ST_REFLECTION_PRIMITIVE_ERR_INVALID_VALUE,
    ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH,
    ST_REFLECTION_PRIMITIVE_ERR_NOT_MEMBER,
    ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR,
    ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT,
    ST_REFLECTION_PRIMITIVE_ERR_LOOKUP,
    ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_REFLECTION_PRIMITIVE_ERR_OVERFLOW,
    ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT
} st_reflection_primitive_status_t;

typedef struct st_reflection_state st_reflection_state_t;

typedef struct st_reflection_context {
    uint32_t abi_version;
    bool initialized;
    st_image_runtime_t *image;
    st_lookup_context_t *lookup;
    st_image_root_provider_t root_provider;
    st_reflection_state_t *state;
} st_reflection_context_t;

typedef struct {
    st_image_runtime_t *image;
    st_lookup_context_t *lookup;

    /* Dense runtime-class-ID map. Element zero is the managed Behavior object
     * representing class ID one. Every value must already be an initialized
     * root of `image`; initialization copies the tagged values, not ownership. */
    const st_value_t *class_objects_by_id;
    size_t class_object_count;

    /* Dense selector-ID map. Element zero is the canonical immutable Symbol
     * for selector ID one. These values must likewise be image roots. */
    const st_value_t *selector_symbols_by_id;
    size_t selector_symbol_count;

    uint32_t symbol_class_id;
    uint32_t compiled_method_class_id;
    uint32_t compiled_method_shape_id;
    st_primitive_allocator_t allocator;
} st_reflection_context_options_t;

/* Initialization proves that image, heap and lookup share the identical
 * descriptor graph. It authenticates every class object and selector Symbol,
 * proves that every bootstrap object is rooted by the image runtime, and
 * builds immutable power-of-two Robin Hood identity maps transactionally. */
st_reflection_primitive_status_t st_reflection_context_init(
    st_reflection_context_t *context,
    const st_reflection_context_options_t *options);
void st_reflection_context_destroy(st_reflection_context_t *context);

bool st_reflection_context_matches(
    const st_reflection_context_t *context,
    const st_image_runtime_t *image,
    const st_lookup_context_t *lookup);

/* Ordinary semantic lookup. The same currently published MethodEntry/binding
 * pair returns the identical rooted CompiledMethod. Publishing a new binding
 * creates a new immutable snapshot and makes the old snapshot collectible.
 * A miss is successful and returns the real Smalltalk nil immediate. All
 * contract, corruption and allocation failures remain distinguishable and
 * leave result_out invalid. */
st_reflection_primitive_status_t st_reflection_lookup_selector(
    st_reflection_context_t *context, st_value_t behavior,
    st_value_t selector, st_value_t *result_out);

/* Bootstrap-only, allocation-free access to the eager immutable snapshot for
 * an emitted MethodEntry. It fails if the entry is foreign or its binding was
 * replaced after reflection initialization. */
st_reflection_primitive_status_t st_reflection_compiled_method_for_entry(
    st_reflection_context_t *context, const StMethodEntry *entry,
    st_value_t *result_out);

const st_primitive_spec_t *st_reflection_primitive_specs(
    size_t *count_out);
const char *st_reflection_primitive_status_string(
    st_reflection_primitive_status_t status);

#ifdef __cplusplus
}
#endif

#endif
