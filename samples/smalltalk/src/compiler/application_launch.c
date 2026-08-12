#include "st_application_launch.h"

#include "st_image_runtime.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define LAUNCH_WORD_COUNT 62u
#define LAUNCH_FIELD_COUNT (1u + 4u + 1u + LAUNCH_WORD_COUNT)

static const st_class_graph_entity_t *class_entity(
    const st_class_graph_result_t *graph, st_class_graph_id_t id)
{
    const st_class_graph_entity_t *entity = st_class_graph_entity(graph, id);
    return entity != NULL && entity->kind == ST_CLASS_GRAPH_CLASS
        ? entity : NULL;
}

static uint32_t runtime_class(
    const st_aot_compile_result_t *compiled, st_class_graph_id_t entity)
{
    return st_image_layout_runtime_class_id(&compiled->layout, entity);
}

static uint32_t shape_for(
    const st_aot_compile_result_t *compiled, st_class_graph_id_t entity,
    st_image_layout_recipe_t recipe, bool require_default)
{
    uint32_t class_id = runtime_class(compiled, entity);
    const st_image_runtime_class_layout_t *class_layout =
        st_image_layout_class(&compiled->layout, class_id);
    if (class_layout == NULL) return 0u;
    for (size_t index = 0u; index < class_layout->shape_count; index++) {
        const st_image_runtime_shape_layout_t *shape =
            &compiled->layout.shapes[class_layout->shape_offset + index];
        if (shape->recipe == recipe
                && (!require_default || shape->is_default)) {
            return shape->runtime_shape_id;
        }
    }
    return 0u;
}

static bool declared_slot(
    const st_class_graph_result_t *graph, st_class_graph_id_t entity,
    uint32_t slot)
{
    for (size_t index = 0u; index < graph->instance_slot_count; index++) {
        const st_class_graph_slot_t *candidate = &graph->instance_slots[index];
        if (candidate->declaring_class == entity && candidate->slot == slot) {
            return true;
        }
    }
    return false;
}

static bool selector_has_arity(
    const st_selector_table_t *selectors, st_selector_id_t id,
    uint32_t arity)
{
    const st_selector_t *selector = st_selector_get(selectors, id);
    return selector != NULL && selector->arity == arity;
}

