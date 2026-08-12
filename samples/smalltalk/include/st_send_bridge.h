#ifndef ANVIL_SMALLTALK_SEND_BRIDGE_H
#define ANVIL_SMALLTALK_SEND_BRIDGE_H

#include "st_lookup.h"
#include "st_heap_primitives.h"
#include "st_control.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_AOT_THREAD_ABI_VERSION UINT32_C(9)
#define ST_AOT_MAX_DYNAMIC_ROOTS UINT32_C(4096)

typedef struct st_aot_closure_context st_aot_closure_context_t;
typedef struct st_image_runtime st_image_runtime_t;
typedef struct st_stream_primitive_context st_stream_primitive_context_t;
typedef struct st_string_primitive_context st_string_primitive_context_t;
typedef struct st_numeric_context st_numeric_context_t;
typedef struct st_reflection_context st_reflection_context_t;

typedef enum {
    ST_AOT_SEND_OK = 0,
    ST_AOT_SEND_ERR_INVALID_ARGUMENT,
    ST_AOT_SEND_ERR_INVALID_THREAD,
    ST_AOT_SEND_ERR_INVALID_FRAME,
    ST_AOT_SEND_ERR_INVALID_RECEIVER,
    ST_AOT_SEND_ERR_LOOKUP,
    ST_AOT_SEND_ERR_NOT_FOUND,
    ST_AOT_SEND_ERR_ARITY,
    ST_AOT_SEND_ERR_INVALID_TARGET,
    ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME
} st_aot_send_status_t;

typedef enum {
    ST_AOT_IMMEDIATE_NIL = 0,
    ST_AOT_IMMEDIATE_FALSE,
    ST_AOT_IMMEDIATE_TRUE,
    ST_AOT_IMMEDIATE_SMALL_INTEGER,
    ST_AOT_IMMEDIATE_CHARACTER,
    ST_AOT_IMMEDIATE_CLASS_COUNT
} st_aot_immediate_class_t;

/* Object classification is a heap-provenance operation, not a tag decode.
 * The callback must authenticate allocation membership/liveness before
 * returning the object's class ID.  The bridge never dereferences an object
 * value itself. */
typedef bool (*st_aot_object_class_fn)(void *user, st_value_t value,
                                       uint32_t *class_id_out);

typedef st_value_t (*st_aot_send_failure_fn)(
    void *user, StFrame *caller, const st_send_site_t *site,
    st_value_t receiver, const st_value_t *argv, uint32_t argc,
    st_aot_send_status_t status);

typedef struct {
    uint32_t abi_version;
    st_lookup_context_t *lookup;
    uint32_t immediate_class_ids[ST_AOT_IMMEDIATE_CLASS_COUNT];
    st_heap_primitive_context_t *heap_primitives;
    st_control_thread_t *control;
    st_aot_closure_context_t *closures;
    st_image_runtime_t *image;
    st_stream_primitive_context_t *streams;
    st_string_primitive_context_t *strings;
    st_numeric_context_t *numeric;
    st_reflection_context_t *reflection;
    st_aot_object_class_fn object_class;
    void *object_class_user;
    st_aot_send_failure_fn failure;
    void *failure_user;
    bool initialized;
} st_aot_thread_t;

typedef struct {
    st_method_code_t code;
    const StMethodDescriptor *descriptor;
    uint32_t frame_root_capacity;
    uint32_t flags;
} st_aot_send_target_t;

/* The lookup context, optional heap-primitive/control contexts, and all
 * callbacks are borrowed and must outlive thread.  A non-NULL control sidecar
 * must have been initialized with this exact `thread` address as its frame
 * identity; the pointer is immutable after initialization.
 * Every immediate class ID is explicit image configuration and must name a
 * distinct descriptor.  No manifest ordering is assumed.  Destroy requires
 * quiescence: it must not race with frame validation, lookup, or failure
 * callbacks/heap primitive execution, and no live frame may retain the thread
 * afterward.  NULL explicitly configures each optional sidecar as
 * unavailable. */
bool st_aot_thread_init(
    st_aot_thread_t *thread, st_lookup_context_t *lookup,
    const uint32_t immediate_class_ids[ST_AOT_IMMEDIATE_CLASS_COUNT],
    st_heap_primitive_context_t *heap_primitives,
    st_control_thread_t *control,
    st_aot_closure_context_t *closures,
    st_aot_object_class_fn object_class, void *object_class_user,
    st_aot_send_failure_fn failure, void *failure_user);
void st_aot_thread_destroy(st_aot_thread_t *thread);

/* The image sidecar is explicit per-thread state, never a C global. Attach
 * and detach require quiescence and exact pointer identity. */
bool st_aot_thread_image_attach(st_aot_thread_t *thread,
                                st_image_runtime_t *image);
bool st_aot_thread_image_detach(st_aot_thread_t *thread,
                                const st_image_runtime_t *image);
bool st_aot_thread_streams_attach(st_aot_thread_t *thread,
                                  st_stream_primitive_context_t *streams);
bool st_aot_thread_streams_detach(
    st_aot_thread_t *thread, const st_stream_primitive_context_t *streams);
bool st_aot_thread_strings_attach(
    st_aot_thread_t *thread, st_string_primitive_context_t *strings);
bool st_aot_thread_strings_detach(
    st_aot_thread_t *thread, const st_string_primitive_context_t *strings);
bool st_aot_thread_numeric_attach(st_aot_thread_t *thread,
                                  st_numeric_context_t *numeric);
bool st_aot_thread_numeric_detach(
    st_aot_thread_t *thread, const st_numeric_context_t *numeric);
bool st_aot_thread_reflection_attach(
    st_aot_thread_t *thread, st_reflection_context_t *reflection);
bool st_aot_thread_reflection_detach(
    st_aot_thread_t *thread, const st_reflection_context_t *reflection);

/* Validate the caller frame before generated code touches its root vector.
 * The active method descriptor must describe the actual arity and provide at
 * least required_root_capacity slots.  A nonzero safepoint must name a root
 * map whose root_count does not exceed frame->root_count.  The frame,
 * descriptor, root map and vectors are borrowed and must remain live for the
 * full call. */
st_aot_send_status_t st_aot_frame_validate(
    StFrame *frame, uint32_t required_root_capacity);

/* Resolve only.  The generated AOT caller constructs the child StFrame and
 * performs the typed indirect st_method_code_t call. */
st_aot_send_status_t st_aot_send_resolve(
    StFrame *caller, st_send_site_t *site, st_value_t receiver,
    uint32_t arity, st_aot_send_target_t *target_out);

/* Initialize descriptor-sized dynamic child roots before publishing the child
 * frame. Resolver guarantees count <= ST_AOT_MAX_DYNAMIC_ROOTS. */
st_aot_send_status_t st_aot_frame_roots_initialize(
    st_value_t *roots, uint32_t count);

/* Explicit language-level miss policy. Only a genuine, already validated
 * ST_AOT_SEND_ERR_NOT_FOUND may enter the callback. Lookup corruption, arity
 * mismatch, invalid bindings, unsupported frame protocols and every ABI or
 * value-authentication failure are fatal before policy code can allocate or
 * trigger GC. Missing policy or an unauthenticated result is likewise fatal;
 * the bridge never substitutes nil/false/zero. */
st_value_t st_aot_send_failure(
    StFrame *caller, const st_send_site_t *site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_aot_send_status_t status);

const char *st_aot_send_status_string(st_aot_send_status_t status);

#ifdef __cplusplus
}
#endif

#endif
