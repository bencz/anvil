#ifndef ANVIL_SMALLTALK_DNU_H
#define ANVIL_SMALLTALK_DNU_H

#include "st_aot_bootstrap.h"
#include "st_send_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_DNU_CONTEXT_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_DNU_OK = 0,
    ST_DNU_ERR_INVALID_ARGUMENT,
    ST_DNU_ERR_INVALID_STATE,
    ST_DNU_ERR_INVALID_METADATA,
    ST_DNU_ERR_INVALID_LAYOUT,
    ST_DNU_ERR_INVALID_FRAME,
    ST_DNU_ERR_INVALID_SEND,
    ST_DNU_ERR_OUT_OF_MEMORY,
    ST_DNU_ERR_HEAP,
    ST_DNU_ERR_LOOKUP,
    ST_DNU_ERR_MISSING_HANDLER,
    ST_DNU_ERR_INVALID_HANDLER,
    ST_DNU_ERR_REENTRANT,
    ST_DNU_ERR_CONFLICT
} st_dnu_status_t;

typedef struct {
    const st_image_metadata_descriptor_t *metadata;
    st_image_runtime_t *image;
    st_lookup_context_t *lookup;
    const st_aot_bootstrap_context_t *bootstrap;

    /* Every identity and physical slot is supplied by the AOT driver's
     * authoritative graph/layout plan. No class, selector or ivar name is
     * rediscovered by the runtime. */
    uint32_t message_entity_id;
    uint32_t message_class_id;
    uint32_t message_shape_id;
    uint32_t message_selector_slot;
    uint32_t message_arguments_slot;
    uint32_t array_entity_id;
    uint32_t array_class_id;
    uint32_t array_shape_id;
    uint32_t does_not_understand_selector_id;
    st_primitive_allocator_t allocator;
} st_dnu_context_options_t;

typedef struct {
    uint32_t abi_version;
    bool initialized;
    bool active;
    const st_image_metadata_descriptor_t *metadata;
    st_image_runtime_t *image;
    st_lookup_context_t *lookup;
    const st_aot_bootstrap_context_t *bootstrap;
    st_heap_t *heap;
    const st_value_t *selector_symbols;
    size_t selector_count;
    uint32_t message_class_id;
    uint32_t message_shape_id;
    uint32_t message_selector_slot;
    uint32_t message_arguments_slot;
    uint32_t array_class_id;
    uint32_t array_shape_id;
    st_primitive_allocator_t allocator;
    st_send_site_t does_not_understand_site;
    st_aot_thread_t *attached_thread;
} st_dnu_context_t;

/* `context` must be zero-initialized. Initialization only authenticates
 * borrowed image/bootstrap/layout state and initializes the data-only DNU PIC.
 * Attach is a distinct quiescent lifecycle
 * step: the AOT thread must already be initialized and attached to the exact
 * image, with no pre-existing failure policy. Detach must precede thread or
 * context destruction, so failure_user can never dangle. */
st_dnu_status_t st_dnu_context_init(
    st_dnu_context_t *context,
    const st_dnu_context_options_t *options);
st_dnu_status_t st_dnu_context_attach(
    st_dnu_context_t *context, st_aot_thread_t *thread);
st_dnu_status_t st_dnu_context_detach(
    st_dnu_context_t *context, st_aot_thread_t *thread);
st_dnu_status_t st_dnu_context_destroy(st_dnu_context_t *context);

/* Constructs the exact language Message without publishing a partial result.
 * The mutable Array is an authenticated copy of argv and both objects are
 * filled through heap write-barrier APIs. Allocation does not invoke
 * Smalltalk or GC; on failure, any unreachable partial allocation remains
 * ordinary GC-owned heap storage and message_out stays ST_VALUE_INVALID. */
st_dnu_status_t st_dnu_message_create(
    st_dnu_context_t *context, StFrame *caller,
    const st_send_site_t *original_site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_value_t *message_out);

/* Exact st_aot_send_failure_fn installed by attach. It accepts only a genuine
 * NOT_FOUND miss. Every build/lookup/control/OOM failure is explicitly fatal;
 * it never returns nil or recursively retries a missing DNU handler. */
st_value_t st_dnu_send_failure(
    void *user, StFrame *caller, const st_send_site_t *site,
    st_value_t receiver, const st_value_t *argv, uint32_t argc,
    st_aot_send_status_t status);

const char *st_dnu_status_string(st_dnu_status_t status);

#ifdef __cplusplus
}
#endif

#endif
