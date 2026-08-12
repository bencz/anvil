#ifndef ANVIL_SMALLTALK_IMAGE_EMIT_H
#define ANVIL_SMALLTALK_IMAGE_EMIT_H

#include "st_class_graph.h"
#include "st_closure_bridge.h"
#include "st_image_layout.h"
#include "st_runtime.h"
#include "st_selector.h"
#include "st_source_bundle.h"

#include <anvil/anvil.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST_IMAGE_METADATA_ABI_VERSION UINT32_C(5)
#define ST_IMAGE_METADATA_MAGIC UINT64_C(0x414e56494c535449)

enum {
    ST_IMAGE_METADATA_FLAG_METADATA_ONLY = UINT32_C(1) << 0,
    ST_IMAGE_METADATA_FLAG_TYPED_RELOCATIONS = UINT32_C(1) << 1,
    ST_IMAGE_METADATA_FLAG_METHOD_CODE = UINT32_C(1) << 2,
    ST_IMAGE_METADATA_FLAG_ROOT_MAPS = UINT32_C(1) << 3,
    ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS = UINT32_C(1) << 4,
    ST_IMAGE_METADATA_FLAG_BLOCK_CODE = UINT32_C(1) << 5,
    ST_IMAGE_METADATA_FLAG_IMAGE_RUNTIME_TABLES = UINT32_C(1) << 6,
    ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS = UINT32_C(1) << 7
};

enum {
    ST_IMAGE_METHOD_FLAG_CLASS_SIDE = UINT32_C(1) << 0
};

enum {
    ST_IMAGE_SLOT_FLAG_HAS_TYPE = UINT32_C(1) << 0
};

/* ABI records consumed by the AOT runtime.  The emitter builds equivalent
 * target-native Anvil struct types instead of serializing host structs, so a
 * cross-compilation always uses the target's pointer width and alignment. */
typedef struct {
    uint32_t id;
    uint32_t kind;
    uint64_t hash;
    uint32_t arity;
    const char *spelling;
} st_image_selector_metadata_t;

typedef struct {
    uint32_t declaring_class;
    uint32_t slot;
    uint32_t kind;
    uint32_t flags;
    uint32_t origin_unit;
    uint32_t origin_line;
    uint32_t origin_column;
    const char *name;
    const char *type_name;
} st_image_slot_metadata_t;

typedef struct {
    uint32_t safepoint_id;
    uint32_t root_count;
    size_t bitmap_word_count;
    const uint64_t *live_root_bitmap;
} st_image_root_map_metadata_t;

/* Canonical input artifacts and their target-native ABI records.  Global
 * runtime indices form a dense permutation of [0, global_count); literal IDs
 * are dense and ordered by (method_id, lexical ordinal). */
typedef struct {
    uint32_t semantic_external_id;
    uint32_t runtime_index;
    const char *name;
    size_t name_length;
} st_image_global_artifact_t;

typedef struct {
    uint32_t literal_id;
    st_class_graph_method_id_t method_id;
    const unsigned char *bytes;
    size_t length;
} st_image_string_literal_artifact_t;

typedef struct {
    uint32_t semantic_external_id;
    uint32_t runtime_index;
    const char *name;
} st_image_global_metadata_t;

typedef struct {
    uint32_t literal_id;
    uint32_t method_id;
    const unsigned char *bytes;
    size_t length;
} st_image_string_literal_metadata_t;

enum {
    ST_IMAGE_RUNTIME_LAYOUT_DEFAULT = UINT32_C(1) << 0
};

typedef struct {
    uint32_t graph_entity_id;
    uint32_t runtime_class_id;
    uint32_t runtime_shape_id;
    uint32_t recipe;
    uint32_t flags;
} st_image_runtime_layout_metadata_t;

typedef struct {
    uint32_t id;
    uint32_t owner;
    uint32_t instance_class;
    uint32_t lexical_super;
    uint32_t selector_id;
    uint32_t arity;
    uint32_t flags;
    uint32_t origin_unit;
    uint32_t origin_line;
    uint32_t origin_column;
    uint32_t frame_root_capacity;
    uint32_t aot_flags;
    st_method_code_t code;
    const st_image_root_map_metadata_t *root_maps;
    size_t root_map_count;
    const char *selector;
    const char *source_name;
    const StMethodDescriptor *runtime_descriptor;
} st_image_method_metadata_t;

typedef struct {
    uint32_t lexical_ordinal;
    uint32_t arity;
    uint32_t flags;
    uint32_t method_flags;
    uint32_t frame_root_capacity;
    const char *code_symbol;
    size_t code_symbol_length;
    const char *descriptor_symbol;
    size_t descriptor_symbol_length;
    const char *method_descriptor_symbol;
    size_t method_descriptor_symbol_length;
    const st_aot_capture_descriptor_t *captures;
    size_t capture_count;
    const st_image_root_map_metadata_t *root_maps;
    size_t root_map_count;
} st_image_aot_block_artifact_t;

/* Stable handoff record owned by the AOT driver. All pointed-to storage is
 * borrowed for st_image_emit_metadata(). Redundant owner/selector/arity fields
 * deliberately detect stale or mis-associated lowering artifacts. */
typedef struct {
    st_class_graph_method_id_t method_id;
    st_class_graph_id_t owner;
    const char *selector;
    size_t selector_length;
    uint32_t arity;
    const char *symbol;
    size_t symbol_length;
    uint32_t frame_root_capacity;
    uint32_t flags;
    const st_image_root_map_metadata_t *root_maps;
    size_t root_map_count;
    const st_image_aot_block_artifact_t *blocks;
    size_t block_count;
    const st_image_string_literal_artifact_t *string_literals;
    size_t string_literal_count;
} st_image_aot_method_artifact_t;

