#include "st_send_bridge.h"
#include "st_reflection_primitives.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(st_aot_send_target_t) == 24u,
               "AOT send target is part of the generated-code ABI");
_Static_assert(offsetof(st_aot_send_target_t, code) == 0u,
               "AOT send target code offset changed");
_Static_assert(offsetof(st_aot_send_target_t, descriptor) == 8u,
               "AOT send target descriptor offset changed");
_Static_assert(offsetof(st_aot_send_target_t, frame_root_capacity) == 16u,
               "AOT send target root-capacity offset changed");
_Static_assert(offsetof(st_aot_send_target_t, flags) == 20u,
               "AOT send target flags offset changed");

static bool lookup_is_ready(const st_lookup_context_t *lookup)
{
    return lookup != NULL && lookup->initialized && lookup->descriptors != NULL
        && lookup->class_epochs != NULL;
}

static bool thread_is_ready(const st_aot_thread_t *thread)
{
    return thread != NULL && thread->initialized
        && thread->abi_version == ST_AOT_THREAD_ABI_VERSION
        && lookup_is_ready(thread->lookup);
}

bool st_aot_thread_image_attach(st_aot_thread_t *thread,
                                st_image_runtime_t *image)
{
    if (!thread_is_ready(thread) || image == NULL || thread->image != NULL)
        return false;
    thread->image = image;
    return true;
}

bool st_aot_thread_image_detach(st_aot_thread_t *thread,
                                const st_image_runtime_t *image)
{
    if (!thread_is_ready(thread) || image == NULL || thread->image != image)
        return false;
    thread->image = NULL;
    return true;
}

bool st_aot_thread_streams_attach(st_aot_thread_t *thread,
                                  st_stream_primitive_context_t *streams)
{
    if (!thread_is_ready(thread) || streams == NULL
            || thread->streams != NULL)
        return false;
    thread->streams = streams;
    return true;
}

bool st_aot_thread_streams_detach(
    st_aot_thread_t *thread, const st_stream_primitive_context_t *streams)
{
    if (!thread_is_ready(thread) || streams == NULL
            || thread->streams != streams)
        return false;
    thread->streams = NULL;
    return true;
}

bool st_aot_thread_strings_attach(
    st_aot_thread_t *thread, st_string_primitive_context_t *strings)
{
    if (!thread_is_ready(thread) || strings == NULL
            || thread->strings != NULL) {
        return false;
    }
    thread->strings = strings;
    return true;
}

bool st_aot_thread_strings_detach(
    st_aot_thread_t *thread, const st_string_primitive_context_t *strings)
{
    if (!thread_is_ready(thread) || strings == NULL
            || thread->strings != strings) {
        return false;
    }
    thread->strings = NULL;
    return true;
}

bool st_aot_thread_numeric_attach(st_aot_thread_t *thread,
                                  st_numeric_context_t *numeric)
{
    if (!thread_is_ready(thread) || numeric == NULL
            || thread->numeric != NULL)
        return false;
    thread->numeric = numeric;
    return true;
}

bool st_aot_thread_numeric_detach(
    st_aot_thread_t *thread, const st_numeric_context_t *numeric)
{
    if (!thread_is_ready(thread) || numeric == NULL
            || thread->numeric != numeric)
        return false;
    thread->numeric = NULL;
    return true;
}

bool st_aot_thread_reflection_attach(
    st_aot_thread_t *thread, st_reflection_context_t *reflection)
{
    if (!thread_is_ready(thread) || reflection == NULL
            || thread->reflection != NULL || thread->image == NULL
            || !reflection->initialized
            || reflection->abi_version != ST_REFLECTION_CONTEXT_ABI_VERSION
            || reflection->state == NULL
            || reflection->image != thread->image
            || reflection->lookup != thread->lookup) {
        return false;
    }
    thread->reflection = reflection;
    return true;
}

bool st_aot_thread_reflection_detach(
    st_aot_thread_t *thread, const st_reflection_context_t *reflection)
{
    if (!thread_is_ready(thread) || reflection == NULL
            || thread->reflection != reflection) {
        return false;
    }
    thread->reflection = NULL;
    return true;
}

static const st_root_map_t *root_map_find(
    const StMethodDescriptor *descriptor, uint32_t safepoint_id)
{
    size_t low = 0u;
    size_t high = descriptor->root_map_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uint32_t candidate = descriptor->root_maps[middle].safepoint_id;
        if (candidate < safepoint_id) low = middle + 1u;
        else high = middle;
    }
    return low < descriptor->root_map_count
            && descriptor->root_maps[low].safepoint_id == safepoint_id
        ? &descriptor->root_maps[low] : NULL;
}

