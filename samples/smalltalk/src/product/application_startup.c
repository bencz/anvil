#include "st_application_startup.h"

#include "st_aot_bootstrap.h"
#include "st_closure_bridge.h"
#include "st_dnu.h"
#include "st_float_primitives.h"
#include "st_heap_primitives.h"
#include "st_integer_primitives.h"
#include "st_string_primitives.h"
#include "st_symbol_intern.h"

#include <stdlib.h>
#include <string.h>

struct st_application_startup_state {
    const st_application_launch_descriptor_t *launch;
    const st_image_metadata_descriptor_t *metadata;
    st_application_startup_allocator_t allocator;

    st_image_runtime_t image;
    st_lookup_context_t lookup;
    st_symbol_intern_context_t symbols;
    st_aot_bootstrap_context_t bootstrap;
    st_heap_primitive_context_t heap_primitives;
    st_float_primitive_context_t floats;
    st_numeric_context_t numeric;
    st_stream_primitive_context_t streams;
    st_string_primitive_context_t strings;
    st_control_thread_t control;
    st_aot_closure_context_t closures;
    st_aot_thread_t thread;
    st_dnu_context_t dnu;

    st_heap_indexed_access_t *indexed_access;
    bool image_ready;
    bool lookup_ready;
    bool symbols_ready;
    bool bootstrap_ready;
    bool heap_primitives_ready;
    bool floats_ready;
    bool numeric_ready;
    bool streams_ready;
    bool strings_ready;
    bool control_ready;
    bool closures_ready;
    bool thread_ready;
    bool image_attached;
    bool streams_attached;
    bool strings_attached;
    bool numeric_attached;
    bool reflection_attached;
    bool dnu_ready;
    bool dnu_attached;
};

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
    st_application_startup_allocator_t input,
    st_application_startup_allocator_t *output)
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

static st_primitive_allocator_t primitive_allocator(
    st_application_startup_state_t *state)
{
    return (st_primitive_allocator_t) {
        state->allocator.allocate,
        state->allocator.deallocate,
        state->allocator.user
    };
}

static bool host_is_little_endian(void)
{
    const uint16_t probe = UINT16_C(1);
    return *(const uint8_t *)&probe == UINT8_C(1);
}

static bool multiply_size(size_t left, size_t right, size_t *result_out)
{
    if (result_out == NULL || (left != 0u && right > SIZE_MAX / left)) {
        return false;
    }
    *result_out = left * right;
    return true;
}

static bool zero_reserved(const st_application_launch_descriptor_t *launch)
{
    for (size_t index = 0u;
         index < ST_APPLICATION_LAUNCH_RESERVED_COUNT; index++) {
        if (launch->reserved[index] != 0u) {
            return false;
        }
    }
    return true;
}

static bool class_and_shape(
    const st_runtime_descriptors_t *descriptors,
    uint32_t class_id, uint32_t shape_id)
{
    const StClassDescriptor *class_descriptor =
        st_runtime_class(descriptors, class_id);
    const StShapeDescriptor *shape_descriptor =
        st_runtime_shape(descriptors, shape_id);

    return class_descriptor != NULL && shape_descriptor != NULL
        && shape_descriptor->class_id == class_descriptor->class_id;
}

static bool launch_is_valid(
    const st_application_launch_descriptor_t *launch)
{
    const st_image_metadata_descriptor_t *metadata;
    const st_runtime_descriptors_t *descriptors;
    const uint32_t classes[] = {
        launch == NULL ? 0u : launch->nil_class_id,
        launch == NULL ? 0u : launch->false_class_id,
        launch == NULL ? 0u : launch->true_class_id,
        launch == NULL ? 0u : launch->small_integer_class_id,
        launch == NULL ? 0u : launch->character_class_id
    };

    if (launch == NULL || launch->magic != ST_APPLICATION_LAUNCH_MAGIC
            || launch->abi_version != ST_APPLICATION_LAUNCH_ABI_VERSION
            || launch->descriptor_size != sizeof(*launch)
            || launch->metadata_abi_version != ST_IMAGE_METADATA_ABI_VERSION
            || launch->flags != 0u || launch->metadata == NULL
            || launch->entry_arity != 0u
            || launch->transcript_semantic_external_id
                != ST_IMAGE_EXTERNAL_ID_TRANSCRIPT
            || launch->expected_global_count == 0u
            || !zero_reserved(launch)) {
        return false;
    }
    metadata = launch->metadata;
    descriptors = metadata->runtime_descriptors;
    if (metadata->magic != ST_IMAGE_METADATA_MAGIC
            || metadata->abi_version != ST_IMAGE_METADATA_ABI_VERSION
            || metadata->pointer_size != sizeof(void *)
            || metadata->endian
                != (host_is_little_endian()
                    ? ANVIL_ENDIAN_LITTLE : ANVIL_ENDIAN_BIG)
            || metadata->global_count != launch->expected_global_count
            || metadata->string_literal_count
                != launch->expected_string_literal_count
            || metadata->selector_count != launch->expected_selector_count
            || descriptors == NULL
            || st_runtime_descriptors_validate(descriptors) != ST_RUNTIME_OK
            || metadata->runtime_class_count != descriptors->class_count
            || metadata->runtime_shape_count != descriptors->shape_count
            || metadata->runtime_layout_count != descriptors->shape_count
            || metadata->entity_runtime_class_ids == NULL
            || launch->entry_entity_id == 0u
            || launch->entry_entity_id > metadata->entity_count
            || metadata->entity_runtime_class_ids[
                   launch->entry_entity_id - 1u]
                != launch->entry_runtime_class_id
            || !class_and_shape(
                descriptors, launch->entry_runtime_class_id,
                launch->entry_default_shape_id)
            || (st_runtime_class(
                    descriptors, launch->entry_runtime_class_id)->flags
                & (ST_CLASS_METACLASS | ST_CLASS_ABSTRACT)) != 0u
            || launch->entry_selector_id == 0u
            || launch->entry_selector_id > metadata->selector_count
            || metadata->selectors[launch->entry_selector_id - 1u].id
                != launch->entry_selector_id
            || metadata->selectors[launch->entry_selector_id - 1u].arity
                != launch->entry_arity) {
        return false;
    }
    for (size_t index = 0u; index < sizeof(classes) / sizeof(classes[0]);
         index++) {
        if (classes[index] == 0u
                || st_runtime_class(descriptors, classes[index]) == NULL) {
            return false;
        }
        for (size_t prior = 0u; prior < index; prior++) {
            if (classes[prior] == classes[index]) {
                return false;
            }
        }
    }
    return true;
}