void st_application_launch_result_init(st_application_launch_result_t *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void st_application_launch_result_destroy(st_application_launch_result_t *result)
{
    if (result == NULL) return;
    if (result->module != NULL) anvil_module_destroy(result->module);
    memset(result, 0, sizeof(*result));
}

st_application_launch_status_t st_application_launch_emit(
    st_application_launch_result_t *result,
    const st_aot_compile_result_t *compiled,
    const st_class_graph_result_t *graph,
    const st_selector_table_t *selectors,
    const st_application_launch_plan_t *plan)
{
    st_application_launch_descriptor_t descriptor;
    anvil_type_t *fields[LAUNCH_FIELD_COUNT];
    anvil_value_t *values[LAUNCH_FIELD_COUNT];
    anvil_module_t *module = NULL;
    anvil_value_t *metadata_global, *initializer, *global;
    anvil_type_t *descriptor_type;
    char metadata_symbol[ST_AOT_SYMBOL_PREFIX_MAX + sizeof("_descriptor")];
    uint32_t *words;
    size_t field = 0u;
    int length;

    _Static_assert(offsetof(st_application_launch_descriptor_t,
                            expected_global_count) == 32u,
                   "launch header ABI changed");
    _Static_assert(sizeof(st_application_launch_descriptor_t) == 280u,
                   "launch descriptor ABI changed");
    if (result == NULL || result->module != NULL || compiled == NULL
            || compiled->status != ST_AOT_COMPILE_OK
            || compiled->context == NULL || graph == NULL || plan == NULL
            || !st_class_graph_succeeded(graph)
            || !st_selector_table_is_frozen(selectors)
            || compiled->global_count > UINT32_MAX
            || compiled->string_literal_count > UINT32_MAX
            || st_selector_count(selectors) > UINT32_MAX) {
        if (result != NULL) result->status =
            ST_APPLICATION_LAUNCH_ERR_INVALID_ARGUMENT;
        return ST_APPLICATION_LAUNCH_ERR_INVALID_ARGUMENT;
    }

    const st_class_graph_id_t roles[] = {
        plan->entry_entity_id, plan->nil_entity_id, plan->false_entity_id,
        plan->true_entity_id, plan->character_entity_id,
        plan->object_entity_id, plan->class_entity_id,
        plan->metaclass_entity_id, plan->integer_entity_id,
        plan->small_integer_entity_id, plan->array_entity_id,
        plan->method_dictionary_entity_id, plan->symbol_entity_id,
        plan->compiled_method_entity_id, plan->string_entity_id,
        plan->external_stream_entity_id, plan->block_entity_id,
        plan->closure_cell_entity_id, plan->message_entity_id,
        plan->large_positive_entity_id, plan->large_negative_entity_id,
        plan->boxed_float64_entity_id
    };
    for (size_t index = 0u; index < sizeof(roles) / sizeof(roles[0]); index++)
        if (class_entity(graph, roles[index]) == NULL
                || runtime_class(compiled, roles[index]) == 0u)
            goto invalid_plan;
    if (!selector_has_arity(selectors, plan->entry_selector_id, 0u)
            || !selector_has_arity(
                selectors, plan->does_not_understand_selector_id, 1u)
            || !declared_slot(graph, plan->external_stream_entity_id,
                              plan->external_stream_descriptor_slot)
            || !declared_slot(graph, plan->message_entity_id,
                              plan->message_selector_slot)
            || !declared_slot(graph, plan->message_entity_id,
                              plan->message_arguments_slot))
        goto invalid_plan;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = ST_APPLICATION_LAUNCH_MAGIC;
    descriptor.abi_version = ST_APPLICATION_LAUNCH_ABI_VERSION;
    descriptor.descriptor_size = (uint32_t)sizeof(descriptor);
    descriptor.metadata_abi_version = ST_IMAGE_METADATA_ABI_VERSION;
    descriptor.expected_global_count = (uint32_t)compiled->global_count;
    descriptor.expected_string_literal_count =
        (uint32_t)compiled->string_literal_count;
    descriptor.expected_selector_count = (uint32_t)st_selector_count(selectors);
    descriptor.entry_entity_id = plan->entry_entity_id;
    descriptor.entry_runtime_class_id = runtime_class(compiled, plan->entry_entity_id);
    descriptor.entry_default_shape_id = shape_for(
        compiled, plan->entry_entity_id, ST_IMAGE_LAYOUT_FIXED_POINTERS, true);
    descriptor.entry_selector_id = plan->entry_selector_id;
    descriptor.entry_arity = 0u;
    descriptor.transcript_semantic_external_id = ST_IMAGE_EXTERNAL_ID_TRANSCRIPT;
    descriptor.transcript_runtime_index = plan->transcript_runtime_index;

#define SET_CLASS(field_name, entity_name) \
    descriptor.field_name = runtime_class(compiled, plan->entity_name)
    SET_CLASS(nil_class_id, nil_entity_id);
    SET_CLASS(false_class_id, false_entity_id);
    SET_CLASS(true_class_id, true_entity_id);
    SET_CLASS(small_integer_class_id, small_integer_entity_id);
    SET_CLASS(character_class_id, character_entity_id);
    descriptor.object_entity_id = plan->object_entity_id;
    descriptor.class_object_layout_entity_id = plan->class_entity_id;
    descriptor.metaclass_entity_id = plan->metaclass_entity_id;
    descriptor.integer_entity_id = plan->integer_entity_id;
    descriptor.small_integer_entity_id = plan->small_integer_entity_id;
    descriptor.array_entity_id = plan->array_entity_id;
    SET_CLASS(array_class_id, array_entity_id);
    descriptor.array_shape_id = shape_for(compiled, plan->array_entity_id, ST_IMAGE_LAYOUT_INDEXED_VALUES, true);
    SET_CLASS(method_dictionary_class_id, method_dictionary_entity_id);
    descriptor.method_dictionary_shape_id = shape_for(compiled, plan->method_dictionary_entity_id, ST_IMAGE_LAYOUT_INDEXED_VALUES, true);
    SET_CLASS(symbol_class_id, symbol_entity_id);
    descriptor.symbol_uint8_shape_id = shape_for(compiled, plan->symbol_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT8, true);
    descriptor.symbol_uint16_shape_id = shape_for(compiled, plan->symbol_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT16, false);
    descriptor.symbol_uint32_shape_id = shape_for(compiled, plan->symbol_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT32, false);
    SET_CLASS(compiled_method_class_id, compiled_method_entity_id);
    descriptor.compiled_method_shape_id = shape_for(compiled, plan->compiled_method_entity_id, ST_IMAGE_LAYOUT_FIXED_POINTERS, true);
    SET_CLASS(string_class_id, string_entity_id);
    descriptor.string_uint8_shape_id = shape_for(compiled, plan->string_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT8, true);
    descriptor.string_uint16_shape_id = shape_for(compiled, plan->string_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT16, false);
    descriptor.string_uint32_shape_id = shape_for(compiled, plan->string_entity_id, ST_IMAGE_LAYOUT_INDEXED_UINT32, false);
    SET_CLASS(external_stream_class_id, external_stream_entity_id);
    descriptor.external_stream_shape_id = shape_for(compiled, plan->external_stream_entity_id, ST_IMAGE_LAYOUT_FIXED_POINTERS, true);
    descriptor.external_stream_descriptor_slot = plan->external_stream_descriptor_slot;
    SET_CLASS(block_class_id, block_entity_id);
    descriptor.block_shape_id = shape_for(compiled, plan->block_entity_id, ST_IMAGE_LAYOUT_CLOSURE, true);
    SET_CLASS(closure_cell_class_id, closure_cell_entity_id);
    descriptor.closure_cell_shape_id = shape_for(compiled, plan->closure_cell_entity_id, ST_IMAGE_LAYOUT_CELL, true);
    descriptor.message_entity_id = plan->message_entity_id;
    SET_CLASS(message_class_id, message_entity_id);
    descriptor.message_shape_id = shape_for(compiled, plan->message_entity_id, ST_IMAGE_LAYOUT_FIXED_POINTERS, true);
    descriptor.message_selector_slot = plan->message_selector_slot;
    descriptor.message_arguments_slot = plan->message_arguments_slot;
    descriptor.does_not_understand_selector_id = plan->does_not_understand_selector_id;
    SET_CLASS(large_positive_class_id, large_positive_entity_id);
    descriptor.large_positive_shape_id = shape_for(compiled, plan->large_positive_entity_id, ST_IMAGE_LAYOUT_LARGE_INTEGER, true);
    SET_CLASS(large_negative_class_id, large_negative_entity_id);
    descriptor.large_negative_shape_id = shape_for(compiled, plan->large_negative_entity_id, ST_IMAGE_LAYOUT_LARGE_INTEGER, true);
    SET_CLASS(boxed_float64_class_id, boxed_float64_entity_id);
    descriptor.boxed_float64_shape_id = shape_for(compiled, plan->boxed_float64_entity_id, ST_IMAGE_LAYOUT_BOXED_FLOAT64, true);
#undef SET_CLASS

    if (descriptor.entry_default_shape_id == 0u
            || descriptor.array_shape_id == 0u
            || descriptor.method_dictionary_shape_id == 0u
            || descriptor.symbol_uint8_shape_id == 0u
            || descriptor.symbol_uint16_shape_id == 0u
            || descriptor.symbol_uint32_shape_id == 0u
            || descriptor.compiled_method_shape_id == 0u
            || descriptor.string_uint8_shape_id == 0u
            || descriptor.string_uint16_shape_id == 0u
            || descriptor.string_uint32_shape_id == 0u
            || descriptor.external_stream_shape_id == 0u
            || descriptor.block_shape_id == 0u
            || descriptor.closure_cell_shape_id == 0u
            || descriptor.message_shape_id == 0u
            || descriptor.large_positive_shape_id == 0u
            || descriptor.large_negative_shape_id == 0u
            || descriptor.boxed_float64_shape_id == 0u) {
        goto invalid_plan;
    }
    words = (uint32_t *)((unsigned char *)&descriptor + 32u);
    for (size_t index = 0u; index < compiled->global_count; index++)
        if (compiled->globals[index].semantic_external_id == ST_IMAGE_EXTERNAL_ID_TRANSCRIPT
                && compiled->globals[index].runtime_index == plan->transcript_runtime_index)
            goto transcript_found;
    goto invalid_plan;
transcript_found:
    length = snprintf(metadata_symbol, sizeof(metadata_symbol), "%s_descriptor", compiled->provenance.symbol_prefix);
    if (length < 0 || (size_t)length >= sizeof(metadata_symbol)) goto invalid_plan;
    length = snprintf(result->symbol, sizeof(result->symbol), "%s_launch_descriptor", compiled->provenance.symbol_prefix);
    if (length < 0 || (size_t)length >= sizeof(result->symbol)) goto invalid_plan;
    result->symbol_length = (size_t)length;

    module = anvil_module_create(compiled->context, "smalltalk.application.launch");
    fields[field++] = anvil_type_u64(compiled->context);
    for (size_t index = 0u; index < 4u; index++) fields[field++] = anvil_type_u32(compiled->context);
    fields[field++] = anvil_type_ptr(compiled->context, anvil_type_u8(compiled->context));
    for (size_t index = 0u; index < LAUNCH_WORD_COUNT; index++) fields[field++] = anvil_type_u32(compiled->context);
    descriptor_type = module != NULL ? anvil_type_literal_struct(compiled->context, fields, field, false) : NULL;
    if (descriptor_type == NULL
            || anvil_type_size(descriptor_type) != sizeof(descriptor)
            || anvil_type_struct_field_offset(descriptor_type, 5u)
                != offsetof(st_application_launch_descriptor_t, metadata)
            || anvil_type_struct_field_offset(descriptor_type, 6u)
                != offsetof(st_application_launch_descriptor_t,
                            expected_global_count)) {
        goto anvil_failure;
    }
    metadata_global = descriptor_type != NULL ? anvil_module_declare_global(module, metadata_symbol, anvil_type_u8(compiled->context), ANVIL_LINK_EXTERNAL) : NULL;
    field = 0u;
    values[field++] = anvil_const_u64(compiled->context, descriptor.magic);
    values[field++] = anvil_const_u32(compiled->context, descriptor.abi_version);
    values[field++] = anvil_const_u32(compiled->context, descriptor.descriptor_size);
    values[field++] = anvil_const_u32(compiled->context, descriptor.metadata_abi_version);
    values[field++] = anvil_const_u32(compiled->context, descriptor.flags);
    values[field++] = metadata_global != NULL ? anvil_const_symbol_addr(metadata_global) : NULL;
    for (size_t index = 0u; index < LAUNCH_WORD_COUNT; index++) values[field++] = anvil_const_u32(compiled->context, words[index]);
    initializer = descriptor_type != NULL ? anvil_const_struct(compiled->context, descriptor_type, values, field) : NULL;
    global = initializer != NULL ? anvil_module_add_global(module, result->symbol, descriptor_type, ANVIL_LINK_EXTERNAL) : NULL;
    if (global == NULL || !anvil_global_set_initializer(global, initializer)) goto anvil_failure;
    {
        char verify[256];
        if (!anvil_module_verify(module, verify, sizeof(verify))) goto anvil_failure;
    }
    result->module = module;
    result->status = ST_APPLICATION_LAUNCH_OK;
    return result->status;

invalid_plan:
    result->status = ST_APPLICATION_LAUNCH_ERR_INVALID_PLAN;
    return result->status;
anvil_failure:
    if (module != NULL) anvil_module_destroy(module);
    result->symbol[0] = '\0';
    result->symbol_length = 0u;
    result->status = anvil_ctx_get_last_error(compiled->context) == ANVIL_ERR_NOMEM
        ? ST_APPLICATION_LAUNCH_ERR_OUT_OF_MEMORY
        : ST_APPLICATION_LAUNCH_ERR_ANVIL;
    return result->status;
}

const char *st_application_launch_status_string(st_application_launch_status_t status)
{
    switch (status) {
    case ST_APPLICATION_LAUNCH_OK: return "ok";
    case ST_APPLICATION_LAUNCH_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_APPLICATION_LAUNCH_ERR_INVALID_PLAN: return "invalid launch plan";
    case ST_APPLICATION_LAUNCH_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_APPLICATION_LAUNCH_ERR_ANVIL: return "Anvil module error";
    }
    return "unknown application launch status";
}