typedef struct {
    uint32_t id;
    uint32_t kind;
    uint32_t namespace_id;
    uint32_t superclass_id;
    uint32_t metaclass_id;
    uint32_t instance_class_id;
    uint32_t instance_slot_offset;
    uint32_t instance_slot_count;
    uint32_t class_variable_offset;
    uint32_t class_variable_count;
    uint32_t method_offset;
    uint32_t method_count;
    uint32_t origin_unit;
    uint32_t origin_line;
    uint32_t origin_column;
    const char *name;
} st_image_entity_metadata_t;

typedef struct {
    uint64_t magic;
    uint32_t abi_version;
    uint32_t flags;
    uint32_t pointer_size;
    uint32_t endian;
    uint32_t image_source_count;
    uint32_t application_source_count;
    uint32_t entity_count;
    uint32_t class_count;
    uint32_t metaclass_count;
    uint32_t namespace_count;
    uint32_t method_count;
    uint32_t selector_count;
    uint32_t instance_slot_count;
    uint32_t class_variable_count;
    uint32_t string_bytes;
    uint32_t block_count;
    uint32_t block_capture_count;
    uint32_t global_count;
    uint32_t string_literal_count;
    uint32_t string_literal_bytes;
    uint32_t runtime_class_count;
    uint32_t runtime_shape_count;
    uint32_t runtime_layout_count;
    const st_image_entity_metadata_t *entities;
    const st_image_method_metadata_t *methods;
    const st_image_selector_metadata_t *selectors;
    const st_image_slot_metadata_t *instance_slots;
    const st_image_slot_metadata_t *class_variables;
    const char *strings;
    const StMethodDescriptor *runtime_methods;
    const st_aot_block_descriptor_t *const *block_descriptors;
    const st_image_global_metadata_t *globals;
    const st_image_string_literal_metadata_t *string_literals;
    const uint32_t *entity_runtime_class_ids;
    const st_image_runtime_layout_metadata_t *runtime_layouts;
    const st_runtime_descriptors_t *runtime_descriptors;
} st_image_metadata_descriptor_t;

typedef enum {
    ST_IMAGE_EMIT_OK = 0,
    ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT,
    ST_IMAGE_EMIT_ERR_INVALID_BUNDLE,
    ST_IMAGE_EMIT_ERR_INVALID_GRAPH,
    ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT,
    ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT,
    ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT,
    ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT,
    ST_IMAGE_EMIT_ERR_UNSUPPORTED_BLOCK_FEATURE,
    ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY,
    ST_IMAGE_EMIT_ERR_OVERFLOW,
    ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET,
    ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE,
    ST_IMAGE_EMIT_ERR_ANVIL
} st_image_emit_status_t;

typedef void *(*st_image_emit_allocate_fn)(void *user, size_t size);
typedef void (*st_image_emit_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_image_emit_allocate_fn allocate;
    st_image_emit_deallocate_fn deallocate;
    void *user;
} st_image_emit_allocator_t;

typedef struct {
    st_image_emit_allocator_t allocator;
    const char *module_name;
    const char *symbol_prefix;

    /* Authoritative frozen selector snapshot shared with lowering.  Metadata
     * emits every entry with its existing ID, including sends without a local
     * implementation.  The emitter never reconstructs or renumbers it. */
    const st_selector_table_t *selectors;

    const st_image_aot_method_artifact_t *method_artifacts;
    size_t method_artifact_count;
    const st_image_global_artifact_t *globals;
    size_t global_count;
    /* Optional precompiled authoritative layout. The AOT driver supplies this
     * exact plan to lowering and emission. A metadata-only caller may omit it;
     * the emitter then builds and destroys an equivalent private plan. */
    const st_image_layout_result_t *layout;

    /* The metadata emitter deliberately does not manufacture method entry
     * points. Setting this option requires complete method_artifacts coverage;
     * absent code returns ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE. */
    bool require_method_code;
} st_image_emit_options_t;

typedef struct {
    st_image_emit_status_t status;
    anvil_module_t *module;
    size_t source_count;
    size_t image_source_count;
    size_t entity_count;
    size_t method_count;
    size_t selector_count;
    size_t instance_slot_count;
    size_t class_variable_count;
    size_t root_map_count;
    size_t root_bitmap_word_count;
    size_t block_count;
    size_t block_capture_count;
    size_t block_root_map_count;
    size_t global_count;
    size_t string_literal_count;
    size_t string_literal_bytes;
    size_t runtime_class_count;
    size_t runtime_shape_count;
    size_t string_bytes;
    bool has_method_code;
} st_image_emit_result_t;

void st_image_emit_result_init(st_image_emit_result_t *result);

/* Destroys the generated module.  The Anvil context supplied to emit must
 * therefore outlive this result. */
void st_image_emit_result_destroy(st_image_emit_result_t *result);

/* Emits immutable, target-laid-out metadata into a newly-created Anvil
 * module.  Source bodies are not lowered here. `options->selectors` is
 * required and must remain alive for this call. Failure is transactional:
 * result owns no module and exposes no partial counts.  bundle, graph, and all
 * of their AST storage must remain live only for the duration of this call. */
st_image_emit_status_t st_image_emit_metadata(
    st_image_emit_result_t *result,
    anvil_ctx_t *context,
    const st_source_bundle_t *bundle,
    const st_class_graph_result_t *graph,
    const st_image_emit_options_t *options);

const char *st_image_emit_status_string(st_image_emit_status_t status);

#endif