static bool build_runtime_entries(
    st_application_startup_state_t *state,
    st_image_runtime_entry_t **globals_out,
    st_image_runtime_entry_t **literals_out)
{
    const st_image_metadata_descriptor_t *metadata = state->metadata;
    st_image_runtime_entry_t *globals = NULL;
    st_image_runtime_entry_t *literals = NULL;

    *globals_out = NULL;
    *literals_out = NULL;
    if (metadata->global_count == 0u || metadata->globals == NULL
            || metadata->global_count > UINT32_MAX) {
        return false;
    }
    size_t global_bytes;
    if (!multiply_size(metadata->global_count, sizeof(*globals),
                       &global_bytes)) {
        return false;
    }
    globals = state->allocator.allocate(state->allocator.user, global_bytes);
    if (globals == NULL) {
        return false;
    }
    size_t transcript_count = 0u;
    for (size_t index = 0u; index < metadata->global_count; index++) {
        const st_image_global_metadata_t *global = &metadata->globals[index];
        if (global->runtime_index >= metadata->global_count
                || global->name == NULL) {
            state->allocator.deallocate(state->allocator.user, globals);
            return false;
        }
        if (global->semantic_external_id
                == ST_IMAGE_EXTERNAL_ID_TRANSCRIPT) {
            transcript_count++;
            if (global->runtime_index
                    != state->launch->transcript_runtime_index) {
                state->allocator.deallocate(state->allocator.user, globals);
                return false;
            }
        }
        globals[index] = (st_image_runtime_entry_t) {
            .id = global->runtime_index + 1u,
            .value = ST_VALUE_INVALID
        };
    }
    if (transcript_count != 1u) {
        state->allocator.deallocate(state->allocator.user, globals);
        return false;
    }
    if (metadata->string_literal_count != 0u) {
        size_t literal_bytes;
        if (metadata->string_literals == NULL
                || !multiply_size(
                    metadata->string_literal_count, sizeof(*literals),
                    &literal_bytes)) {
            state->allocator.deallocate(state->allocator.user, globals);
            return false;
        }
        literals = state->allocator.allocate(
            state->allocator.user,
            literal_bytes);
        if (literals == NULL) {
            state->allocator.deallocate(state->allocator.user, globals);
            return false;
        }
        for (size_t index = 0u; index < metadata->string_literal_count;
             index++) {
            const st_image_string_literal_metadata_t *literal =
                &metadata->string_literals[index];
            if (literal->literal_id != index
                    || (literal->bytes == NULL && literal->length != 0u)) {
                state->allocator.deallocate(state->allocator.user, literals);
                state->allocator.deallocate(state->allocator.user, globals);
                return false;
            }
            literals[index] = (st_image_runtime_entry_t) {
                .id = (uint32_t)index + 1u,
                .value = ST_VALUE_INVALID
            };
        }
    }
    *globals_out = globals;
    *literals_out = literals;
    return true;
}

