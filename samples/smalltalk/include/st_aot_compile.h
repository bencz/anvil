#ifndef ANVIL_SMALLTALK_AOT_COMPILE_H
#define ANVIL_SMALLTALK_AOT_COMPILE_H

#include "st_image_emit.h"
#include "st_lower.h"

#include <anvil/anvil_opt.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_AOT_SYMBOL_PREFIX_MAX 128u

typedef enum {
    ST_AOT_COMPILE_OK = 0,
    ST_AOT_COMPILE_ERR_INVALID_ARGUMENT,
    ST_AOT_COMPILE_ERR_OUT_OF_MEMORY,
    ST_AOT_COMPILE_ERR_OVERFLOW,
    ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET,
    ST_AOT_COMPILE_ERR_PRIMITIVES,
    ST_AOT_COMPILE_ERR_SEMANTIC,
    ST_AOT_COMPILE_ERR_LAYOUT,
    ST_AOT_COMPILE_ERR_LOWERING,
    ST_AOT_COMPILE_ERR_METADATA
} st_aot_compile_status_t;

typedef enum {
    ST_AOT_DIAG_INPUT = 0,
    ST_AOT_DIAG_PRIMITIVE,
    ST_AOT_DIAG_SEMA_VIEW,
    ST_AOT_DIAG_SEMANTIC,
    ST_AOT_DIAG_LAYOUT,
    ST_AOT_DIAG_LOWERING,
    ST_AOT_DIAG_METADATA
} st_aot_diagnostic_stage_t;

typedef struct {
    st_aot_diagnostic_stage_t stage;
    st_class_graph_method_id_t method_id;
    size_t unit_index;
    st_source_span_t span;
    bool has_method;
    bool has_span;

    /* Stage-specific status/code.  Unused fields are zero. */
    st_primitive_diagnostic_code_t primitive_code;
    st_class_graph_status_t graph_status;
    st_sema_status_t sema_status;
    st_sema_diagnostic_code_t sema_code;
    st_image_layout_status_t layout_status;
    st_lower_status_t lower_status;
    st_lower_diagnostic_code_t lower_code;
    st_image_emit_status_t image_status;

    /* Owned, NUL-terminated copies. */
    char *source_name;
    size_t source_name_length;
    char *detail;
    size_t detail_length;
} st_aot_diagnostic_t;

typedef void *(*st_aot_allocate_fn)(void *user, size_t size);
typedef void (*st_aot_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_aot_allocate_fn allocate;
    st_aot_deallocate_fn deallocate;
    void *user;
} st_aot_allocator_t;

/* Bootstrap globals not declared by the class graph (for example
 * Transcript).  The name participates only in semantic resolution and
 * metadata; lowering consumes semantic_external_id, never spelling. */
typedef struct {
    const char *name;
    size_t name_length;
    uint32_t semantic_external_id;
    uint32_t runtime_index;
} st_aot_external_global_t;

typedef struct {
    const st_source_bundle_t *bundle;
    const st_class_graph_result_t *graph;
    const st_selector_table_t *selectors;
    const st_primitive_result_t *primitives;
    const st_aot_external_global_t *external_globals;
    size_t external_global_count;
    anvil_arch_t target;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    anvil_opt_level_t optimization;

    /* Required NUL-terminated portable C/linker identifier and its exact byte
     * length, excluding that terminator. */
    const char *symbol_prefix;
    size_t symbol_prefix_length;
    st_aot_allocator_t allocator;
} st_aot_compile_options_t;

typedef struct {
    st_class_graph_method_id_t method_id;
    st_class_graph_id_t owner;
    uint32_t arity;
    char *selector;
    size_t selector_length;
    char *symbol;
    size_t symbol_length;
    st_lower_result_t lowering;

    /* The adapter records are owned here.  Their bitmap pointers borrow the
     * canonical storage in `lowering` and are valid until result destruction. */
    st_image_root_map_metadata_t *root_maps;
    st_image_aot_block_artifact_t *block_artifacts;
    st_aot_capture_descriptor_t *block_captures;
    size_t block_capture_count;
    st_image_root_map_metadata_t *block_root_maps;
    size_t block_root_map_count;
    st_image_string_literal_artifact_t *string_literals;
    st_image_aot_method_artifact_t artifact;
} st_aot_method_result_t;

typedef struct {
    uint32_t semantic_external_id;
    uint32_t runtime_index;
    char *name;
    size_t name_length;
} st_aot_global_result_t;

typedef struct {
    anvil_arch_t target;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    anvil_opt_level_t optimization;
    /* Owned, NUL-terminated exact copy of the compile input. */
    char *symbol_prefix;
    size_t symbol_prefix_length;
} st_aot_compile_provenance_t;

typedef struct {
    st_aot_compile_status_t status;

    /* Canonical configuration published atomically with successful modules.
     * It remains neutral/empty on every failure. */
    st_aot_compile_provenance_t provenance;

    /* Published together with provenance only after every method and metadata
     * ABI v5 succeed. */
    anvil_ctx_t *context;
    st_aot_method_result_t *methods;
    size_t method_count;
    st_aot_global_result_t *globals;
    size_t global_count;
    size_t string_literal_count;
    st_image_layout_result_t layout;
    st_image_emit_result_t metadata;

    /* On failure, the fields above remain empty and only these owned
 * diagnostics may be published. provenance also remains neutral/empty. */
    st_aot_diagnostic_t *diagnostics;
    size_t diagnostic_count;
    void *implementation;
} st_aot_compile_result_t;

void st_aot_compile_result_init(st_aot_compile_result_t *result);
void st_aot_compile_result_destroy(st_aot_compile_result_t *result);

/*
 * Transactionally compiles a complete, already-built image/application
 * graph.  The input bundle, graph, frozen selector table and primitive result
 * are borrowed.  On success the result owns the context, every method module,
 * the metadata-v5 module, symbols, layout plan and root-map adapters.
 * Destruction order is
 * metadata module, method modules, adapters/symbols, then context.
 *
 * The current Smalltalk frame/value ABI is deliberately 64-bit.  A target
 * whose Anvil data layout has another pointer width is rejected before any
 * method work; the driver never silently changes the requested target.
 */
st_aot_compile_status_t st_aot_compile(
    st_aot_compile_result_t *result,
    const st_aot_compile_options_t *options);

const char *st_aot_compile_status_string(st_aot_compile_status_t status);
const char *st_aot_diagnostic_stage_string(st_aot_diagnostic_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif
