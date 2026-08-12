#include "st_dnu.h"

#include <stdlib.h>
#include <string.h>

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static bool normalize_allocator(
    st_primitive_allocator_t input, st_primitive_allocator_t *output)
{
    if (output == NULL
            || ((input.allocate == NULL) != (input.deallocate == NULL))) {
        return false;
    }
    if (input.allocate == NULL) {
        input.allocate = default_allocate;
        input.deallocate = default_deallocate;
        input.user = NULL;
    }
    *output = input;
    return true;
}

static bool pointer_slot(const StShapeDescriptor *shape, size_t index)
{
    return shape != NULL
        && index < shape->fixed_word_count
        && shape->fixed_pointer_bitmap != NULL
        && index / 64u < shape->fixed_pointer_bitmap_word_count
        && (shape->fixed_pointer_bitmap[index / 64u]
            & (UINT64_C(1) << (index % 64u))) != 0u;
}

static const st_image_entity_metadata_t *entity_at(
    const st_image_metadata_descriptor_t *metadata, uint32_t entity_id)
{
    if (metadata == NULL || metadata->entities == NULL
            || entity_id == 0u || entity_id > metadata->entity_count) {
        return NULL;
    }
    const st_image_entity_metadata_t *entity =
        &metadata->entities[entity_id - 1u];
    return entity->id == entity_id ? entity : NULL;
}

static const st_image_selector_metadata_t *selector_at(
    const st_image_metadata_descriptor_t *metadata, uint32_t selector_id)
{
    if (metadata == NULL || metadata->selectors == NULL
            || selector_id == 0u || selector_id > metadata->selector_count) {
        return NULL;
    }
    const st_image_selector_metadata_t *selector =
        &metadata->selectors[selector_id - 1u];
    return selector->id == selector_id ? selector : NULL;
}

static bool entity_declares_slot(
    const st_image_metadata_descriptor_t *metadata,
    const st_image_entity_metadata_t *entity, uint32_t slot_index)
{
    if (metadata == NULL || entity == NULL
            || metadata->instance_slots == NULL
            || entity->instance_slot_offset > metadata->instance_slot_count
            || entity->instance_slot_count
                > metadata->instance_slot_count
                    - entity->instance_slot_offset) {
        return false;
    }
    for (size_t index = 0u; index < entity->instance_slot_count; index++) {
        const st_image_slot_metadata_t *slot = &metadata->instance_slots[
            entity->instance_slot_offset + index];
        if (slot->declaring_class == entity->id
                && slot->kind == ST_CLASS_GRAPH_INSTANCE_SLOT
                && slot->slot == slot_index) {
            return true;
        }
    }
    return false;
}

static bool context_is_ready(const st_dnu_context_t *context)
{
    size_t selector_count = 0u;
    const st_value_t *selector_symbols;

    if (context == NULL || !context->initialized
            || context->abi_version != ST_DNU_CONTEXT_ABI_VERSION
            || context->metadata == NULL || context->image == NULL
            || context->lookup == NULL || context->bootstrap == NULL
            || context->heap == NULL || context->selector_symbols == NULL
            || context->selector_count == 0u
            || !context->does_not_understand_site.initialized) {
        return false;
    }
    selector_symbols = st_aot_bootstrap_selector_symbols(
        context->bootstrap, &selector_count);
    return selector_symbols == context->selector_symbols
        && selector_count == context->selector_count
        && st_image_runtime_heap(context->image) == context->heap
        && context->lookup->initialized
        && context->lookup->descriptors
            == context->metadata->runtime_descriptors;
}

static st_dnu_status_t authenticate_value(
    const st_dnu_context_t *context, const st_aot_thread_t *thread,
    st_value_t value)
{
    if (!st_value_has_valid_encoding(value)) {
        return ST_DNU_ERR_INVALID_SEND;
    }
    if (st_value_kind(value) != ST_VALUE_OBJECT) {
        return ST_DNU_OK;
    }

    st_object_view_t view;
    uint32_t callback_class_id = 0u;
    if (st_heap_object_view(context->heap, value, &view) != ST_HEAP_OK) {
        return ST_DNU_ERR_HEAP;
    }
    if (thread->object_class == NULL
            || !thread->object_class(
                thread->object_class_user, value, &callback_class_id)
            || callback_class_id != view.class_descriptor->class_id) {
        return ST_DNU_ERR_INVALID_SEND;
    }
    return ST_DNU_OK;
}

static st_dnu_status_t map_heap_status(st_heap_status_t status)
{
    if (status == ST_HEAP_OK) {
        return ST_DNU_OK;
    }
    if (status == ST_HEAP_ERR_OUT_OF_MEMORY) {
        return ST_DNU_ERR_OUT_OF_MEMORY;
    }
    return ST_DNU_ERR_HEAP;
}

st_dnu_status_t st_dnu_context_init(
    st_dnu_context_t *context,
    const st_dnu_context_options_t *options)
{
    const st_image_metadata_descriptor_t *metadata;
    const st_runtime_descriptors_t *descriptors;
    const st_image_entity_metadata_t *message_entity;
    const st_image_entity_metadata_t *array_entity;
    const StClassDescriptor *message_class;
    const StShapeDescriptor *message_shape;
    const StClassDescriptor *array_class;
    const StShapeDescriptor *array_shape;
    const st_image_selector_metadata_t *dnu_selector;
    const st_value_t *selector_symbols;
    st_heap_t *heap;
    size_t selector_count = 0u;
    st_primitive_allocator_t allocator;

    if (context == NULL || options == NULL || context->initialized
            || context->attached_thread != NULL
            || options->metadata == NULL || options->image == NULL
            || options->lookup == NULL || options->bootstrap == NULL
            || options->message_entity_id == 0u
            || options->message_class_id == 0u
            || options->message_shape_id == 0u
            || options->message_selector_slot
                == options->message_arguments_slot
            || options->array_entity_id == 0u
            || options->array_class_id == 0u
            || options->array_shape_id == 0u
            || options->does_not_understand_selector_id == 0u
            || !normalize_allocator(options->allocator, &allocator)) {
        return ST_DNU_ERR_INVALID_ARGUMENT;
    }

    metadata = options->metadata;
    heap = st_image_runtime_heap(options->image);
    descriptors = heap == NULL ? NULL : st_heap_descriptors(heap);
    selector_symbols = st_aot_bootstrap_selector_symbols(
        options->bootstrap, &selector_count);
    if (metadata->magic != ST_IMAGE_METADATA_MAGIC
            || metadata->abi_version != ST_IMAGE_METADATA_ABI_VERSION
            || metadata->runtime_descriptors == NULL
            || descriptors != metadata->runtime_descriptors
            || !options->lookup->initialized
            || options->lookup->descriptors != descriptors
            || options->bootstrap->image != options->image
            || options->bootstrap->lookup != options->lookup
            || selector_symbols == NULL
            || selector_count != metadata->selector_count) {
        return ST_DNU_ERR_INVALID_METADATA;
    }

    message_entity = entity_at(metadata, options->message_entity_id);
    array_entity = entity_at(metadata, options->array_entity_id);
    message_class = st_runtime_class(
        descriptors, options->message_class_id);
    message_shape = st_runtime_shape(
        descriptors, options->message_shape_id);
    array_class = st_runtime_class(descriptors, options->array_class_id);
    array_shape = st_runtime_shape(descriptors, options->array_shape_id);
    dnu_selector = selector_at(
        metadata, options->does_not_understand_selector_id);
    if (message_entity == NULL
            || metadata->entity_runtime_class_ids == NULL
            || metadata->entity_runtime_class_ids[
                options->message_entity_id - 1u]
                != options->message_class_id
            || message_entity->kind != ST_CLASS_GRAPH_CLASS
            || message_class == NULL || message_shape == NULL
            || message_shape->class_id != message_class->class_id
            || message_class->default_shape_id != message_shape->shape_id
            || message_shape->indexed_format != ST_INDEXED_NONE
            || !pointer_slot(
                message_shape, options->message_selector_slot)
            || !pointer_slot(
                message_shape, options->message_arguments_slot)
            || !entity_declares_slot(
                metadata, message_entity,
                options->message_selector_slot)
            || !entity_declares_slot(
                metadata, message_entity,
                options->message_arguments_slot)
            || array_entity == NULL
            || metadata->entity_runtime_class_ids[
                options->array_entity_id - 1u]
                != options->array_class_id
            || array_entity->kind != ST_CLASS_GRAPH_CLASS
            || array_class == NULL || array_shape == NULL
            || array_shape->class_id != array_class->class_id
            || array_class->default_shape_id != array_shape->shape_id
            || array_shape->fixed_word_count != 0u
            || array_shape->indexed_format != ST_INDEXED_VALUES
            || dnu_selector == NULL || dnu_selector->arity != 1u) {
        return ST_DNU_ERR_INVALID_LAYOUT;
    }

    memset(context, 0, sizeof(*context));
    context->abi_version = ST_DNU_CONTEXT_ABI_VERSION;
    context->metadata = metadata;
    context->image = options->image;
    context->lookup = options->lookup;
    context->bootstrap = options->bootstrap;
    context->heap = heap;
    context->selector_symbols = selector_symbols;
    context->selector_count = selector_count;
    context->message_class_id = options->message_class_id;
    context->message_shape_id = options->message_shape_id;
    context->message_selector_slot = options->message_selector_slot;
    context->message_arguments_slot = options->message_arguments_slot;
    context->array_class_id = options->array_class_id;
    context->array_shape_id = options->array_shape_id;
    context->allocator = allocator;
    if (!st_send_site_init(
            &context->does_not_understand_site,
            options->does_not_understand_selector_id, 0u)) {
        memset(context, 0, sizeof(*context));
        return ST_DNU_ERR_INVALID_STATE;
    }
    context->initialized = true;
    return ST_DNU_OK;
}