static st_heap_indexed_access_t access_for_layout(
    const st_application_startup_state_t *state,
    const st_image_runtime_layout_metadata_t *layout)
{
    switch ((st_image_layout_recipe_t)layout->recipe) {
    case ST_IMAGE_LAYOUT_FIXED_POINTERS:
    case ST_IMAGE_LAYOUT_BOXED_FLOAT64:
    case ST_IMAGE_LAYOUT_CELL:
        return ST_HEAP_INDEXED_ACCESS_NONE;
    case ST_IMAGE_LAYOUT_INDEXED_VALUES:
    case ST_IMAGE_LAYOUT_CLOSURE:
        return ST_HEAP_INDEXED_ACCESS_VALUES;
    case ST_IMAGE_LAYOUT_INDEXED_UINT8:
    case ST_IMAGE_LAYOUT_INDEXED_UINT16:
    case ST_IMAGE_LAYOUT_INDEXED_UINT32:
        if (layout->runtime_class_id == state->launch->string_class_id
                || layout->runtime_class_id
                    == state->launch->symbol_class_id) {
            return ST_HEAP_INDEXED_ACCESS_CHARACTER;
        }
        return ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    case ST_IMAGE_LAYOUT_LARGE_INTEGER:
        return ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    }
    return (st_heap_indexed_access_t)UINT32_MAX;
}

static bool build_indexed_access(st_application_startup_state_t *state)
{
    const st_image_metadata_descriptor_t *metadata = state->metadata;
    size_t bytes;

    if (!multiply_size(
            metadata->runtime_shape_count, sizeof(*state->indexed_access),
            &bytes)) {
        return false;
    }
    state->indexed_access = state->allocator.allocate(
        state->allocator.user, bytes);
    if (state->indexed_access == NULL && bytes != 0u) {
        return false;
    }
    for (size_t index = 0u; index < metadata->runtime_shape_count; index++) {
        state->indexed_access[index] = (st_heap_indexed_access_t)UINT32_MAX;
    }
    for (size_t index = 0u; index < metadata->runtime_layout_count; index++) {
        const st_image_runtime_layout_metadata_t *layout =
            &metadata->runtime_layouts[index];
        st_heap_indexed_access_t access;

        if (layout->runtime_shape_id == 0u
                || layout->runtime_shape_id > metadata->runtime_shape_count
                || state->indexed_access[layout->runtime_shape_id - 1u]
                    != (st_heap_indexed_access_t)UINT32_MAX) {
            return false;
        }
        access = access_for_layout(state, layout);
        if (access == (st_heap_indexed_access_t)UINT32_MAX) {
            return false;
        }
        state->indexed_access[layout->runtime_shape_id - 1u] = access;
    }
    for (size_t index = 0u; index < metadata->runtime_shape_count; index++) {
        if (state->indexed_access[index]
                == (st_heap_indexed_access_t)UINT32_MAX) {
            return false;
        }
    }
    return true;
}

static bool startup_object_class(
    void *user, st_value_t value, uint32_t *class_id_out)
{
    st_application_startup_state_t *state = user;
    st_object_view_t view;

    if (state == NULL || class_id_out == NULL
            || st_heap_object_view(
                st_image_runtime_heap(&state->image), value, &view)
                != ST_HEAP_OK) {
        return false;
    }
    *class_id_out = view.class_descriptor->class_id;
    return true;
}

static bool startup_class_object(
    void *user, st_value_t class_object, uint32_t *class_id_out)
{
    st_application_startup_state_t *state = user;
    size_t count = 0u;
    const st_value_t *classes;

    if (state == NULL || class_id_out == NULL) {
        return false;
    }
    classes = st_aot_bootstrap_class_objects(&state->bootstrap, &count);
    for (size_t index = 0u; index < count; index++) {
        if (classes[index] == class_object) {
            *class_id_out = (uint32_t)index + 1u;
            return true;
        }
    }
    return false;
}

static st_application_startup_status_t initialize_image(
    st_application_startup_state_t *state,
    const st_application_startup_options_t *options)
{
    st_image_runtime_entry_t *globals = NULL;
    st_image_runtime_entry_t *literals = NULL;
    st_image_runtime_status_t status;
    st_value_t bootstrapped;

    if (!build_runtime_entries(state, &globals, &literals)) {
        return ST_APPLICATION_STARTUP_ERR_INVALID_METADATA;
    }
    status = st_image_runtime_init(
        &state->image,
        &(st_image_runtime_options_t) {
            .descriptors = state->metadata->runtime_descriptors,
            .heap_allocator = options->heap_allocator,
            .globals = globals,
            .global_count = state->metadata->global_count,
            .literals = literals,
            .literal_count = state->metadata->string_literal_count,
            .string_layout = {
                state->launch->string_class_id,
                state->launch->string_uint8_shape_id
            },
            .external_stream_layout = {
                state->launch->external_stream_class_id,
                state->launch->external_stream_shape_id,
                state->launch->external_stream_descriptor_slot
            }
        });
    state->allocator.deallocate(state->allocator.user, literals);
    state->allocator.deallocate(state->allocator.user, globals);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status == ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY
            ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
            : ST_APPLICATION_STARTUP_ERR_IMAGE;
    }
    state->image_ready = true;
    for (size_t index = 0u; index < state->metadata->string_literal_count;
         index++) {
        const st_image_string_literal_metadata_t *literal =
            &state->metadata->string_literals[index];
        status = st_image_runtime_bootstrap_string_literal(
            &state->image, (uint32_t)index, literal->bytes, literal->length,
            &bootstrapped);
        if (status != ST_IMAGE_RUNTIME_OK) {
            return status == ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY
                ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
                : ST_APPLICATION_STARTUP_ERR_IMAGE;
        }
    }
    status = st_image_runtime_bootstrap_transcript(
        &state->image, state->launch->transcript_runtime_index,
        &bootstrapped);
    if (status != ST_IMAGE_RUNTIME_OK) {
        return status == ST_IMAGE_RUNTIME_ERR_OUT_OF_MEMORY
            ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
            : ST_APPLICATION_STARTUP_ERR_IMAGE;
    }
    return ST_APPLICATION_STARTUP_OK;
}

