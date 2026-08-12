#ifndef ANVIL_SMALLTALK_CLOSURE_BRIDGE_H
#define ANVIL_SMALLTALK_CLOSURE_BRIDGE_H

#include "st_heap.h"
#include "st_send_bridge.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_AOT_BLOCK_ABI_VERSION UINT32_C(2)

typedef enum {
    ST_AOT_CAPTURE_VALUE = 0,
    ST_AOT_CAPTURE_CELL,
    ST_AOT_CAPTURE_SELF
} st_aot_capture_kind_t;

typedef enum {
    ST_AOT_BLOCK_HAS_HOME = UINT32_C(1) << 0,
    ST_AOT_BLOCK_HAS_CELLS = UINT32_C(1) << 1,
    ST_AOT_BLOCK_FLAGS_MASK = ST_AOT_BLOCK_HAS_HOME
                            | ST_AOT_BLOCK_HAS_CELLS
} st_aot_block_flags_t;

typedef struct {
    uint32_t binding_id;
    uint32_t kind;
} st_aot_capture_descriptor_t;

/* Immutable image-lifetime metadata. `code` and `method` are ordinary AOT
 * relocations.  method owns the exact root maps used by the generated block
 * function.  Context initialization authenticates descriptors once and keeps
 * a private sorted registry; bytes read from a heap object are never trusted
 * as a descriptor until pointer membership succeeds. */
typedef struct {
    uint32_t abi_version;
    uint32_t arity;
    uint32_t capture_count;
    uint32_t flags;
    st_method_code_t code;
    const StMethodDescriptor *method;
    const st_aot_capture_descriptor_t *captures;
    size_t capture_descriptor_count;
} st_aot_block_descriptor_t;

typedef enum {
    ST_AOT_CLOSURE_OK = 0,
    ST_AOT_CLOSURE_ERR_INVALID_ARGUMENT,
    ST_AOT_CLOSURE_ERR_INVALID_CONTEXT,
    ST_AOT_CLOSURE_ERR_INVALID_FRAME,
    ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR,
    ST_AOT_CLOSURE_ERR_INVALID_CLOSURE,
    ST_AOT_CLOSURE_ERR_WRONG_ARITY,
    ST_AOT_CLOSURE_ERR_INVALID_CAPTURE,
    ST_AOT_CLOSURE_ERR_OUT_OF_MEMORY,
    ST_AOT_CLOSURE_ERR_HOME_REQUIRED,
    ST_AOT_CLOSURE_ERR_HOME_RETURNED,
    ST_AOT_CLOSURE_ERR_BLOCK_RETURNED,
    ST_AOT_CLOSURE_ERR_UNSUPPORTED,
    ST_AOT_CLOSURE_ERR_BUSY,
    ST_AOT_CLOSURE_ERR_HEAP
} st_aot_closure_status_t;

typedef struct {
    st_method_code_t code;
    const StMethodDescriptor *method;
    StHomeToken *home;
    uint32_t frame_root_capacity;
    uint32_t flags;
    uint32_t capture_count;
} st_aot_closure_target_t;

typedef void *(*st_aot_closure_allocate_fn)(void *user, size_t size);
typedef void (*st_aot_closure_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_heap_t *heap;
    uint32_t closure_class_id;
    uint32_t closure_shape_id;
    uint32_t cell_class_id;
    uint32_t cell_shape_id;
    /* Optional exact Array layout used by valueWithArguments:.  Both IDs are
     * zero when that protocol is deliberately unavailable. */
    uint32_t argument_array_class_id;
    uint32_t argument_array_shape_id;
    const st_aot_block_descriptor_t *const *descriptors;
    size_t descriptor_count;
    st_aot_closure_allocate_fn allocate;
    st_aot_closure_deallocate_fn deallocate;
    void *allocator_user;
} st_aot_closure_options_t;

typedef struct st_aot_closure_state st_aot_closure_state_t;
struct st_aot_closure_context {
    st_aot_closure_state_t *state;
};

/* Init installs a precise post-sweep observer on heap. Descriptors and their
 * transitive metadata are borrowed and immutable. Destroy requires quiescence,
 * and removes the observer before returning; heap and control sidecar must
 * still be alive.
 * Destroy returns BUSY while any closure remains live: clients must first run
 * a collection with the intended final root set. */
st_aot_closure_status_t st_aot_closure_context_init(
    st_aot_closure_context_t *context,
    const st_aot_closure_options_t *options);
st_aot_closure_status_t st_aot_closure_context_destroy(
    st_aot_closure_context_t *context);

/* `captures` is borrowed only for this call.  Every captured StValue and the
 * creating receiver must already be represented by the caller's active root
 * map for the full allocation call; an arbitrary unrooted C array is not a GC
 * root.  The current heap allocator does not collect recursively, but this is
 * an ABI precondition so a future allocating/collecting policy remains safe.
 * Generated lowering must publish the receiver/captures in scratch roots,
 * publish its safepoint, call this function, then clear both on all exits. */
st_aot_closure_status_t st_aot_closure_create(
    StFrame *frame, const st_aot_block_descriptor_t *descriptor,
    st_value_t lexical_self, const st_value_t *captures,
    uint32_t capture_count,
    st_value_t *closure_out);

/* Compiler-private mutable lexical storage.  Cell identity, class and shape
 * are authenticated against the exact context configuration.  Store crosses
 * the heap's fixed-reference mutation boundary and therefore always applies
 * the collector write barrier. */
st_aot_closure_status_t st_aot_closure_cell_create(
    StFrame *frame, st_value_t initial_value, st_value_t *cell_out);
st_aot_closure_status_t st_aot_closure_cell_load(
    StFrame *frame, st_value_t cell, st_value_t *value_out);
st_aot_closure_status_t st_aot_closure_cell_store(
    StFrame *frame, st_value_t cell, st_value_t value);

/* Resolve only. Generated AOT code allocates the descriptor-known child root
 * vector, constructs StFrame (including the captured home), then performs the
 * typed indirect call. No executable memory, VLA, or hot-path allocation is
 * performed here. */
st_aot_closure_status_t st_aot_closure_resolve(
    StFrame *caller, st_value_t closure, uint32_t arity,
    st_aot_closure_target_t *target_out);

st_aot_closure_status_t st_aot_closure_capture_load(
    StFrame *caller, st_value_t closure, uint32_t capture_index,
    st_value_t *value_out);

/* Invoke an authenticated closure through the uniform AOT method ABI.  The
 * bridge allocates and initializes the descriptor-sized child root vector,
 * publishes closure and arguments before the call, and never consumes a
 * pending non-local return.  A returned home is rejected before entering the
 * block, so BLOCK_RETURNED remains distinguishable from malformed closure
 * metadata. */
st_aot_closure_status_t st_aot_closure_invoke(
    StFrame *caller, st_value_t closure, const st_value_t *arguments,
    uint32_t argument_count, st_value_t *result_out);

/* Authenticate the exact configured Array representation and return its
 * borrowed, non-moving indexed storage.  Every element is validated as an
 * StValue and every object element must be a live member of the same heap.
 * The Array itself must remain in the caller's published roots throughout a
 * subsequent invocation because child.argv points directly into this
 * storage. Runtime-symbol primitive lowering satisfies this by keeping the
 * original receiver and argv live at its published safepoint. */
st_aot_closure_status_t st_aot_closure_argument_array_view(
    StFrame *caller, st_value_t array, const st_value_t **arguments_out,
    uint32_t *argument_count_out);

_Noreturn st_value_t st_aot_closure_contract_violation(
    st_aot_closure_status_t status, StFrame *frame);

const char *st_aot_closure_status_string(st_aot_closure_status_t status);

#ifdef __cplusplus
}
#endif

#endif