bool st_aot_thread_init(
    st_aot_thread_t *thread, st_lookup_context_t *lookup,
    const uint32_t immediate_class_ids[ST_AOT_IMMEDIATE_CLASS_COUNT],
    st_heap_primitive_context_t *heap_primitives,
    st_control_thread_t *control,
    st_aot_closure_context_t *closures,
    st_aot_object_class_fn object_class, void *object_class_user,
    st_aot_send_failure_fn failure, void *failure_user)
{
    size_t index;
    size_t other;
    if (thread == NULL || thread->initialized || !lookup_is_ready(lookup)
            || immediate_class_ids == NULL
            || (control != NULL
                && (control->_st_abi_version != ST_CONTROL_ABI_VERSION
                    || control->_st_thread_id == 0u
                    || control->_st_frame_thread_identity != thread
                    || !control->_st_allocator.allocate
                    || !control->_st_allocator.deallocate))) return false;
    for (index = 0u; index < ST_AOT_IMMEDIATE_CLASS_COUNT; index++) {
        uint32_t class_id = immediate_class_ids[index];
        if (class_id == 0u
                || st_runtime_class(lookup->descriptors, class_id) == NULL)
            return false;
        for (other = 0u; other < index; other++)
            if (immediate_class_ids[other] == class_id) return false;
    }
    memset(thread, 0, sizeof(*thread));
    thread->abi_version = ST_AOT_THREAD_ABI_VERSION;
    thread->lookup = lookup;
    memcpy(thread->immediate_class_ids, immediate_class_ids,
           sizeof(thread->immediate_class_ids));
    thread->heap_primitives = heap_primitives;
    thread->control = control;
    thread->closures = closures;
    thread->object_class = object_class;
    thread->object_class_user = object_class_user;
    thread->failure = failure;
    thread->failure_user = failure_user;
    thread->initialized = true;
    return true;
}

void st_aot_thread_destroy(st_aot_thread_t *thread)
{
    if (thread != NULL) memset(thread, 0, sizeof(*thread));
}

st_aot_send_status_t st_aot_frame_validate(
    StFrame *frame, uint32_t required_root_capacity)
{
    const StMethodDescriptor *descriptor;
    const st_root_map_t *map;
    st_aot_thread_t *thread;
    if (frame == NULL || frame->thread == NULL)
        return ST_AOT_SEND_ERR_INVALID_FRAME;
    thread = frame->thread;
    if (!thread_is_ready(thread)) return ST_AOT_SEND_ERR_INVALID_THREAD;
    descriptor = frame->method;
    if (!st_method_descriptor_is_valid(descriptor)
            || descriptor->arity != frame->argc
            || descriptor->frame_root_capacity < required_root_capacity
            || frame->root_count != descriptor->frame_root_capacity
            || (frame->argc != 0u && frame->argv == NULL)
            || (frame->root_count != 0u && frame->roots == NULL)
            || frame->flags != 0u)
        return ST_AOT_SEND_ERR_INVALID_FRAME;
    if (frame->safepoint_id == 0u) return ST_AOT_SEND_OK;
    map = root_map_find(descriptor, frame->safepoint_id);
    if (map == NULL || map->root_count > frame->root_count)
        return ST_AOT_SEND_ERR_INVALID_FRAME;
    return ST_AOT_SEND_OK;
}

static st_aot_send_status_t receiver_class(
    const st_aot_thread_t *thread, st_value_t receiver,
    uint32_t *class_id_out)
{
    st_value_kind_t kind = st_value_kind(receiver);
    uint32_t class_id;
    switch (kind) {
    case ST_VALUE_NIL:
        class_id = thread->immediate_class_ids[ST_AOT_IMMEDIATE_NIL];
        break;
    case ST_VALUE_FALSE:
        class_id = thread->immediate_class_ids[ST_AOT_IMMEDIATE_FALSE];
        break;
    case ST_VALUE_TRUE:
        class_id = thread->immediate_class_ids[ST_AOT_IMMEDIATE_TRUE];
        break;
    case ST_VALUE_SMALL_INTEGER:
        class_id = thread->immediate_class_ids[ST_AOT_IMMEDIATE_SMALL_INTEGER];
        break;
    case ST_VALUE_CHARACTER:
        class_id = thread->immediate_class_ids[ST_AOT_IMMEDIATE_CHARACTER];
        break;
    case ST_VALUE_OBJECT:
        class_id = 0u;
        if (thread->object_class == NULL
                || !thread->object_class(thread->object_class_user, receiver,
                                         &class_id))
            return ST_AOT_SEND_ERR_INVALID_RECEIVER;
        break;
    default:
        return ST_AOT_SEND_ERR_INVALID_RECEIVER;
    }
    if (class_id == 0u
            || st_runtime_class(thread->lookup->descriptors, class_id) == NULL)
        return ST_AOT_SEND_ERR_INVALID_RECEIVER;
    *class_id_out = class_id;
    return ST_AOT_SEND_OK;
}