static st_application_startup_status_t initialize_bootstrap(
    st_application_startup_state_t *state)
{
    const st_application_launch_descriptor_t *launch = state->launch;
    st_primitive_allocator_t allocator = primitive_allocator(state);
    st_lookup_status_t lookup_status;
    st_symbol_intern_status_t symbol_status;
    st_aot_bootstrap_status_t bootstrap_status;

    lookup_status = st_lookup_context_init(
        &state->lookup, state->metadata->runtime_descriptors,
        (st_lookup_allocator_t) {
            allocator.allocate, allocator.deallocate, allocator.user
        });
    if (lookup_status != ST_LOOKUP_FOUND) {
        return lookup_status == ST_LOOKUP_ERR_OUT_OF_MEMORY
            ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
            : ST_APPLICATION_STARTUP_ERR_LOOKUP;
    }
    state->lookup_ready = true;
    symbol_status = st_symbol_intern_context_init(
        &state->symbols,
        &(st_symbol_intern_options_t) {
            .image = &state->image,
            .string_class_id = launch->string_class_id,
            .string_uint8_shape_id = launch->string_uint8_shape_id,
            .string_uint16_shape_id = launch->string_uint16_shape_id,
            .string_uint32_shape_id = launch->string_uint32_shape_id,
            .symbol_class_id = launch->symbol_class_id,
            .symbol_uint8_shape_id = launch->symbol_uint8_shape_id,
            .symbol_uint16_shape_id = launch->symbol_uint16_shape_id,
            .symbol_uint32_shape_id = launch->symbol_uint32_shape_id
        });
    if (symbol_status != ST_SYMBOL_INTERN_OK) {
        return symbol_status == ST_SYMBOL_INTERN_ERR_OUT_OF_MEMORY
            ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
            : ST_APPLICATION_STARTUP_ERR_SYMBOLS;
    }
    state->symbols_ready = true;
    bootstrap_status = st_aot_bootstrap_context_init(
        &state->bootstrap,
        &(st_aot_bootstrap_options_t) {
            .metadata = state->metadata,
            .image = &state->image,
            .symbols = &state->symbols,
            .lookup = &state->lookup,
            .object_entity_id = launch->object_entity_id,
            .class_object_layout_entity_id =
                launch->class_object_layout_entity_id,
            .metaclass_entity_id = launch->metaclass_entity_id,
            .integer_entity_id = launch->integer_entity_id,
            .small_integer_entity_id = launch->small_integer_entity_id,
            .array_class_id = launch->array_class_id,
            .array_shape_id = launch->array_shape_id,
            .method_dictionary_class_id = launch->method_dictionary_class_id,
            .method_dictionary_shape_id = launch->method_dictionary_shape_id,
            .symbol_class_id = launch->symbol_class_id,
            .compiled_method_class_id = launch->compiled_method_class_id,
            .compiled_method_shape_id = launch->compiled_method_shape_id,
            .allocator = allocator
        });
    if (bootstrap_status != ST_AOT_BOOTSTRAP_OK) {
        return bootstrap_status == ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY
            ? ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY
            : ST_APPLICATION_STARTUP_ERR_BOOTSTRAP;
    }
    state->bootstrap_ready = true;

    size_t class_count = 0u;
    const st_value_t *class_objects = st_aot_bootstrap_class_objects(
        &state->bootstrap, &class_count);
    for (size_t index = 0u; index < state->metadata->global_count; index++) {
        const st_image_global_metadata_t *global =
            &state->metadata->globals[index];
        const st_image_entity_metadata_t *entity;
        uint32_t runtime_class_id;

        if (global->semantic_external_id == ST_IMAGE_EXTERNAL_ID_TRANSCRIPT) {
            continue;
        }
        if (global->semantic_external_id == 0u
                || global->semantic_external_id > state->metadata->entity_count) {
            return ST_APPLICATION_STARTUP_ERR_INVALID_METADATA;
        }
        entity = &state->metadata->entities[
            global->semantic_external_id - 1u];
        runtime_class_id = state->metadata->entity_runtime_class_ids[
            global->semantic_external_id - 1u];
        if (entity->id != global->semantic_external_id
                || entity->kind != ST_CLASS_GRAPH_CLASS
                || entity->name == NULL
                || strcmp(entity->name, global->name) != 0
                || runtime_class_id == 0u
                || runtime_class_id > class_count
                || st_image_runtime_bootstrap_global_value(
                    &state->image, global->runtime_index,
                    class_objects[runtime_class_id - 1u])
                    != ST_IMAGE_RUNTIME_OK) {
            return ST_APPLICATION_STARTUP_ERR_INVALID_METADATA;
        }
    }
    return ST_APPLICATION_STARTUP_OK;
}

