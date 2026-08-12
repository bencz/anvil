#ifndef ANVIL_SMALLTALK_LOWER_H
#define ANVIL_SMALLTALK_LOWER_H

#include "anvil/anvil.h"
#include "st_class_graph.h"
#include "st_closure_bridge.h"
#include "st_primitive.h"
#include "st_selector.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_LOWER_OK = 0,
    ST_LOWER_ERR_INVALID_ARGUMENT,
    ST_LOWER_ERR_OUT_OF_MEMORY,
    ST_LOWER_ERR_OVERFLOW,
    ST_LOWER_ERR_UNSUPPORTED,
    ST_LOWER_ERR_IR_BUILD,
    ST_LOWER_ERR_VERIFY
} st_lower_status_t;

typedef enum {
    ST_LOWER_DIAG_NONE = 0,
    ST_LOWER_DIAG_INVALID_INPUT,
    ST_LOWER_DIAG_UNSUPPORTED_TARGET,
    ST_LOWER_DIAG_UNSUPPORTED_PRIMITIVE,
    ST_LOWER_DIAG_UNSUPPORTED_NODE,
    ST_LOWER_DIAG_UNSUPPORTED_SEND,
    ST_LOWER_DIAG_UNSUPPORTED_BINDING,
    ST_LOWER_DIAG_LITERAL_OUT_OF_RANGE,
    ST_LOWER_DIAG_IR_BUILD,
    ST_LOWER_DIAG_IR_VERIFY
} st_lower_diagnostic_code_t;

typedef struct {
    st_lower_diagnostic_code_t code;
    st_source_span_t span;
    st_ast_kind_t node_kind;
    st_ast_string_t detail;
    bool has_span;
} st_lower_diagnostic_t;

typedef void *(*st_lower_allocate_fn)(void *user, size_t size);
typedef void (*st_lower_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_lower_allocate_fn allocate;
    st_lower_deallocate_fn deallocate;
    void *user;
} st_lower_allocator_t;

/* Exact bridge from semantic global identity to the final dense image slot.
 * Entries are strictly ordered by semantic_external_id; both columns must be
 * unique.  runtime_index is the zero-based operand of
 * st_image_runtime_global_load().  Names are intentionally absent: lowering
 * never guesses an image slot from source spelling. */
typedef struct {
    uint32_t semantic_external_id;
    uint32_t runtime_index;
} st_lower_global_binding_t;

typedef struct {
    /* Required, NUL-terminated assembler/linker symbol. */
    const char *symbol_name;
    anvil_linkage_t linkage;
    /* Required only when the method contains a non-intrinsic send.  The table
     * must be frozen so selector IDs are immutable AOT metadata. */
    const st_selector_table_t *selectors;
    /* Optional exact resolver binding for this method.  A pragma without its
     * matching binding is never guessed from spelling. */
    const st_primitive_binding_t *primitive_binding;
    const st_lower_global_binding_t *globals;
    size_t global_count;
    /* Dense runtime class ID for each graph entity (graph ID minus one).
     * Namespace entries are zero. Required when a graph contains namespaces;
     * namespace-free graphs may omit it because their IDs are already dense. */
    const uint32_t *runtime_class_ids_by_entity;
    size_t runtime_class_id_count;
    /* Final zero-based ID of the method's first literal.  The image driver
     * computes non-overlapping prefix sums before lowering any method. */
    uint32_t literal_base_index;
    st_lower_allocator_t allocator;
} st_lower_options_t;

/* Owned by st_lower_result_t.  The bitmap is canonical and may be shared by
 * multiple maps.  Image emission converts these records directly to immutable
 * st_root_map_t descriptor data; harnesses must not invent divergent maps. */
typedef struct {
    uint32_t safepoint_id;
    uint32_t root_count;
    size_t bitmap_word_count;
    const uint64_t *live_root_bitmap;
} st_lower_root_map_t;

typedef struct {
    const char *bytes;
    size_t length;
} st_lower_symbol_t;

/* Owned immutable source bytes and their final dense image-runtime ID. */
typedef struct {
    uint32_t literal_id;
    const unsigned char *bytes;
    size_t length;
} st_lower_string_literal_artifact_t;

/* Canonical, owned input to the final artifact emitter.  The block function
 * and descriptor symbol are already relocations in module; the emitter uses
 * these exact captures/maps after native layout is known to materialize the
 * immutable StMethodDescriptor and st_aot_block_descriptor_t. */