st_dnu_status_t st_dnu_context_attach(
    st_dnu_context_t *context, st_aot_thread_t *thread)
{
    if (!context_is_ready(context) || thread == NULL
            || !thread->initialized
            || thread->abi_version != ST_AOT_THREAD_ABI_VERSION
            || thread->lookup != context->lookup
            || thread->image != context->image
            || thread->object_class == NULL
            || thread->control == NULL
            || thread->control->_st_abi_version != ST_CONTROL_ABI_VERSION
            || thread->control->_st_frame_thread_identity != thread) {
        return ST_DNU_ERR_INVALID_STATE;
    }
    if (context->attached_thread != NULL
            || thread->failure != NULL || thread->failure_user != NULL) {
        return ST_DNU_ERR_CONFLICT;
    }

    context->attached_thread = thread;
    thread->failure_user = context;
    thread->failure = st_dnu_send_failure;
    return ST_DNU_OK;
}

st_dnu_status_t st_dnu_context_detach(
    st_dnu_context_t *context, st_aot_thread_t *thread)
{
    if (!context_is_ready(context) || thread == NULL) {
        return ST_DNU_ERR_INVALID_ARGUMENT;
    }
    if (context->active || context->attached_thread != thread
            || thread->failure != st_dnu_send_failure
            || thread->failure_user != context) {
        return ST_DNU_ERR_CONFLICT;
    }

    thread->failure = NULL;
    thread->failure_user = NULL;
    context->attached_thread = NULL;
    return ST_DNU_OK;
}

st_dnu_status_t st_dnu_context_destroy(st_dnu_context_t *context)
{
    if (context == NULL) {
        return ST_DNU_ERR_INVALID_ARGUMENT;
    }
    if (context->attached_thread != NULL || context->active) {
        return ST_DNU_ERR_CONFLICT;
    }
    memset(context, 0, sizeof(*context));
    return ST_DNU_OK;
}

static st_dnu_status_t message_create(
    st_dnu_context_t *context, StFrame *caller,
    const st_send_site_t *original_site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_value_t *message_out)
{
    const st_image_selector_metadata_t *original_selector;
    st_aot_thread_t *thread;
    st_value_t arguments = ST_VALUE_INVALID;
    st_value_t message = ST_VALUE_INVALID;
    st_dnu_status_t status;

    *message_out = ST_VALUE_INVALID;
    if (!context_is_ready(context) || caller == NULL
            || st_aot_frame_validate(caller, 0u) != ST_AOT_SEND_OK
            || caller->thread != context->attached_thread
            || original_site == NULL || !original_site->initialized
            || (argc != 0u && argv == NULL)) {
        return ST_DNU_ERR_INVALID_FRAME;
    }
    thread = caller->thread;
    original_selector = selector_at(
        context->metadata, original_site->selector_id);
    if (original_selector == NULL || original_selector->arity != argc) {
        return ST_DNU_ERR_INVALID_SEND;
    }
    status = authenticate_value(context, thread, receiver);
    if (status != ST_DNU_OK) {
        return status;
    }
    for (uint32_t index = 0u; index < argc; index++) {
        status = authenticate_value(context, thread, argv[index]);
        if (status != ST_DNU_OK) {
            return status;
        }
    }

    status = map_heap_status(st_heap_allocate(
        context->heap, context->array_class_id, context->array_shape_id,
        argc, argc, 0u, &arguments));
    if (status != ST_DNU_OK) {
        return status;
    }
    for (uint32_t index = 0u; index < argc; index++) {
        status = map_heap_status(st_heap_indexed_reference_store(
            context->heap, arguments, index, argv[index]));
        if (status != ST_DNU_OK) {
            return status;
        }
    }

    status = map_heap_status(st_heap_allocate(
        context->heap, context->message_class_id,
        context->message_shape_id, 0u, 0u, 0u, &message));
    if (status != ST_DNU_OK) {
        return status;
    }
    status = map_heap_status(st_heap_fixed_reference_store(
        context->heap, message, context->message_selector_slot,
        context->selector_symbols[original_site->selector_id - 1u]));
    if (status != ST_DNU_OK) {
        return status;
    }
    status = map_heap_status(st_heap_fixed_reference_store(
        context->heap, message, context->message_arguments_slot,
        arguments));
    if (status != ST_DNU_OK) {
        return status;
    }

    *message_out = message;
    return ST_DNU_OK;
}

st_dnu_status_t st_dnu_message_create(
    st_dnu_context_t *context, StFrame *caller,
    const st_send_site_t *original_site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_value_t *message_out)
{
    if (message_out == NULL) {
        return ST_DNU_ERR_INVALID_ARGUMENT;
    }
    return message_create(
        context, caller, original_site, receiver, argv, argc, message_out);
}

static st_dnu_status_t dispatch_dnu(
    st_dnu_context_t *context, StFrame *caller,
    const st_send_site_t *original_site, st_value_t receiver,
    const st_value_t *argv, uint32_t argc, st_value_t *result_out)
{
    st_aot_send_target_t target;
    st_aot_send_status_t send_status;
    st_value_t message = ST_VALUE_INVALID;
    st_dnu_status_t status;

    *result_out = ST_VALUE_INVALID;
    send_status = st_aot_send_resolve(
        caller, &context->does_not_understand_site, receiver, 1u, &target);
    if (send_status == ST_AOT_SEND_ERR_NOT_FOUND) {
        return ST_DNU_ERR_MISSING_HANDLER;
    }
    if (send_status == ST_AOT_SEND_ERR_LOOKUP) {
        return ST_DNU_ERR_LOOKUP;
    }
    if (send_status != ST_AOT_SEND_OK) {
        return ST_DNU_ERR_INVALID_HANDLER;
    }

    status = message_create(
        context, caller, original_site, receiver, argv, argc, &message);
    if (status != ST_DNU_OK) {
        return status;
    }

    uint32_t root_capacity = target.frame_root_capacity;
    st_value_t *roots = NULL;
    if (root_capacity != 0u) {
        roots = context->allocator.allocate(
            context->allocator.user,
            (size_t)root_capacity * sizeof(*roots));
        if (roots == NULL) {
            return ST_DNU_ERR_OUT_OF_MEMORY;
        }
    }
    if (st_aot_frame_roots_initialize(roots, root_capacity)
            != ST_AOT_SEND_OK) {
        if (roots != NULL) {
            context->allocator.deallocate(context->allocator.user, roots);
        }
        return ST_DNU_ERR_INVALID_HANDLER;
    }
    st_value_t dnu_arguments[1] = { message };
    StFrame child = {
        .thread = caller->thread,
        .caller = caller,
        .method = target.descriptor,
        .receiver = receiver,
        .argv = dnu_arguments,
        .roots = roots,
        .argc = 1u,
        .root_count = root_capacity
    };

    /* Message is now an authenticated child argv root; its Array is reached
     * through the Message's fixed reference before any Smalltalk code or
     * safepoint can run. */
    *result_out = target.code(&child);
    if (roots != NULL) {
        context->allocator.deallocate(context->allocator.user, roots);
    }
    return ST_DNU_OK;
}

st_value_t st_dnu_send_failure(
    void *user, StFrame *caller, const st_send_site_t *site,
    st_value_t receiver, const st_value_t *argv, uint32_t argc,
    st_aot_send_status_t status)
{
    st_dnu_context_t *context = user;
    st_value_t result = ST_VALUE_INVALID;

    if (status != ST_AOT_SEND_ERR_NOT_FOUND
            || !context_is_ready(context)
            || context->attached_thread == NULL
            || caller == NULL
            || caller->thread != context->attached_thread
            || context->attached_thread->failure != st_dnu_send_failure
            || context->attached_thread->failure_user != context
            || context->active) {
        abort();
    }

    context->active = true;
    st_dnu_status_t dnu_status = dispatch_dnu(
        context, caller, site, receiver, argv, argc, &result);
    context->active = false;
    if (dnu_status != ST_DNU_OK) {
        abort();
    }
    return result;
}

const char *st_dnu_status_string(st_dnu_status_t status)
{
    switch (status) {
    case ST_DNU_OK: return "ok";
    case ST_DNU_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_DNU_ERR_INVALID_STATE: return "invalid state";
    case ST_DNU_ERR_INVALID_METADATA: return "invalid metadata";
    case ST_DNU_ERR_INVALID_LAYOUT: return "invalid layout";
    case ST_DNU_ERR_INVALID_FRAME: return "invalid frame";
    case ST_DNU_ERR_INVALID_SEND: return "invalid send";
    case ST_DNU_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_DNU_ERR_HEAP: return "heap failure";
    case ST_DNU_ERR_LOOKUP: return "lookup failure";
    case ST_DNU_ERR_MISSING_HANDLER: return "missing doesNotUnderstand:";
    case ST_DNU_ERR_INVALID_HANDLER: return "invalid doesNotUnderstand:";
    case ST_DNU_ERR_REENTRANT: return "recursive doesNotUnderstand:";
    case ST_DNU_ERR_CONFLICT: return "lifecycle conflict";
    default: return "unknown DNU status";
    }
}