static st_application_startup_status_t initialize_primitives(
    st_application_startup_state_t *state,
    const st_application_startup_options_t *options)
{
    const st_application_launch_descriptor_t *launch = state->launch;
    st_primitive_allocator_t allocator = primitive_allocator(state);
    const st_value_t *class_objects;
    size_t class_count = 0u;

    if (!build_indexed_access(state)) {
        return ST_APPLICATION_STARTUP_ERR_INVALID_METADATA;
    }
    class_objects = st_aot_bootstrap_class_objects(
        &state->bootstrap, &class_count);
    if (st_heap_primitive_context_init(
            &state->heap_primitives,
            &(st_heap_primitive_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .immediate_classes = {
                    launch->small_integer_class_id,
                    launch->character_class_id,
                    launch->nil_class_id,
                    launch->false_class_id,
                    launch->true_class_id
                },
                .class_objects = class_objects,
                .class_object_count = class_count,
                .indexed_access = state->indexed_access,
                .indexed_access_count = state->metadata->runtime_shape_count,
                .allocator = allocator
            }) != ST_HEAP_PRIMITIVE_OK) {
        return ST_APPLICATION_STARTUP_ERR_PRIMITIVES;
    }
    state->heap_primitives_ready = true;
    if (st_float_primitive_context_init(
            &state->floats,
            &(st_float_primitive_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .boxed_float64_class_id = launch->boxed_float64_class_id,
                .boxed_float64_shape_id = launch->boxed_float64_shape_id,
                .allocator = allocator
            }) != ST_FLOAT_PRIMITIVE_OK) {
        return ST_APPLICATION_STARTUP_ERR_PRIMITIVES;
    }
    state->floats_ready = true;
    if (st_numeric_context_init(
            &state->numeric,
            &(st_numeric_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .large_positive_class_id = launch->large_positive_class_id,
                .large_positive_shape_id = launch->large_positive_shape_id,
                .large_negative_class_id = launch->large_negative_class_id,
                .large_negative_shape_id = launch->large_negative_shape_id,
                .float_primitives = &state->floats,
                .scratch_allocator = allocator
            }) != ST_INTEGER_PRIMITIVE_OK) {
        return ST_APPLICATION_STARTUP_ERR_PRIMITIVES;
    }
    state->numeric_ready = true;
    if (st_stream_primitive_context_init(
            &state->streams,
            &(st_stream_primitive_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .string_class_id = launch->string_class_id,
                .string_shape_id = launch->string_uint8_shape_id,
                .write_bytes = options->write_bytes,
                .write_user = options->write_user
            }) != ST_STREAM_PRIMITIVE_OK) {
        return ST_APPLICATION_STARTUP_ERR_PRIMITIVES;
    }
    state->streams_ready = true;
    if (st_string_primitive_context_init(
            &state->strings,
            &(st_string_primitive_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .string_class_id = launch->string_class_id,
                .uint8_shape_id = launch->string_uint8_shape_id,
                .uint16_shape_id = launch->string_uint16_shape_id,
                .uint32_shape_id = launch->string_uint32_shape_id
            }) != ST_STRING_PRIMITIVE_OK) {
        return ST_APPLICATION_STARTUP_ERR_PRIMITIVES;
    }
    state->strings_ready = true;
    return ST_APPLICATION_STARTUP_OK;
}

