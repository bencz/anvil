#ifndef ANVIL_SMALLTALK_AOT_BOOTSTRAP_H
#define ANVIL_SMALLTALK_AOT_BOOTSTRAP_H

#include "st_image_emit.h"
#include "st_reflection_primitives.h"
#include "st_symbol_intern.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_AOT_BOOTSTRAP_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_AOT_BOOTSTRAP_OK = 0,
    ST_AOT_BOOTSTRAP_ERR_INVALID_ARGUMENT,
    ST_AOT_BOOTSTRAP_ERR_INVALID_STATE,
    ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA,
    ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR,
    ST_AOT_BOOTSTRAP_ERR_INVALID_LAYOUT,
    ST_AOT_BOOTSTRAP_ERR_INVALID_HIERARCHY,
    ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY,
    ST_AOT_BOOTSTRAP_ERR_OVERFLOW,
    ST_AOT_BOOTSTRAP_ERR_HEAP,
    ST_AOT_BOOTSTRAP_ERR_SYMBOLS,
    ST_AOT_BOOTSTRAP_ERR_REFLECTION,
    ST_AOT_BOOTSTRAP_ERR_CONFLICT
} st_aot_bootstrap_status_t;

typedef struct {
    const st_image_metadata_descriptor_t *metadata;
    st_image_runtime_t *image;
    st_symbol_intern_context_t *symbols;
    st_lookup_context_t *lookup;

    /* ABI-v5 graph roles. They are explicit identities, never name lookups. */
    uint32_t object_entity_id;
    uint32_t class_object_layout_entity_id; /* Class: exactly 9 pointer slots. */
    uint32_t metaclass_entity_id;           /* MetaClass: instanceClass slot. */
    uint32_t integer_entity_id;
    uint32_t small_integer_entity_id;

    /* Explicit managed support layouts. Both shapes must have no fixed words
     * and ST_INDEXED_VALUES storage. MethodDictionary payloads are immutable
     * sorted pairs [selector Symbol, CompiledMethod]. */
    uint32_t array_class_id;
    uint32_t array_shape_id;
    uint32_t method_dictionary_class_id;
    uint32_t method_dictionary_shape_id;

    uint32_t symbol_class_id;
    uint32_t compiled_method_class_id;
    uint32_t compiled_method_shape_id;
    st_primitive_allocator_t allocator;
} st_aot_bootstrap_options_t;

typedef struct st_aot_bootstrap_state st_aot_bootstrap_state_t;
typedef struct {
    uint32_t abi_version;
    bool initialized;
    st_image_runtime_t *image;
    st_symbol_intern_context_t *symbols;
    st_lookup_context_t *lookup;
    st_image_root_provider_t root_provider;
    st_reflection_context_t reflection;
    st_aot_bootstrap_state_t *state;
} st_aot_bootstrap_context_t;

/* Builds the complete managed class/metaclass graph and eager method mirrors
 * as one externally atomic operation. The implementation may temporarily
 * attach private root providers while reflection allocates, but every failure
 * detaches them and leaves the interner/context unpublished; heap allocations
 * then remain unreachable and are reclaimed by the next precise collection.
 * Metadata/descriptors, image, interner and lookup are borrowed and must
 * outlive the context. Destroy this context before destroying the interner,
 * lookup, image or heap; heap/provider mutation remains externally serialized. */
st_aot_bootstrap_status_t st_aot_bootstrap_context_init(
    st_aot_bootstrap_context_t *context,
    const st_aot_bootstrap_options_t *options);
void st_aot_bootstrap_context_destroy(st_aot_bootstrap_context_t *context);

/* Borrowed immutable dense ID-1 maps, authenticated during initialization. */
const st_value_t *st_aot_bootstrap_class_objects(
    const st_aot_bootstrap_context_t *context, size_t *count_out);
const st_value_t *st_aot_bootstrap_selector_symbols(
    const st_aot_bootstrap_context_t *context, size_t *count_out);
const st_reflection_context_t *st_aot_bootstrap_reflection(
    const st_aot_bootstrap_context_t *context);

const char *st_aot_bootstrap_status_string(st_aot_bootstrap_status_t status);

#ifdef __cplusplus
}
#endif

#endif