st_aot_send_status_t st_aot_send_resolve(
    StFrame *caller, st_send_site_t *site, st_value_t receiver,
    uint32_t arity, st_aot_send_target_t *target_out)
{
    st_aot_thread_t *thread;
    st_lookup_result_t lookup_result;
    st_lookup_status_t lookup_status;
    st_aot_send_status_t status;
    uint32_t class_id;
    bool cache_hit;
    if (target_out != NULL) memset(target_out, 0, sizeof(*target_out));
    if (caller == NULL || site == NULL || target_out == NULL
            || !site->initialized || site->selector_id == 0u)
        return ST_AOT_SEND_ERR_INVALID_ARGUMENT;
    status = st_aot_frame_validate(caller, 0u);
    if (status != ST_AOT_SEND_OK) return status;
    thread = caller->thread;
    if (!thread_is_ready(thread)) return ST_AOT_SEND_ERR_INVALID_THREAD;
    status = receiver_class(thread, receiver, &class_id);
    if (status != ST_AOT_SEND_OK) return status;
    lookup_status = st_send_site_resolve(
        thread->lookup, site, class_id, NULL, NULL, &lookup_result,
        &cache_hit);
    (void)cache_hit;
    if (lookup_status == ST_LOOKUP_NOT_FOUND) return ST_AOT_SEND_ERR_NOT_FOUND;
    if (lookup_status != ST_LOOKUP_FOUND) return ST_AOT_SEND_ERR_LOOKUP;
    if (lookup_result.binding == NULL
            || !st_method_binding_is_valid(lookup_result.binding)
            || lookup_result.binding->descriptor->selector_id
                != site->selector_id)
        return ST_AOT_SEND_ERR_INVALID_TARGET;
    const StMethodDescriptor *descriptor = lookup_result.binding->descriptor;
    if (descriptor->arity != arity) return ST_AOT_SEND_ERR_ARITY;
    if (descriptor->frame_root_capacity > ST_AOT_MAX_DYNAMIC_ROOTS)
        return ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME;
    /* Root storage is a frame-layout/GC concern, not an unwind concern.
     * Requiring a control scope for an otherwise ordinary root-bearing
     * callee would make perfectly valid leaf/runtime methods undispatchable.
     * Only methods that can actually publish control state require the
     * caller to be the authenticated top scope. */
    if ((descriptor->flags & (ST_METHOD_CAN_UNWIND
                              | ST_METHOD_HAS_NON_LOCAL_RETURN)) != 0u
            && (thread->control == NULL
                || thread->control->_st_abi_version != ST_CONTROL_ABI_VERSION
                || thread->control->_st_frame_thread_identity != thread
                || thread->control->_st_top_scope == NULL
                || thread->control->_st_top_scope->_st_frame != caller))
        return ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME;
    target_out->code = lookup_result.binding->code;
    target_out->descriptor = descriptor;
    target_out->frame_root_capacity = descriptor->frame_root_capacity;
    target_out->flags = descriptor->flags;
    return ST_AOT_SEND_OK;
}

st_aot_send_status_t st_aot_frame_roots_initialize(
    st_value_t *roots, uint32_t count)
{
    if (count > ST_AOT_MAX_DYNAMIC_ROOTS || (count != 0u && roots == NULL))
        return ST_AOT_SEND_ERR_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < count; index++)
        roots[index] = st_value_nil();
    return ST_AOT_SEND_OK;
}

st_value_t st_aot_send_failure(
    StFrame *caller, const st_send_site_t *site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_aot_send_status_t status)
{
    st_aot_thread_t *thread;
    st_value_t result;
    uint32_t authenticated_class;
    /* A lookup subsystem failure or a structurally invalid target is not a
     * language message miss. In particular, arity mismatches must never be
     * disguised as doesNotUnderstand:. */
    if (status != ST_AOT_SEND_ERR_NOT_FOUND) {
        abort();
    }
    if (caller == NULL || st_aot_frame_validate(caller, 0u) != ST_AOT_SEND_OK
            || (thread = caller->thread) == NULL
            || !thread_is_ready(thread) || thread->failure == NULL
            || site == NULL || !site->initialized
            || site->selector_id == 0u
            || (argc != 0u && argv == NULL)
            || receiver_class(thread, receiver, &authenticated_class)
                != ST_AOT_SEND_OK) {
        abort();
    }
    for (uint32_t index = 0u; index < argc; index++) {
        if (receiver_class(thread, argv[index], &authenticated_class)
                != ST_AOT_SEND_OK) {
            abort();
        }
    }
    result = thread->failure(thread->failure_user, caller, site, receiver,
                             argv, argc, status);
    if (receiver_class(thread, result, &authenticated_class)
            != ST_AOT_SEND_OK)
        abort();
    return result;
}

const char *st_aot_send_status_string(st_aot_send_status_t status)
{
    switch (status) {
    case ST_AOT_SEND_OK: return "ok";
    case ST_AOT_SEND_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_AOT_SEND_ERR_INVALID_THREAD: return "invalid AOT thread";
    case ST_AOT_SEND_ERR_INVALID_FRAME: return "invalid Smalltalk frame";
    case ST_AOT_SEND_ERR_INVALID_RECEIVER: return "invalid receiver";
    case ST_AOT_SEND_ERR_LOOKUP: return "method lookup failed";
    case ST_AOT_SEND_ERR_NOT_FOUND: return "method not found";
    case ST_AOT_SEND_ERR_ARITY: return "method arity mismatch";
    case ST_AOT_SEND_ERR_INVALID_TARGET: return "invalid method target";
    case ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME:
        return "callee frame protocol is not supported";
    default: return "invalid AOT send status";
    }
}
