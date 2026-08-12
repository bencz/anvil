#ifndef ANVIL_SMALLTALK_APPLICATION_LAUNCH_H
#define ANVIL_SMALLTALK_APPLICATION_LAUNCH_H

#include "st_aot_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_APPLICATION_LAUNCH_MAGIC UINT64_C(0x414e5653544c4348)
#define ST_APPLICATION_LAUNCH_ABI_VERSION UINT32_C(1)
#define ST_APPLICATION_LAUNCH_RESERVED_COUNT 8u

/* Target-native, immutable launch contract.  Every identity is resolved and
 * cross-checked by the AOT product compiler.  Startup performs no class,
 * selector or slot lookup by source-level name. */
typedef struct {
    uint64_t magic;
    uint32_t abi_version;
    uint32_t descriptor_size;
    uint32_t metadata_abi_version;
    uint32_t flags;
    const st_image_metadata_descriptor_t *metadata;

    uint32_t expected_global_count;
    uint32_t expected_string_literal_count;
    uint32_t expected_selector_count;

    uint32_t entry_entity_id;
    uint32_t entry_runtime_class_id;
    uint32_t entry_default_shape_id;
    uint32_t entry_selector_id;
    uint32_t entry_arity;
    uint32_t transcript_semantic_external_id;
    uint32_t transcript_runtime_index;

    uint32_t nil_class_id;
    uint32_t false_class_id;
    uint32_t true_class_id;
    uint32_t small_integer_class_id;
    uint32_t character_class_id;

    uint32_t object_entity_id;
    uint32_t class_object_layout_entity_id;
    uint32_t metaclass_entity_id;
    uint32_t integer_entity_id;
    uint32_t small_integer_entity_id;

    uint32_t array_entity_id;
    uint32_t array_class_id;
    uint32_t array_shape_id;
    uint32_t method_dictionary_class_id;
    uint32_t method_dictionary_shape_id;

    uint32_t symbol_class_id;
    uint32_t symbol_uint8_shape_id;
    uint32_t symbol_uint16_shape_id;
    uint32_t symbol_uint32_shape_id;
    uint32_t compiled_method_class_id;
    uint32_t compiled_method_shape_id;

    uint32_t string_class_id;
    uint32_t string_uint8_shape_id;
    uint32_t string_uint16_shape_id;
    uint32_t string_uint32_shape_id;
    uint32_t external_stream_class_id;
    uint32_t external_stream_shape_id;
    uint32_t external_stream_descriptor_slot;

    uint32_t block_class_id;
    uint32_t block_shape_id;
    uint32_t closure_cell_class_id;
    uint32_t closure_cell_shape_id;

    uint32_t message_entity_id;
    uint32_t message_class_id;
    uint32_t message_shape_id;
    uint32_t message_selector_slot;
    uint32_t message_arguments_slot;
    uint32_t does_not_understand_selector_id;

    uint32_t large_positive_class_id;
    uint32_t large_positive_shape_id;
    uint32_t large_negative_class_id;
    uint32_t large_negative_shape_id;
    uint32_t boxed_float64_class_id;
    uint32_t boxed_float64_shape_id;

    uint32_t reserved[ST_APPLICATION_LAUNCH_RESERVED_COUNT];
} st_application_launch_descriptor_t;

/* Graph identities and physical slots resolved by the product frontend.
 * Runtime class/shape IDs are deliberately absent: the emitter derives and
 * authenticates them against the exact st_aot_compile layout. */
typedef struct {
    st_class_graph_id_t entry_entity_id;
    st_selector_id_t entry_selector_id;
    st_class_graph_id_t nil_entity_id;
    st_class_graph_id_t false_entity_id;
    st_class_graph_id_t true_entity_id;
    st_class_graph_id_t character_entity_id;
    st_class_graph_id_t object_entity_id;
    st_class_graph_id_t class_entity_id;
    st_class_graph_id_t metaclass_entity_id;
    st_class_graph_id_t integer_entity_id;
    st_class_graph_id_t small_integer_entity_id;
    st_class_graph_id_t array_entity_id;
    st_class_graph_id_t method_dictionary_entity_id;
    st_class_graph_id_t symbol_entity_id;
    st_class_graph_id_t compiled_method_entity_id;
    st_class_graph_id_t string_entity_id;
    st_class_graph_id_t external_stream_entity_id;
    uint32_t external_stream_descriptor_slot;
    st_class_graph_id_t block_entity_id;
    st_class_graph_id_t closure_cell_entity_id;
    st_class_graph_id_t message_entity_id;
    uint32_t message_selector_slot;
    uint32_t message_arguments_slot;
    st_selector_id_t does_not_understand_selector_id;
    st_class_graph_id_t large_positive_entity_id;
    st_class_graph_id_t large_negative_entity_id;
    st_class_graph_id_t boxed_float64_entity_id;
    uint32_t transcript_runtime_index;
} st_application_launch_plan_t;

typedef enum {
    ST_APPLICATION_LAUNCH_OK = 0,
    ST_APPLICATION_LAUNCH_ERR_INVALID_ARGUMENT,
    ST_APPLICATION_LAUNCH_ERR_INVALID_PLAN,
    ST_APPLICATION_LAUNCH_ERR_OUT_OF_MEMORY,
    ST_APPLICATION_LAUNCH_ERR_ANVIL
} st_application_launch_status_t;

typedef struct {
    st_application_launch_status_t status;
    anvil_module_t *module;
    char symbol[ST_AOT_SYMBOL_PREFIX_MAX + sizeof("_launch_descriptor")];
    size_t symbol_length;
} st_application_launch_result_t;

void st_application_launch_result_init(st_application_launch_result_t *result);
void st_application_launch_result_destroy(st_application_launch_result_t *result);

st_application_launch_status_t st_application_launch_emit(
    st_application_launch_result_t *result,
    const st_aot_compile_result_t *compiled,
    const st_class_graph_result_t *graph,
    const st_selector_table_t *selectors,
    const st_application_launch_plan_t *plan);

const char *st_application_launch_status_string(
    st_application_launch_status_t status);

#ifdef __cplusplus
}
#endif

#endif