typedef struct {
    anvil_func_t *function;
    st_lower_symbol_t code_symbol;
    st_lower_symbol_t descriptor_symbol;
    st_lower_symbol_t method_descriptor_symbol;
    uint32_t lexical_ordinal;
    uint32_t arity;
    uint32_t flags;
    uint32_t method_flags;
    uint32_t required_root_capacity;
    const st_aot_capture_descriptor_t *captures;
    size_t capture_count;
    const st_lower_root_map_t *root_maps;
    size_t root_map_count;
} st_lower_block_artifact_t;

typedef struct {
    st_lower_status_t status;
    st_lower_diagnostic_t diagnostic;
    anvil_module_t *module;
    anvil_func_t *function;
    anvil_type_t *frame_type;
    anvil_type_t *method_type;
    uint32_t required_root_capacity;
    uint32_t safepoint_count;
    size_t send_site_count;
    const st_lower_root_map_t *root_maps;
    size_t root_map_count;
    uint32_t primitive_intrinsic_id;
    st_primitive_failure_policy_t primitive_failure_policy;
    uint32_t method_flags;
    const st_lower_block_artifact_t *blocks;
    size_t block_count;
    const st_lower_string_literal_artifact_t *string_literals;
    size_t string_literal_count;
    bool has_primitive;
    void *implementation;
} st_lower_result_t;

void st_lower_result_init(st_lower_result_t *result);
/* The result, and therefore its module, must be destroyed before its context. */
void st_lower_result_destroy(st_lower_result_t *result);

/*
 * Lowers exactly one graph method to the uniform 64-bit Smalltalk ABI:
 *
 *     st_value_t method(StFrame *frame)
 *
 * `sema` must be the successful semantic result for the selected graph method.
 * Construction is transactional.  On every error, including unsupported AST,
 * result->module and result->function remain NULL.  The supported baseline is
 * deliberately explicit: self/super reads, method arguments, temporaries,
 * assignment, method/block sequences, local returns, nil/booleans,
 * SmallInteger/Character immediates, non-escaping literal blocks used by
 * exact-Boolean ifTrue:/ifFalse:/ifTrue:ifFalse: sends, and ordinary AOT
 * message chains through fixed selector IDs plus data-only PIC sites.  General
 * sends call the runtime bridge for lookup, construct a complete child frame,
 * and perform a typed indirect st_value_t(StFrame *) call.  A method with an
 * exact primitive_binding calls the canonical core helper with independent
 * status/result storage: OK returns immediately, FALL_THROUGH enters its
 * Smalltalk body, and a violated CANNOT_FAIL contract aborts through the AOT
 * primitive bridge. Nothing else is silently approximated.
 *
 * Methods which may create a home context, or which can invoke another
 * Smalltalk activation, use the st_control sidecar.  Their result metadata
 * carries ST_METHOD_CAN_UNWIND and, for inline block returns,
 * ST_METHOD_HAS_NON_LOCAL_RETURN.  Every language return converges on one
 * scope-leave epilogue; its dedicated live root and safepoint map are included
 * in required_root_capacity/root_maps.  A pending NLR observed after a call is
 * rooted and propagated before any continuation is evaluated.
 *
 * One proven escaping literal block is lowered as a real AOT function in the
 * same module.  Its owned block artifact contains the exact captures, root
 * maps, flags, and deterministic `(method_id, lexical_ordinal)` symbols.  The
 * descriptor remains an obligatory external relocation because native code
 * size/unwind offsets are final-artifact facts.  Callers must materialize it
 * from this artifact before linking; an unresolved symbol is intentional, not
 * a runtime fallback.  The current complete subset is zero/one argument,
 * VALUE/SELF captures, implicit local return, caret NLR, and proven direct
 * value/value: invocation. CELL captures, nested closures, and unproven
 * dynamic closure receivers remain transactionally unsupported.
 *
 * The frame is an internal compiler/runtime ABI object: callers must provide a
 * non-NULL frame and exactly the method arity, with a non-NULL argv for nonzero
 * arity.  Dispatch/link verification is responsible for enforcing that
 * precondition before entering generated methods.
 */
st_lower_status_t st_lower_method(
    st_lower_result_t *result, anvil_ctx_t *ctx,
    const st_class_graph_result_t *graph,
    st_class_graph_method_id_t method_id,
    const st_sema_result_t *sema,
    const st_lower_options_t *options);

const char *st_lower_status_string(st_lower_status_t status);
const char *st_lower_diagnostic_string(st_lower_diagnostic_code_t code);

#ifdef __cplusplus
}
#endif

#endif