static st_application_startup_status_t initialize_thread(
    st_application_startup_state_t *state)
{
    const st_application_launch_descriptor_t *launch = state->launch;
    st_primitive_allocator_t allocator = primitive_allocator(state);
    const uint32_t immediate_classes[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        launch->nil_class_id,
        launch->false_class_id,
        launch->true_class_id,
        launch->small_integer_class_id,
        launch->character_class_id
    };

    if (st_control_thread_init(
            &state->control, &state->thread,
            (st_control_allocator_t) {
                allocator.allocate, allocator.deallocate, allocator.user
            }) != ST_CONTROL_OK) {
        return ST_APPLICATION_STARTUP_ERR_CONTROL;
    }
    state->control_ready = true;
    if (st_control_exception_configure(
            &state->control, startup_class_object, state) != ST_CONTROL_OK) {
        return ST_APPLICATION_STARTUP_ERR_CONTROL;
    }
    if (st_aot_closure_context_init(
            &state->closures,
            &(st_aot_closure_options_t) {
                .heap = st_image_runtime_heap(&state->image),
                .closure_class_id = launch->block_class_id,
                .closure_shape_id = launch->block_shape_id,
                .cell_class_id = launch->closure_cell_class_id,
                .cell_shape_id = launch->closure_cell_shape_id,
                .argument_array_class_id = launch->array_class_id,
                .argument_array_shape_id = launch->array_shape_id,
                .descriptors = state->metadata->block_descriptors,
                .descriptor_count = state->metadata->block_count,
                .allocate = allocator.allocate,
                .deallocate = allocator.deallocate,
                .allocator_user = allocator.user
            }) != ST_AOT_CLOSURE_OK) {
        return ST_APPLICATION_STARTUP_ERR_CLOSURES;
    }
    state->closures_ready = true;
    if (!st_aot_thread_init(
            &state->thread, &state->lookup, immediate_classes,
            &state->heap_primitives, &state->control, &state->closures,
            startup_object_class, state, NULL, NULL)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->thread_ready = true;
    if (!st_aot_thread_image_attach(&state->thread, &state->image)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->image_attached = true;
    if (!st_aot_thread_streams_attach(&state->thread, &state->streams)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->streams_attached = true;
    if (!st_aot_thread_strings_attach(&state->thread, &state->strings)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->strings_attached = true;
    if (!st_aot_thread_numeric_attach(&state->thread, &state->numeric)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->numeric_attached = true;
    if (!st_aot_thread_reflection_attach(
            &state->thread, &state->bootstrap.reflection)) {
        return ST_APPLICATION_STARTUP_ERR_THREAD;
    }
    state->reflection_attached = true;
    if (st_dnu_context_init(
            &state->dnu,
            &(st_dnu_context_options_t) {
                .metadata = state->metadata,
                .image = &state->image,
                .lookup = &state->lookup,
                .bootstrap = &state->bootstrap,
                .message_entity_id = launch->message_entity_id,
                .message_class_id = launch->message_class_id,
                .message_shape_id = launch->message_shape_id,
                .message_selector_slot = launch->message_selector_slot,
                .message_arguments_slot = launch->message_arguments_slot,
                .array_entity_id = launch->array_entity_id,
                .array_class_id = launch->array_class_id,
                .array_shape_id = launch->array_shape_id,
                .does_not_understand_selector_id =
                    launch->does_not_understand_selector_id,
                .allocator = allocator
            }) != ST_DNU_OK) {
        return ST_APPLICATION_STARTUP_ERR_DNU;
    }
    state->dnu_ready = true;
    if (st_dnu_context_attach(&state->dnu, &state->thread) != ST_DNU_OK) {
        return ST_APPLICATION_STARTUP_ERR_DNU;
    }
    state->dnu_attached = true;
    return ST_APPLICATION_STARTUP_OK;
}

static st_application_startup_status_t destroy_state(
    st_application_startup_context_t *context, bool final_collection)
{
    st_application_startup_state_t *state = context->state;
    st_application_startup_status_t status = ST_APPLICATION_STARTUP_OK;
    st_heap_collection_stats_t final_collection_stats;

    if (state == NULL) {
        return ST_APPLICATION_STARTUP_OK;
    }
    if (state->control_ready
            && state->control._st_pending_kind != ST_CONTROL_PENDING_NONE
            && st_control_pending_clear(&state->control) != ST_CONTROL_OK) {
        return ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    if (final_collection && state->image_ready) {
        memset(&final_collection_stats, 0, sizeof(final_collection_stats));
        if (st_image_runtime_collect(
                &state->image, NULL, &final_collection_stats)
                != ST_IMAGE_RUNTIME_OK) {
            return ST_APPLICATION_STARTUP_ERR_CLEANUP;
        }
    }
    if (state->dnu_attached
            && st_dnu_context_detach(&state->dnu, &state->thread)
                != ST_DNU_OK) {
        return ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->dnu_attached = false;
    if (state->dnu_ready
            && st_dnu_context_destroy(&state->dnu) != ST_DNU_OK) {
        return ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->dnu_ready = false;
    if (state->reflection_attached
            && !st_aot_thread_reflection_detach(
                &state->thread, &state->bootstrap.reflection)) {
        status = ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->reflection_attached = false;
    if (state->numeric_attached
            && !st_aot_thread_numeric_detach(
                &state->thread, &state->numeric)) {
        status = ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->numeric_attached = false;
    if (state->strings_attached
            && !st_aot_thread_strings_detach(
                &state->thread, &state->strings)) {
        status = ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->strings_attached = false;
    if (state->streams_attached
            && !st_aot_thread_streams_detach(
                &state->thread, &state->streams)) {
        status = ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->streams_attached = false;
    if (state->image_attached
            && !st_aot_thread_image_detach(
                &state->thread, &state->image)) {
        status = ST_APPLICATION_STARTUP_ERR_CLEANUP;
    }
    state->image_attached = false;
    if (state->thread_ready) {
        st_aot_thread_destroy(&state->thread);
        state->thread_ready = false;
    }
    if (state->closures_ready) {
        if (st_aot_closure_context_destroy(&state->closures)
                != ST_AOT_CLOSURE_OK) {
            return ST_APPLICATION_STARTUP_ERR_CLEANUP;
        }
        state->closures_ready = false;
    }
    if (state->control_ready) {
        if (st_control_thread_destroy(&state->control) != ST_CONTROL_OK) {
            return ST_APPLICATION_STARTUP_ERR_CLEANUP;
        }
        state->control_ready = false;
    }
    if (state->strings_ready) {
        st_string_primitive_context_destroy(&state->strings);
        state->strings_ready = false;
    }
    if (state->streams_ready) {
        st_stream_primitive_context_destroy(&state->streams);
        state->streams_ready = false;
    }
    if (state->numeric_ready) {
        st_numeric_context_destroy(&state->numeric);
        state->numeric_ready = false;
    }
    if (state->floats_ready) {
        st_float_primitive_context_destroy(&state->floats);
        state->floats_ready = false;
    }
    if (state->heap_primitives_ready) {
        st_heap_primitive_context_destroy(&state->heap_primitives);
        state->heap_primitives_ready = false;
    }
    if (state->bootstrap_ready) {
        st_aot_bootstrap_context_destroy(&state->bootstrap);
        state->bootstrap_ready = false;
    }
    if (state->symbols_ready) {
        st_symbol_intern_context_destroy(&state->symbols);
        state->symbols_ready = false;
    }
    if (state->lookup_ready) {
        st_lookup_context_destroy(&state->lookup);
        state->lookup_ready = false;
    }
    if (state->image_ready) {
        st_image_runtime_destroy(&state->image);
        state->image_ready = false;
    }
    state->allocator.deallocate(
        state->allocator.user, state->indexed_access);
    {
        st_application_startup_allocator_t allocator = state->allocator;
        memset(state, 0, sizeof(*state));
        allocator.deallocate(allocator.user, state);
    }
    memset(context, 0, sizeof(*context));
    return status;
}

st_application_startup_status_t st_application_startup_context_init(
    st_application_startup_context_t *context,
    const st_application_startup_options_t *options)
{
    st_application_startup_allocator_t allocator;
    st_application_startup_state_t *state;
    st_application_startup_status_t status;

    if (context == NULL || options == NULL || context->initialized
            || context->state != NULL
            || !normalize_allocator(options->allocator, &allocator)) {
        if (context != NULL) {
            context->status = ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT;
        }
        return ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT;
    }
    if (!launch_is_valid(options->launch)) {
        context->status = ST_APPLICATION_STARTUP_ERR_INVALID_LAUNCH;
        return context->status;
    }
    state = allocator.allocate(allocator.user, sizeof(*state));
    if (state == NULL) {
        context->status = ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY;
        return context->status;
    }
    memset(state, 0, sizeof(*state));
    state->allocator = allocator;
    state->launch = options->launch;
    state->metadata = options->launch->metadata;
    context->state = state;
    context->abi_version = ST_APPLICATION_STARTUP_ABI_VERSION;

    status = initialize_image(state, options);
    if (status == ST_APPLICATION_STARTUP_OK) {
        status = initialize_bootstrap(state);
    }
    if (status == ST_APPLICATION_STARTUP_OK) {
        status = initialize_primitives(state, options);
    }
    if (status == ST_APPLICATION_STARTUP_OK) {
        status = initialize_thread(state);
    }
    if (status != ST_APPLICATION_STARTUP_OK) {
        context->status = status;
        (void)destroy_state(context, false);
        context->status = status;
        return status;
    }
    context->initialized = true;
    context->status = ST_APPLICATION_STARTUP_OK;
    return context->status;
}

st_application_startup_status_t st_application_startup_run(
    st_application_startup_context_t *context, st_value_t *result_out)
{
    st_application_startup_state_t *state;
    st_lookup_result_t lookup;
    st_control_pending_info_t pending;
    st_value_t receiver = ST_VALUE_INVALID;
    st_value_t result = ST_VALUE_INVALID;
    st_value_t *roots = NULL;
    StFrame frame;
    size_t root_bytes;

    if (result_out != NULL) {
        *result_out = ST_VALUE_INVALID;
    }
    if (context == NULL || result_out == NULL || !context->initialized
            || context->running || context->state == NULL) {
        return ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT;
    }
    state = context->state;
    context->running = true;
    if (st_heap_allocate(
            st_image_runtime_heap(&state->image),
            state->launch->entry_runtime_class_id,
            state->launch->entry_default_shape_id, 0u, 0u, 0u, &receiver)
            != ST_HEAP_OK
            || st_lookup_inherited(
                &state->lookup, state->launch->entry_runtime_class_id,
                state->launch->entry_selector_id, &lookup)
                != ST_LOOKUP_FOUND
            || lookup.binding == NULL
            || !st_method_binding_is_valid(lookup.binding)
            || lookup.binding->descriptor->arity != 0u) {
        context->running = false;
        context->status = ST_APPLICATION_STARTUP_ERR_ENTRY;
        return context->status;
    }
    if (!multiply_size(
            lookup.binding->descriptor->frame_root_capacity,
            sizeof(*roots), &root_bytes)) {
        context->running = false;
        context->status = ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY;
        return context->status;
    }
    if (root_bytes != 0u) {
        roots = state->allocator.allocate(state->allocator.user, root_bytes);
        if (roots == NULL
                || st_aot_frame_roots_initialize(
                    roots, lookup.binding->descriptor->frame_root_capacity)
                    != ST_AOT_SEND_OK) {
            state->allocator.deallocate(state->allocator.user, roots);
            context->running = false;
            context->status = ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY;
            return context->status;
        }
    }
    frame = (StFrame) {
        .thread = &state->thread,
        .method = lookup.binding->descriptor,
        .receiver = receiver,
        .roots = roots,
        .root_count = lookup.binding->descriptor->frame_root_capacity
    };
    result = lookup.binding->code(&frame);
    state->allocator.deallocate(state->allocator.user, roots);
    if (!st_value_has_valid_encoding(result)
            || (st_value_kind(result) == ST_VALUE_OBJECT
                && !st_heap_contains(
                    st_image_runtime_heap(&state->image), result))) {
        context->running = false;
        context->status = ST_APPLICATION_STARTUP_ERR_INVALID_RESULT;
        return context->status;
    }
    if (st_control_pending_get(&state->control, &pending) != ST_CONTROL_OK) {
        context->running = false;
        context->status = ST_APPLICATION_STARTUP_ERR_CONTROL;
        return context->status;
    }
    if (pending.kind != ST_CONTROL_PENDING_NONE) {
        st_application_startup_status_t status =
            pending.kind == ST_CONTROL_PENDING_EXCEPTION
                ? ST_APPLICATION_STARTUP_ERR_UNHANDLED_EXCEPTION
                : ST_APPLICATION_STARTUP_ERR_ESCAPED_CONTROL;
        if (st_control_pending_clear(&state->control) != ST_CONTROL_OK) {
            status = ST_APPLICATION_STARTUP_ERR_CONTROL;
        }
        context->running = false;
        context->status = status;
        return status;
    }
    *result_out = result;
    context->running = false;
    context->status = ST_APPLICATION_STARTUP_OK;
    return context->status;
}

st_application_startup_status_t st_application_startup_exit_code(
    st_value_t result, int *exit_code_out)
{
    int64_t integer;

    if (exit_code_out == NULL) {
        return ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT;
    }
    *exit_code_out = 0;
    if (!st_value_to_small_integer(result, &integer)
            || integer < 0 || integer > 255) {
        return ST_APPLICATION_STARTUP_ERR_INVALID_RESULT;
    }
    *exit_code_out = (int)integer;
    return ST_APPLICATION_STARTUP_OK;
}

st_application_startup_status_t st_application_startup_context_destroy(
    st_application_startup_context_t *context)
{
    st_application_startup_status_t status;

    if (context == NULL || context->running) {
        return ST_APPLICATION_STARTUP_ERR_BUSY;
    }
    status = destroy_state(context, true);
    if (status != ST_APPLICATION_STARTUP_OK) {
        context->status = status;
    }
    return status;
}

const char *st_application_startup_status_string(
    st_application_startup_status_t status)
{
    switch (status) {
    case ST_APPLICATION_STARTUP_OK: return "ok";
    case ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_APPLICATION_STARTUP_ERR_INVALID_LAUNCH:
        return "invalid launch descriptor";
    case ST_APPLICATION_STARTUP_ERR_INVALID_METADATA:
        return "invalid image metadata";
    case ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_APPLICATION_STARTUP_ERR_IMAGE: return "image bootstrap failed";
    case ST_APPLICATION_STARTUP_ERR_LOOKUP: return "lookup initialization failed";
    case ST_APPLICATION_STARTUP_ERR_SYMBOLS: return "Symbol initialization failed";
    case ST_APPLICATION_STARTUP_ERR_BOOTSTRAP:
        return "managed image bootstrap failed";
    case ST_APPLICATION_STARTUP_ERR_PRIMITIVES:
        return "primitive context initialization failed";
    case ST_APPLICATION_STARTUP_ERR_CONTROL:
        return "control protocol failed";
    case ST_APPLICATION_STARTUP_ERR_CLOSURES:
        return "closure context initialization failed";
    case ST_APPLICATION_STARTUP_ERR_THREAD:
        return "AOT thread initialization failed";
    case ST_APPLICATION_STARTUP_ERR_DNU: return "DNU initialization failed";
    case ST_APPLICATION_STARTUP_ERR_ENTRY: return "entry method failed";
    case ST_APPLICATION_STARTUP_ERR_UNHANDLED_EXCEPTION:
        return "unhandled Smalltalk exception";
    case ST_APPLICATION_STARTUP_ERR_ESCAPED_CONTROL:
        return "non-local return escaped the entry activation";
    case ST_APPLICATION_STARTUP_ERR_INVALID_RESULT:
        return "invalid application result";
    case ST_APPLICATION_STARTUP_ERR_BUSY: return "startup context is busy";
    case ST_APPLICATION_STARTUP_ERR_CLEANUP: return "runtime cleanup failed";
    }
    return "unknown application startup status";
}
