#include "st_lower.h"

#include "st_dispatch.h"
#include "st_core_primitives.h"
#include "st_control_bridge.h"
#include "st_closure_bridge.h"
#include "st_heap_primitive_bridge.h"
#include "st_image_runtime.h"
#include "st_primitive_bridge.h"
#include "st_send_bridge.h"
#include "st_value.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    anvil_value_t *value;
    bool address;
    bool cell;
    uint32_t root_slot;
} binding_location_t;

typedef struct {
    st_lower_result_t *result;
    const st_sema_result_t *sema;
    const st_ast_node_t *method;
    const st_class_graph_method_t *graph_method;
    const st_selector_table_t *selectors;
    const st_primitive_binding_t *primitive_binding;
    const st_lower_global_binding_t *globals;
    size_t global_count;
    size_t depth;
    size_t send_count;
    size_t safepoint_count;
    bool heap_primitive;
    bool runtime_symbol_primitive;
    bool runtime_control_primitive;
    size_t maximum_send_arity;
    size_t scratch_depth;
    size_t maximum_scratch_roots;
    size_t closure_create_count;
    size_t closure_call_count;
    size_t string_literal_count;
    size_t string_literal_bytes;
    bool uses_image_runtime;
    size_t image_load_count;
    size_t heap_access_count;
    bool has_cascade;
} preflight_t;

typedef struct {
    anvil_value_t *value;
    bool terminated;
} lowered_value_t;

typedef struct {
    st_lower_result_t *result;
    anvil_ctx_t *ctx;
    anvil_module_t *module;
    anvil_func_t *function;
    anvil_type_t *frame_type;
    anvil_type_t *u64;
    anvil_type_t *u32;
    anvil_type_t *byte_ptr;
    anvil_type_t *value_ptr;
    anvil_type_t *method_ptr;
    anvil_type_t *send_site_type;
    anvil_type_t *send_site_ptr;
    anvil_type_t *send_target_type;
    anvil_type_t *send_target_ptr;
    anvil_func_t *frame_validate_function;
    anvil_func_t *send_resolve_function;
    anvil_func_t *send_failure_function;
    anvil_func_t *frame_roots_initialize_function;
    anvil_func_t *primitive_execute_function;
    anvil_func_t *primitive_fatal_function;
    anvil_func_t *heap_primitive_execute_function;
    anvil_func_t *heap_primitive_fatal_function;
    anvil_func_t *runtime_primitive_function;
    anvil_func_t *runtime_primitive_fatal_function;
    anvil_func_t *image_global_load_function;
    anvil_func_t *image_literal_load_function;
    anvil_func_t *image_fatal_function;
    anvil_func_t *control_enter_function;
    anvil_func_t *control_leave_function;
    anvil_func_t *control_pending_function;
    anvil_func_t *control_nlr_function;
    anvil_func_t *control_fatal_function;
    anvil_type_t *closure_target_type;
    anvil_type_t *closure_target_ptr;
    anvil_func_t *closure_create_function;
    anvil_func_t *closure_resolve_function;
    anvil_func_t *closure_capture_load_function;
    anvil_func_t *closure_cell_create_function;
    anvil_func_t *closure_cell_load_function;
    anvil_func_t *closure_cell_store_function;
    anvil_func_t *closure_fatal_function;
    anvil_value_t **closure_descriptor_globals;
    anvil_value_t *frame;
    anvil_value_t *closure;
    anvil_value_t *self;
    anvil_value_t *roots;
    const st_sema_result_t *sema;
    const st_class_graph_method_t *graph_method;
    const st_selector_table_t *selectors;
    const st_primitive_binding_t *primitive_binding;
    const char *runtime_primitive_symbol;
    const st_lower_global_binding_t *globals;
    size_t global_count;
    const uint32_t *runtime_class_ids_by_entity;
    size_t runtime_class_id_count;
    bool heap_primitive;
    bool runtime_symbol_primitive;
    bool control_scope;
    bool establish_home;
    anvil_value_t *control_scope_storage;
    anvil_value_t *control_return_address;
    anvil_value_t *control_leave_result_address;
    anvil_block_t *control_epilogue;
    uint32_t control_return_root;
    binding_location_t *locations;
    size_t locations_count;
    anvil_value_t **argument_values;
    size_t argument_capacity;
    uint32_t required_root_capacity;
    uint32_t base_root_count;
    uint32_t scratch_root_offset;
    uint32_t scratch_depth;
    uint32_t next_safepoint_id;
    size_t next_send_site;
    size_t next_control_block;
    const st_ast_node_t *current_block;
    const st_sema_block_t *current_block_info;
    st_lower_block_artifact_t *block_artifact;
    st_lower_block_artifact_t *block_artifacts;
    size_t block_count;
    st_lower_string_literal_artifact_t *string_literals;
    unsigned char *string_literal_bytes;
    uint32_t literal_base_index;
    size_t string_literal_capacity;
    size_t string_literal_byte_capacity;
    size_t next_string_literal;
    size_t next_string_byte;
    size_t next_image_load;
    size_t next_heap_access;
} lowerer_t;

typedef struct {
    st_lower_allocator_t allocator;
    st_lower_root_map_t *root_maps;
    uint64_t *bitmap;
    st_lower_block_artifact_t *blocks;
    st_aot_capture_descriptor_t **block_captures;
    char **block_symbols;
    size_t block_count;
    st_lower_string_literal_artifact_t *string_literals;
    unsigned char *string_literal_bytes;
} lower_result_impl_t;

enum { ST_LOWER_MAX_NESTING = 512 };

_Static_assert(sizeof(st_value_t) == sizeof(uint64_t),
               "Smalltalk AOT ABI requires a 64-bit StValue");
_Static_assert(sizeof(StFrame) == 72u,
               "Smalltalk AOT lowering and runtime disagree on StFrame");
_Static_assert(offsetof(StFrame, receiver) == 32u,
               "Smalltalk AOT lowering and runtime disagree on receiver");
_Static_assert(offsetof(StFrame, argv) == 40u,
               "Smalltalk AOT lowering and runtime disagree on argv");
_Static_assert(sizeof(st_pic_slot_t) == 32u,
               "Smalltalk AOT lowering and PIC ABI disagree");
_Static_assert(offsetof(st_send_site_t, slots) == 16u,
               "Smalltalk AOT lowering and send-site ABI disagree");
_Static_assert(offsetof(st_send_site_t, initialized) == 144u,
               "Smalltalk AOT lowering and send-site ABI disagree");
_Static_assert(sizeof(st_send_site_t) == 152u,
               "Smalltalk AOT lowering and send-site ABI disagree");
_Static_assert(ST_AOT_CONTROL_SCOPE_SIZE == sizeof(st_control_scope_t),
               "Smalltalk control scope ABI size changed");

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

static bool string_is(st_ast_string_t string, const char *text);

/* Exact-Boolean protocol sends are the one deliberately non-escaping block
 * form.  Sema still records those lexical blocks, so keep their ordinals in
 * the semantic stream while omitting closure descriptors/code artifacts. */
static bool node_has_inline_boolean_argument(
    const st_ast_node_t *node, const st_ast_node_t *target)
{
    if (!node) {
        return false;
    }
    switch (node->kind) {
    case ST_AST_METHOD:
        return node_has_inline_boolean_argument(node->as.method.body, target);
    case ST_AST_BLOCK:
        for (size_t index = 0u; index < node->as.block.expressions.count;
             index++)
            if (node_has_inline_boolean_argument(
                    node->as.block.expressions.items[index], target))
                return true;
        return false;
    case ST_AST_EXPRESSION: {
        const st_ast_expression_t *expression = &node->as.expression;
        if (expression->messages.count == 1u
                && expression->receiver
                && (expression->receiver->kind == ST_AST_TRUE
                    || expression->receiver->kind == ST_AST_FALSE)) {
            const st_ast_node_t *message = expression->messages.items[0];
            if (message && message->kind == ST_AST_MESSAGE
                    && (string_is(message->as.message.selector, "ifTrue:")
                        || string_is(message->as.message.selector, "ifFalse:")
                        || string_is(message->as.message.selector,
                                     "ifTrue:ifFalse:"))) {
                for (size_t index = 0u;
                     index < message->as.message.arguments.count; index++)
                    if (message->as.message.arguments.items[index] == target)
                        return true;
            }
        }
        if (node_has_inline_boolean_argument(expression->receiver, target))
            return true;
        for (size_t index = 0u; index < expression->messages.count; index++)
            if (node_has_inline_boolean_argument(
                    expression->messages.items[index], target))
                return true;
        return false;
    }
    case ST_AST_MESSAGE:
        for (size_t index = 0u; index < node->as.message.arguments.count;
             index++)
            if (node_has_inline_boolean_argument(
                    node->as.message.arguments.items[index], target))
                return true;
        return false;
    default:
        return false;
    }
}

static bool sema_block_needs_artifact(const st_sema_result_t *sema,
                                      const st_ast_node_t *method,
                                      size_t semantic_index)
{
    return semantic_index < sema->block_count
        && !node_has_inline_boolean_argument(
            method, sema->blocks[semantic_index].node);
}

static bool sema_block_artifact_index(const st_sema_result_t *sema,
                                      const st_ast_node_t *method,
                                      size_t semantic_index,
                                      size_t *artifact_index_out)
{
    size_t artifact_index = 0u;
    if (!artifact_index_out || semantic_index >= sema->block_count
            || !sema_block_needs_artifact(sema, method, semantic_index))
        return false;
    for (size_t index = 0u; index < semantic_index; index++) {
        if (sema_block_needs_artifact(sema, method, index)) artifact_index++;
    }
    *artifact_index_out = artifact_index;
    return true;
}

static bool sema_block_needs_home(const st_sema_result_t *sema,
                                  size_t semantic_index)
{
    if (semantic_index >= sema->block_count) {
        return false;
    }
    for (size_t candidate = semantic_index; candidate < sema->block_count;
         candidate++) {
        if (!sema->blocks[candidate].has_nonlocal_return) {
            continue;
        }
        st_sema_block_id_t cursor = (st_sema_block_id_t)candidate;
        while (cursor != ST_SEMA_INVALID_ID && cursor < sema->block_count) {
            if (cursor == semantic_index) {
                return true;
            }
            cursor = sema->blocks[cursor].parent;
        }
    }
    return false;
}

static bool expression_is_inline_boolean_send(const st_ast_node_t *node)
{
    if (!node || node->kind != ST_AST_EXPRESSION
            || node->as.expression.messages.count != 1u
            || !node->as.expression.receiver
            || (node->as.expression.receiver->kind != ST_AST_TRUE
                && node->as.expression.receiver->kind != ST_AST_FALSE))
        return false;
    const st_ast_node_t *message = node->as.expression.messages.items[0];
    return message && message->kind == ST_AST_MESSAGE
        && (string_is(message->as.message.selector, "ifTrue:")
            || string_is(message->as.message.selector, "ifFalse:")
            || string_is(message->as.message.selector, "ifTrue:ifFalse:"));
}

/* True when evaluating node in the current activation can enter another
 * Smalltalk activation.  Escaping block bodies are separate activations;
 * exact-Boolean inline block bodies are deliberately part of the caller. */
static bool activation_may_call(const st_ast_node_t *node)
{
    if (!node) {
        return false;
    }
    switch (node->kind) {
    case ST_AST_BLOCK:
        for (size_t index = 0u; index < node->as.block.expressions.count;
             index++) {
            if (activation_may_call(node->as.block.expressions.items[index]))
                return true;
        }
        return false;
    case ST_AST_EXPRESSION: {
        const st_ast_expression_t *expression = &node->as.expression;
        if (expression->receiver->kind != ST_AST_BLOCK
                && activation_may_call(expression->receiver))
            return true;
        if (expression->messages.count == 0u) {
            return false;
        }
        if (!expression_is_inline_boolean_send(node)) {
            return true;
        }
        const st_ast_node_t *message = expression->messages.items[0];
        for (size_t index = 0u; index < message->as.message.arguments.count;
             index++) {
            if (activation_may_call(message->as.message.arguments.items[index]))
                return true;
        }
        return false;
    }
    /* A literal block encountered as a value is created now but its body is
     * not evaluated until the closure's own activation. */
    case ST_AST_MESSAGE:
        return true;
    default:
        return false;
    }
}

static bool prepare_block_artifact(
    lower_result_impl_t *implementation, st_lower_allocator_t allocator,
    const char *method_symbol, st_class_graph_method_id_t method_id,
    const st_sema_result_t *sema, const st_sema_block_t *info,
    size_t artifact_index, uint32_t lexical_ordinal, uint32_t root_capacity,
    const st_lower_root_map_t *root_maps, size_t root_map_count,
    bool needs_home, bool needs_control)
{
    char suffix[96];
    int suffix_length = snprintf(suffix, sizeof(suffix),
                                 "__m%u_block%u", method_id,
                                 lexical_ordinal);
    if (suffix_length <= 0 || (size_t)suffix_length >= sizeof(suffix))
        return false;
    size_t base = strlen(method_symbol);
    size_t code_length = base + (size_t)suffix_length;
    static const char descriptor_suffix[] = "__descriptor";
    static const char method_suffix[] = "__method_descriptor";
    if (code_length < base
            || code_length > SIZE_MAX - (sizeof(descriptor_suffix) - 1u)
            || code_length > SIZE_MAX - (sizeof(method_suffix) - 1u))
        return false;
    size_t descriptor_length = code_length + sizeof(descriptor_suffix) - 1u;
    size_t method_length = code_length + sizeof(method_suffix) - 1u;
    size_t storage_size = code_length + 1u;
    if (storage_size > SIZE_MAX - descriptor_length - 1u
            || storage_size + descriptor_length + 1u
                > SIZE_MAX - method_length - 1u)
        return false;
    storage_size += descriptor_length + 1u + method_length + 1u;
    char *symbol_storage = allocator.allocate(
        allocator.user, storage_size);
    if (!symbol_storage) {
        return false;
    }
    implementation->block_symbols[artifact_index] = symbol_storage;
    char *code = symbol_storage;
    char *descriptor = code + code_length + 1u;
    char *method = descriptor + descriptor_length + 1u;
    memcpy(code, method_symbol, base);
    memcpy(code + base, suffix, (size_t)suffix_length + 1u);
    memcpy(descriptor, code, code_length);
    memcpy(descriptor + code_length, descriptor_suffix,
           sizeof(descriptor_suffix));
    memcpy(method, code, code_length);
    memcpy(method + code_length, method_suffix, sizeof(method_suffix));

    if (info->capture_count != 0u) {
        if (info->capture_count
                > SIZE_MAX / sizeof(
                    *implementation->block_captures[artifact_index]))
            return false;
        implementation->block_captures[artifact_index] = allocator.allocate(
            allocator.user,
            info->capture_count
                * sizeof(*implementation->block_captures[artifact_index]));
        if (!implementation->block_captures[artifact_index]) {
            return false;
        }
        for (size_t index = 0u; index < info->capture_count; index++) {
            const st_sema_capture_t *capture = &sema->captures[
                info->capture_offset + index];
            implementation->block_captures[artifact_index][index] =
                (st_aot_capture_descriptor_t) {
                    capture->binding,
                    capture->mode == ST_SEMA_CAPTURE_SELF
                        ? ST_AOT_CAPTURE_SELF
                        : capture->mode == ST_SEMA_CAPTURE_CELL
                            ? ST_AOT_CAPTURE_CELL : ST_AOT_CAPTURE_VALUE
                };
        }
    }
    uint32_t arity = (uint32_t)info->node->as.block.arguments.count;
    bool has_cells = false;
    for (size_t index = 0u; index < info->capture_count; index++)
        has_cells = has_cells || sema->captures[
            info->capture_offset + index].mode == ST_SEMA_CAPTURE_CELL;
    implementation->blocks[artifact_index] = (st_lower_block_artifact_t) {
        .code_symbol = { code, code_length },
        .descriptor_symbol = { descriptor, descriptor_length },
        .method_descriptor_symbol = { method, method_length },
        .lexical_ordinal = lexical_ordinal,
        .arity = arity,
        .flags = (needs_home ? ST_AOT_BLOCK_HAS_HOME : 0u)
            | (has_cells ? ST_AOT_BLOCK_HAS_CELLS : 0u),
        .method_flags = (needs_control ? ST_METHOD_CAN_UNWIND : 0u)
            | (needs_home ? ST_METHOD_HAS_NON_LOCAL_RETURN : 0u),
        .required_root_capacity = root_capacity,
        .captures = implementation->block_captures[artifact_index],
        .capture_count = info->capture_count,
        .root_maps = root_maps,
        .root_map_count = root_map_count
    };
    return true;
}

static st_ast_string_t static_string(const char *text)
{
    st_ast_string_t result;
    result.data = text;
    result.length = strlen(text);
    return result;
}

static bool string_is(st_ast_string_t string, const char *text)
{
    size_t length = strlen(text);
    return string.length == length && string.data != NULL
        && memcmp(string.data, text, length) == 0;
}

static const st_lower_global_binding_t *find_global_binding(
    const st_lower_global_binding_t *bindings, size_t count,
    uint32_t semantic_external_id)
{
    size_t first = 0u;
    size_t end = count;
    while (first < end) {
        size_t middle = first + (end - first) / 2u;
        if (bindings[middle].semantic_external_id < semantic_external_id)
            first = middle + 1u;
        else
            end = middle;
    }
    return first < count
            && bindings[first].semantic_external_id == semantic_external_id
        ? &bindings[first] : NULL;
}

static bool global_bindings_are_valid(
    const st_lower_global_binding_t *bindings, size_t count)
{
    if ((bindings == NULL) != (count == 0u)) return false;
    for (size_t index = 0u; index < count; index++) {
        if (bindings[index].semantic_external_id == ST_SEMA_INVALID_ID
                || (index != 0u
                    && bindings[index - 1u].semantic_external_id
                        >= bindings[index].semantic_external_id))
            return false;
        for (size_t prior = 0u; prior < index; prior++)
            if (bindings[prior].runtime_index
                    == bindings[index].runtime_index)
                return false;
    }
    return true;
}

static bool primitive_matches_heap_spec(const st_primitive_t *primitive)
{
    size_t count = 0u;
    const st_primitive_spec_t *specs = st_heap_primitive_specs(&count);
    if (primitive == NULL || specs == NULL) return false;
    for (size_t index = 0u; index < count; index++) {
        const st_primitive_spec_t *spec = &specs[index];
        if (primitive->intrinsic_id == spec->intrinsic_id
                && primitive->implementation_kind
                    == spec->implementation_kind
                && primitive->method_arity == spec->method_arity
                && primitive->receiver_policy == spec->receiver_policy
                && primitive->failure_policy == spec->failure_policy
                && primitive->name.length == spec->name_length
                && primitive->name.data != NULL && spec->name != NULL
                && memcmp(primitive->name.data, spec->name,
                          spec->name_length) == 0
                && primitive->runtime_symbol.length
                    == spec->runtime_symbol_length
                && ((spec->runtime_symbol_length == 0u
                     && primitive->runtime_symbol.data == NULL)
                    || (spec->runtime_symbol_length != 0u
                        && primitive->runtime_symbol.data != NULL
                        && spec->runtime_symbol != NULL
                        && memcmp(primitive->runtime_symbol.data,
                                  spec->runtime_symbol,
                                  spec->runtime_symbol_length) == 0)))
            return true;
    }
    return false;
}

static bool portable_symbol_is_valid(const char *symbol)
{
    const unsigned char *cursor = (const unsigned char *)symbol;
    if (!cursor || !(cursor[0] == '_' ||
                     (cursor[0] >= 'A' && cursor[0] <= 'Z') ||
                     (cursor[0] >= 'a' && cursor[0] <= 'z')))
        return false;
    for (++cursor; *cursor != '\0'; ++cursor) {
        if (!(*cursor == '_' || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9')))
            return false;
    }
    return true;
}

static bool portable_symbol_span_is_valid(st_ast_string_t symbol)
{
    if (symbol.data == NULL || symbol.length == 0u)
        return false;
    if (!(symbol.data[0] == '_'
            || (symbol.data[0] >= 'A' && symbol.data[0] <= 'Z')
            || (symbol.data[0] >= 'a' && symbol.data[0] <= 'z')))
        return false;
    for (size_t index = 1u; index < symbol.length; index++) {
        unsigned char byte = (unsigned char)symbol.data[index];
        if (!(byte == '_' || (byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9')))
            return false;
    }
    return true;
}

static bool list_is_well_formed(const st_ast_list_t *list)
{
    return list != NULL && list->count <= list->capacity
        && (list->count == 0u || list->items != NULL);
}

static void set_diagnostic(st_lower_result_t *result,
                           st_lower_status_t status,
                           st_lower_diagnostic_code_t code,
                           const st_ast_node_t *node,
                           st_ast_string_t detail)
{
    if (result->status != ST_LOWER_OK) return;
    result->status = status;
    result->diagnostic.code = code;
    result->diagnostic.detail = detail;
    if (node != NULL) {
        result->diagnostic.span = node->span;
        result->diagnostic.node_kind = node->kind;
        result->diagnostic.has_span = true;
    }
}

static void unsupported(preflight_t *flight,
                        st_lower_diagnostic_code_t code,
                        const st_ast_node_t *node, st_ast_string_t detail)
{
    set_diagnostic(flight->result, ST_LOWER_ERR_UNSUPPORTED, code, node,
                   detail);
}

static const st_sema_binding_t *binding_for_site(
    preflight_t *flight, const st_ast_node_t *site,
    st_sema_access_t expected_access)
{
    const st_sema_reference_t *reference = st_sema_reference_for_node(
        flight->sema, site);
    if (reference == NULL || reference->binding >= flight->sema->binding_count
            || reference->access != expected_access) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, site,
                       static_string("semantic reference does not match AST"));
        return NULL;
    }
    return &flight->sema->bindings[reference->binding];
}

static bool parse_small_integer(const st_ast_node_t *node,
                                uint64_t *encoded_out)
{
    const st_ast_integer_t *integer = &node->as.integer;
    uint64_t magnitude = 0u;
    uint64_t limit = integer->negative
        ? (UINT64_C(1) << 60) : (UINT64_C(1) << 60) - UINT64_C(1);
    size_t index;
    if (integer->radix < 2u || integer->radix > 36u
            || integer->spelling.data == NULL
            || integer->spelling.length == 0u) return false;
    for (index = 0u; index < integer->spelling.length; index++) {
        unsigned char byte = (unsigned char)integer->spelling.data[index];
        unsigned digit;
        if (byte >= '0' && byte <= '9') digit = (unsigned)(byte - '0');
        else if (byte >= 'A' && byte <= 'Z')
            digit = 10u + (unsigned)(byte - 'A');
        else if (byte >= 'a' && byte <= 'z')
            digit = 10u + (unsigned)(byte - 'a');
        else return false;
        if (digit >= integer->radix
                || magnitude > (limit - digit) / integer->radix) return false;
        magnitude = magnitude * integer->radix + digit;
    }
    if (magnitude > limit) return false;
    uint64_t signed_bits = integer->negative
        ? UINT64_C(0) - magnitude : magnitude;
    *encoded_out = (signed_bits << ST_VALUE_TAG_BITS)
                 | ST_VALUE_TAG_SMALL_INTEGER;
    return true;
}

static bool preflight_value(preflight_t *flight, const st_ast_node_t *node);
static bool preflight_expression(preflight_t *flight,
                                 const st_ast_node_t *expression);
static anvil_value_t *create_send_site(lowerer_t *lowerer,
                                       st_selector_id_t selector_id,
                                       uint32_t lexical_owner_class_id);
static bool emit_control_return(lowerer_t *lowerer, anvil_value_t *value);
static bool check_control_pending(lowerer_t *lowerer,
                                  const st_ast_node_t *node);
static bool initialize_bindings(lowerer_t *lowerer);
static bool closure_status_or_abort(lowerer_t *lowerer,
                                    anvil_value_t *status,
                                    const st_ast_node_t *node,
                                    const char *stem);

static bool runtime_class_map_is_valid(
    const st_class_graph_result_t *graph, const st_lower_options_t *options)
{
    bool has_namespace = false;
    if ((options->runtime_class_ids_by_entity == NULL)
            != (options->runtime_class_id_count == 0u))
        return false;
    if (options->runtime_class_ids_by_entity == NULL) {
        for (size_t index = 0u; index < graph->entity_count; index++)
            has_namespace = has_namespace
                || graph->entities[index].kind == ST_CLASS_GRAPH_NAMESPACE;
        return !has_namespace;
    }
    if (options->runtime_class_id_count != graph->entity_count) return false;
    uint32_t expected = 1u;
    for (size_t index = 0u; index < graph->entity_count; index++) {
        uint32_t runtime_id = options->runtime_class_ids_by_entity[index];
        if (graph->entities[index].kind == ST_CLASS_GRAPH_NAMESPACE) {
            if (runtime_id != 0u) return false;
        } else if (runtime_id != expected++) {
            return false;
        }
    }
    return true;
}

static uint32_t runtime_class_id(lowerer_t *lowerer,
                                 st_class_graph_id_t graph_id)
{
    if (graph_id == ST_CLASS_GRAPH_INVALID_ID) return 0u;
    if (lowerer->runtime_class_ids_by_entity == NULL) return graph_id;
    if ((size_t)graph_id > lowerer->runtime_class_id_count) return 0u;
    return lowerer->runtime_class_ids_by_entity[graph_id - 1u];
}

static bool preflight_escaping_block(preflight_t *flight,
                                     const st_ast_node_t *block)
{
    const st_sema_block_t *info = st_sema_block_for_node(
        flight->sema, block);
    if (!info || !list_is_well_formed(&block->as.block.arguments)
            || !list_is_well_formed(&block->as.block.temporaries)
            || !list_is_well_formed(&block->as.block.expressions)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, block,
                       static_string("escaping block lacks semantic metadata"));
        return false;
    }
    if (block->as.block.arguments.count > 3u) {
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_NODE, block,
                    static_string("AOT block arity exceeds three"));
        return false;
    }
    if (info->capture_offset > flight->sema->capture_count
            || info->capture_count
                > flight->sema->capture_count - info->capture_offset) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, block,
                       static_string("block capture range is invalid"));
        return false;
    }
    for (size_t index = 0u; index < info->capture_count; index++) {
        const st_sema_capture_t *capture = &flight->sema->captures[
            info->capture_offset + index];
        if (capture->binding >= flight->sema->binding_count) {
            set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                           ST_LOWER_DIAG_INVALID_INPUT, block,
                           static_string("block capture binding is invalid"));
            return false;
        }
        const st_sema_binding_t *binding = &flight->sema->bindings[
            capture->binding];
        if (!((capture->mode == ST_SEMA_CAPTURE_SELF
                    && binding->kind == ST_SEMA_BIND_SELF)
                || ((capture->mode == ST_SEMA_CAPTURE_VALUE
                        || capture->mode == ST_SEMA_CAPTURE_CELL)
                    && (binding->kind == ST_SEMA_BIND_METHOD_ARGUMENT
                        || binding->kind == ST_SEMA_BIND_BLOCK_ARGUMENT
                        || binding->kind == ST_SEMA_BIND_TEMPORARY)))) {
            unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_BINDING, block,
                        binding->name);
            return false;
        }
    }
    size_t scratch = info->capture_count + 2u; /* creator + captures + result */
    if (scratch < info->capture_count
            || flight->scratch_depth > SIZE_MAX - scratch
            || flight->safepoint_count == UINT32_MAX) {
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, block,
                       static_string("closure root plan overflows"));
        return false;
    }
    size_t required = flight->scratch_depth + scratch;
    if (required > flight->maximum_scratch_roots)
        flight->maximum_scratch_roots = required;
    flight->safepoint_count++;
    flight->closure_create_count++;
    if (++flight->depth > ST_LOWER_MAX_NESTING) {
        flight->depth--;
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, block,
                       static_string("lowering nesting limit exceeded"));
        return false;
    }
    for (size_t index = 0u; index < block->as.block.expressions.count;
         index++) {
        if (!preflight_expression(
                flight, block->as.block.expressions.items[index])) {
            flight->depth--;
            return false;
        }
    }
    flight->depth--;
    return true;
}

static bool preflight_direct_closure_call(preflight_t *flight,
                                          const st_ast_node_t *message,
                                          size_t block_arity)
{
    if (!message || message->kind != ST_AST_MESSAGE
            || !list_is_well_formed(&message->as.message.arguments)
            || message->as.message.starts_cascade || message->as.message.super_send
                || !((block_arity == 0u
                    && string_is(message->as.message.selector, "value"))
                || (block_arity == 1u
                    && string_is(message->as.message.selector, "value:"))
                || (block_arity == 2u
                    && string_is(message->as.message.selector,
                                 "value:value:"))
                || (block_arity == 3u
                    && string_is(message->as.message.selector,
                                 "value:value:value:")))
            || message->as.message.arguments.count != block_arity) {
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_SEND, message,
                    static_string("direct closure invocation selector/arity mismatch"));
        return false;
    }
    size_t scratch = block_arity + 1u;
    if (flight->scratch_depth > SIZE_MAX - scratch
            || flight->safepoint_count == UINT32_MAX) {
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("closure invocation root plan overflows"));
        return false;
    }
    size_t saved = flight->scratch_depth;
    flight->scratch_depth += scratch;
    if (flight->scratch_depth > flight->maximum_scratch_roots)
        flight->maximum_scratch_roots = flight->scratch_depth;
    for (size_t index = 0u; index < block_arity; index++)
        if (!preflight_value(flight, message->as.message.arguments.items[index])) {
            flight->scratch_depth = saved;
            return false;
        }
    flight->scratch_depth = saved;
    flight->safepoint_count++;
    flight->closure_call_count++;
    return true;
}

static bool message_is_direct_closure_call(const st_ast_node_t *message,
                                           size_t block_arity)
{
    if (message == NULL || message->kind != ST_AST_MESSAGE
            || !list_is_well_formed(&message->as.message.arguments)
            || message->as.message.starts_cascade
            || message->as.message.super_send
            || message->as.message.arguments.count != block_arity) {
        return false;
    }

    switch (block_arity) {
    case 0u:
        return string_is(message->as.message.selector, "value");
    case 1u:
        return string_is(message->as.message.selector, "value:");
    case 2u:
        return string_is(message->as.message.selector, "value:value:");
    case 3u:
        return string_is(message->as.message.selector,
                         "value:value:value:");
    default:
        return false;
    }
}

static bool expression_uses_direct_closure_call(
    const st_ast_node_t *expression)
{
    if (expression == NULL || expression->kind != ST_AST_EXPRESSION
            || expression->as.expression.receiver == NULL
            || expression->as.expression.receiver->kind != ST_AST_BLOCK
            || expression->as.expression.messages.count != 1u) {
        return false;
    }

    return message_is_direct_closure_call(
        expression->as.expression.messages.items[0],
        expression->as.expression.receiver->as.block.arguments.count);
}

static bool preflight_block(preflight_t *flight, const st_ast_node_t *block)
{
    size_t index;
    if (block == NULL || block->kind != ST_AST_BLOCK
            || !list_is_well_formed(&block->as.block.arguments)
            || !list_is_well_formed(&block->as.block.temporaries)
            || !list_is_well_formed(&block->as.block.expressions)
            || block->as.block.arguments.count != 0u) {
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_NODE, block,
                    static_string("only zero-argument inline blocks are supported"));
        return false;
    }
    if (++flight->depth > ST_LOWER_MAX_NESTING) {
        flight->depth--;
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, block,
                       static_string("lowering nesting limit exceeded"));
        return false;
    }
    for (index = 0u; index < block->as.block.expressions.count; index++) {
        if (!preflight_value(flight, block->as.block.expressions.items[index])) {
            flight->depth--;
            return false;
        }
    }
    flight->depth--;
    return true;
}

static bool preflight_boolean_intrinsic(preflight_t *flight,
                                        const st_ast_node_t *expression,
                                        const st_ast_node_t *message,
                                        bool *recognized_out)
{
    *recognized_out = false;
    size_t required;
    if (message == NULL || message->kind != ST_AST_MESSAGE
            || !list_is_well_formed(&message->as.message.arguments)
            || message->as.message.selector.data == NULL
            || message->as.message.selector.length == 0u) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("malformed message AST"));
        return false;
    }
    if (expression->as.expression.receiver->kind != ST_AST_TRUE
            && expression->as.expression.receiver->kind != ST_AST_FALSE) {
        return true;
    }
    if (string_is(message->as.message.selector, "ifTrue:")
            || string_is(message->as.message.selector, "ifFalse:")) {
        required = 1u;
    } else if (string_is(message->as.message.selector,
                         "ifTrue:ifFalse:")) {
        required = 2u;
    } else {
        return true;
    }
    *recognized_out = true;
    if (message->as.message.arguments.count != required
            || message->as.message.starts_cascade
            || message->as.message.super_send) {
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_SEND, message,
                    message->as.message.selector);
        return false;
    }
    for (size_t index = 0u; index < required; index++) {
        if (!preflight_block(flight, message->as.message.arguments.items[index]))
            return false;
    }
    return true;
}

static bool preflight_general_message(preflight_t *flight,
                                      const st_ast_node_t *message)
{
    st_selector_id_t selector_id;
    const st_selector_t *selector;
    if (message == NULL || message->kind != ST_AST_MESSAGE
            || !list_is_well_formed(&message->as.message.arguments)
            || message->as.message.selector.data == NULL
            || message->as.message.selector.length == 0u) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("malformed message AST"));
        return false;
    }
    if (flight->selectors == NULL
            || !st_selector_table_is_frozen(flight->selectors)
            || !st_selector_lookup(flight->selectors,
                                   message->as.message.selector.data,
                                   message->as.message.selector.length,
                                   &selector_id)
            || (selector = st_selector_get(flight->selectors, selector_id))
                == NULL
            || selector->arity != message->as.message.arguments.count) {
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_SEND, message,
                    message->as.message.selector);
        return false;
    }
    if (message->as.message.super_send
            && (flight->graph_method == NULL
                || flight->graph_method->lexical_super
                    == ST_CLASS_GRAPH_INVALID_ID)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("super send has no lexical superclass"));
        return false;
    }
    size_t scratch = message->as.message.arguments.count + 1u;
    if (scratch < message->as.message.arguments.count
            || flight->scratch_depth > SIZE_MAX - scratch) {
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("send root requirement overflows size_t"));
        return false;
    }
    size_t saved_scratch_depth = flight->scratch_depth;
    flight->scratch_depth += scratch;
    if (flight->scratch_depth > flight->maximum_scratch_roots)
        flight->maximum_scratch_roots = flight->scratch_depth;
    for (size_t index = 0u; index < message->as.message.arguments.count;
         index++) {
        if (!preflight_value(flight,
                message->as.message.arguments.items[index])) {
            flight->scratch_depth = saved_scratch_depth;
            return false;
        }
    }
    flight->scratch_depth = saved_scratch_depth;
    if (flight->send_count == UINT32_MAX
            || flight->safepoint_count == UINT32_MAX) {
        set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("send-site ID space exhausted"));
        return false;
    }
    flight->send_count++;
    flight->safepoint_count++;
    if (message->as.message.arguments.count > flight->maximum_send_arity)
        flight->maximum_send_arity = message->as.message.arguments.count;
    return true;
}

static bool preflight_expression(preflight_t *flight,
                                 const st_ast_node_t *expression)
{
    size_t index;
    bool has_cascade = false;
    size_t saved_scratch_depth;
    if (expression == NULL || expression->kind != ST_AST_EXPRESSION
            || expression->as.expression.receiver == NULL
            || !list_is_well_formed(&expression->as.expression.assignments)
            || !list_is_well_formed(&expression->as.expression.messages)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, expression,
                       static_string("malformed expression AST"));
        return false;
    }
    if (!preflight_value(flight, expression->as.expression.receiver))
        return false;
    for (index = 0u; index < expression->as.expression.messages.count;
         index++) {
        const st_ast_node_t *message =
            expression->as.expression.messages.items[index];
        if (message != NULL && message->kind == ST_AST_MESSAGE
                && message->as.message.starts_cascade)
            has_cascade = true;
    }
    if (has_cascade
            && (expression->as.expression.messages.count == 0u
                || expression->as.expression.messages.items[0] == NULL
                || expression->as.expression.messages.items[0]->kind
                    != ST_AST_MESSAGE
                || expression->as.expression.messages.items[0]
                       ->as.message.starts_cascade)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, expression,
                       static_string("cascade boundary precedes its initial message"));
        return false;
    }
    if (has_cascade) flight->has_cascade = true;
    saved_scratch_depth = flight->scratch_depth;
    if (has_cascade) {
        if (flight->scratch_depth == SIZE_MAX) {
            set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, expression,
                           static_string("cascade root requirement overflows size_t"));
            return false;
        }
        flight->scratch_depth++;
        if (flight->scratch_depth > flight->maximum_scratch_roots)
            flight->maximum_scratch_roots = flight->scratch_depth;
    }
    if (expression->as.expression.messages.count != 0u) {
        bool intrinsic = false;
        if (expression_uses_direct_closure_call(expression)) {
            if (!preflight_direct_closure_call(
                        flight, expression->as.expression.messages.items[0],
                        expression->as.expression.receiver->as.block.arguments.count))
                goto fail;
            intrinsic = true;
        } else if (expression->as.expression.messages.count == 1u
                && !preflight_boolean_intrinsic(
                    flight, expression,
                    expression->as.expression.messages.items[0],
                    &intrinsic)) goto fail;
        if (!intrinsic) {
            for (index = 0u;
                 index < expression->as.expression.messages.count; index++) {
                if (!preflight_general_message(
                        flight, expression->as.expression.messages.items[index]))
                    goto fail;
            }
        }
    }
    flight->scratch_depth = saved_scratch_depth;
    for (index = 0u; index < expression->as.expression.assignments.count;
         index++) {
        const st_ast_node_t *target =
            expression->as.expression.assignments.items[index];
        const st_sema_binding_t *binding = binding_for_site(
            flight, target, ST_SEMA_ACCESS_WRITE);
        if (binding == NULL) return false;
        if (binding->kind == ST_SEMA_BIND_INSTANCE_VARIABLE) {
            if ((binding->flags & ST_SEMA_BINDING_EXTERNAL) == 0u
                    || binding->slot == ST_SEMA_INVALID_ID
                    || flight->heap_access_count == SIZE_MAX) {
                set_diagnostic(
                    flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                    ST_LOWER_DIAG_INVALID_INPUT, target,
                    static_string("invalid semantic instance-variable slot"));
                return false;
            }
            flight->heap_access_count++;
        } else if (binding->kind != ST_SEMA_BIND_TEMPORARY) {
            unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_BINDING, target,
                        binding->name);
            return false;
        }
    }
    return true;
fail:
    flight->scratch_depth = saved_scratch_depth;
    return false;
}

static bool preflight_variable(preflight_t *flight,
                               const st_ast_node_t *node)
{
    const st_sema_reference_t *reference = st_sema_reference_for_node(
        flight->sema, node);
    if (reference == NULL || reference->binding >= flight->sema->binding_count
            || (reference->access != ST_SEMA_ACCESS_READ
                && reference->access != ST_SEMA_ACCESS_SUPER_RECEIVER)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, node,
                       static_string("semantic reference does not match AST"));
        return false;
    }
    const st_sema_binding_t *binding =
        &flight->sema->bindings[reference->binding];
    switch (binding->kind) {
    case ST_SEMA_BIND_SELF:
    case ST_SEMA_BIND_SUPER:
    case ST_SEMA_BIND_METHOD_ARGUMENT:
    case ST_SEMA_BIND_BLOCK_ARGUMENT:
    case ST_SEMA_BIND_TEMPORARY:
        return true;
    case ST_SEMA_BIND_INSTANCE_VARIABLE:
        if ((binding->flags & ST_SEMA_BINDING_EXTERNAL) == 0u
                || binding->slot == ST_SEMA_INVALID_ID
                || flight->heap_access_count == SIZE_MAX) {
            set_diagnostic(
                flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                ST_LOWER_DIAG_INVALID_INPUT, node,
                static_string("invalid semantic instance-variable slot"));
            return false;
        }
        flight->heap_access_count++;
        return true;
    case ST_SEMA_BIND_GLOBAL:
    case ST_SEMA_BIND_FORWARD_GLOBAL:
        if ((binding->flags & ST_SEMA_BINDING_EXTERNAL) == 0u
                || binding->external_id == ST_SEMA_INVALID_ID
                || find_global_binding(
                    flight->globals, flight->global_count,
                    binding->external_id) == NULL) {
            unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_BINDING, node,
                        binding->name);
            return false;
        }
        flight->uses_image_runtime = true;
        if (flight->image_load_count == SIZE_MAX) {
            set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, node,
                           static_string("image load ordinal overflows size_t"));
            return false;
        }
        flight->image_load_count++;
        return true;
    default:
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_BINDING, node,
                    binding->name);
        return false;
    }
}

static bool preflight_value(preflight_t *flight, const st_ast_node_t *node)
{
    uint64_t ignored;
    if (node == NULL) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, NULL,
                       static_string("NULL AST value"));
        return false;
    }
    switch (node->kind) {
    case ST_AST_EXPRESSION:
        return preflight_expression(flight, node);
    case ST_AST_VARIABLE:
        return preflight_variable(flight, node);
    case ST_AST_NIL:
    case ST_AST_TRUE:
    case ST_AST_FALSE:
        return true;
    case ST_AST_INTEGER:
        if (parse_small_integer(node, &ignored)) return true;
        unsupported(flight, ST_LOWER_DIAG_LITERAL_OUT_OF_RANGE, node,
                    node->as.integer.spelling);
        return false;
    case ST_AST_CHARACTER:
        if (node->as.character <= UINT32_C(0x10ffff)
                && !(node->as.character >= UINT32_C(0xd800)
                     && node->as.character <= UINT32_C(0xdfff))) return true;
        unsupported(flight, ST_LOWER_DIAG_LITERAL_OUT_OF_RANGE, node,
                    static_string("invalid Unicode scalar"));
        return false;
    case ST_AST_STRING:
        if (node->as.text.data == NULL && node->as.text.length != 0u) {
            set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                           ST_LOWER_DIAG_INVALID_INPUT, node,
                           static_string("malformed String literal"));
            return false;
        }
        if (flight->string_literal_count == UINT32_MAX
                || node->as.text.length
                    > SIZE_MAX - flight->string_literal_bytes) {
            set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, node,
                           static_string("String literal artifact plan overflows"));
            return false;
        }
        flight->string_literal_count++;
        flight->string_literal_bytes += node->as.text.length;
        flight->uses_image_runtime = true;
        if (flight->image_load_count == SIZE_MAX) {
            set_diagnostic(flight->result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, node,
                           static_string("image load ordinal overflows size_t"));
            return false;
        }
        flight->image_load_count++;
        return true;
    case ST_AST_BLOCK:
        return preflight_escaping_block(flight, node);
    default:
        unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_NODE, node,
                    static_string(st_ast_kind_name(node->kind)));
        return false;
    }
}

static bool preflight_method(preflight_t *flight)
{
    const st_ast_node_t *body;
    if (flight->method == NULL || flight->method->kind != ST_AST_METHOD) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, flight->method,
                       static_string("graph method has malformed AST"));
        return false;
    }
    body = flight->method->as.method.body;
    if (body == NULL || body->kind != ST_AST_BLOCK
            || !list_is_well_formed(&flight->method->as.method.arguments)
            || !list_is_well_formed(&flight->method->as.method.pragmas)
            || !list_is_well_formed(&body->as.block.arguments)
            || !list_is_well_formed(&body->as.block.temporaries)
            || !list_is_well_formed(&body->as.block.expressions)) {
        set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, flight->method,
                       static_string("graph method has malformed AST"));
        return false;
    }
    if (flight->method->as.method.pragmas.count != 0u
            || flight->primitive_binding != NULL) {
        const st_primitive_binding_t *binding = flight->primitive_binding;
        const st_primitive_t *primitive = binding ? binding->primitive : NULL;
        bool supported_id = false;
        bool heap_primitive = false;
        bool runtime_symbol_primitive = false;
        if (primitive != NULL) {
            switch (primitive->intrinsic_id) {
            case ST_INTRINSIC_IDENTITY:
            case ST_INTRINSIC_INT_EQUALS:
            case ST_INTRINSIC_INT_NOT_EQUALS:
            case ST_INTRINSIC_INT_LESS_THAN:
            case ST_INTRINSIC_INT_LESS_EQUALS:
            case ST_INTRINSIC_INT_GREATER_THAN:
            case ST_INTRINSIC_INT_GREATER_EQUALS:
            case ST_INTRINSIC_INT_ADD:
            case ST_INTRINSIC_INT_SUBTRACT:
            case ST_INTRINSIC_INT_MULTIPLY:
            case ST_INTRINSIC_INT_FLOOR_DIVIDE:
            case ST_INTRINSIC_INT_MODULO:
            case ST_INTRINSIC_INT_NEGATE:
            case ST_INTRINSIC_INT_BIT_AND:
            case ST_INTRINSIC_INT_BIT_OR:
            case ST_INTRINSIC_INT_BIT_XOR:
            case ST_INTRINSIC_INT_SHIFT:
            case ST_INTRINSIC_CHARACTER_NEW:
            case ST_INTRINSIC_CHARACTER_CODE:
                supported_id = true;
                break;
            default:
                break;
            }
            if (!supported_id && primitive_matches_heap_spec(primitive)) {
                supported_id = true;
                heap_primitive = true;
            }
            if (primitive->implementation_kind != ST_PRIMITIVE_INTRINSIC
                    && primitive->intrinsic_id
                       == ST_PRIMITIVE_INVALID_INTRINSIC_ID
                    && portable_symbol_span_is_valid(
                        primitive->runtime_symbol))
                runtime_symbol_primitive = true;
        }
        if (binding == NULL && flight->method->as.method.pragmas.count != 0u) {
            unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_PRIMITIVE,
                        flight->method->as.method.pragmas.items[0],
                        static_string("primitive pragma has no resolved binding"));
            return false;
        }
        if (binding == NULL || primitive == NULL
                || binding->method != flight->method
                || flight->method->as.method.pragmas.count != 1u
                || binding->pragma
                    != flight->method->as.method.pragmas.items[0]
                || binding->pragma == NULL
                || binding->pragma->kind != ST_AST_MESSAGE
                || !string_is(binding->pragma->as.message.selector,
                              "primitive:")
                || !list_is_well_formed(
                    &binding->pragma->as.message.arguments)
                || binding->pragma->as.message.arguments.count != 1u
                || binding->pragma->as.message.arguments.items[0] == NULL
                || binding->pragma->as.message.arguments.items[0]->kind
                    != ST_AST_VARIABLE
                || binding->pragma->as.message.arguments.items[0]
                       ->as.variable.name.data == NULL
                || primitive->name.data == NULL
                || primitive->name.length == 0u
                || binding->pragma->as.message.arguments.items[0]
                       ->as.variable.name.length != primitive->name.length
                || memcmp(binding->pragma->as.message.arguments.items[0]
                              ->as.variable.name.data,
                          primitive->name.data, primitive->name.length) != 0
                || primitive->method_arity
                    != flight->method->as.method.arguments.count
                || primitive->failure_policy > ST_PRIMITIVE_FALL_THROUGH
                || primitive->receiver_policy
                    > ST_PRIMITIVE_INSTANCE_OR_CLASS
                || (primitive->receiver_policy == ST_PRIMITIVE_INSTANCE_ONLY
                    && flight->method->as.method.class_side)
                || (primitive->receiver_policy == ST_PRIMITIVE_CLASS_ONLY
                    && !flight->method->as.method.class_side)) {
            set_diagnostic(flight->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                           ST_LOWER_DIAG_INVALID_INPUT, flight->method,
                           static_string("primitive binding does not match method"));
            return false;
        }
        if (!((primitive->implementation_kind == ST_PRIMITIVE_INTRINSIC
                    && primitive->intrinsic_id != 0u && supported_id)
                || runtime_symbol_primitive)) {
            unsupported(flight, ST_LOWER_DIAG_UNSUPPORTED_PRIMITIVE,
                        binding->pragma, primitive->name);
            return false;
        }
        if (heap_primitive) {
            flight->heap_primitive = true;
            flight->safepoint_count = 1u;
        }
        if (runtime_symbol_primitive) {
            flight->runtime_symbol_primitive = true;
            flight->runtime_control_primitive =
                primitive->implementation_kind ==
                    ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL;
            flight->safepoint_count = 1u;
        }
        if (primitive->failure_policy == ST_PRIMITIVE_CANNOT_FAIL)
            return true;
    }
    for (size_t index = 0u; index < body->as.block.expressions.count; index++) {
        if (!preflight_expression(flight,
                body->as.block.expressions.items[index])) return false;
    }
    return true;
}

static void fail_ir(lowerer_t *lowerer, const st_ast_node_t *node)
{
    st_lower_status_t status = anvil_ctx_get_last_error(lowerer->ctx)
        == ANVIL_ERR_NOMEM ? ST_LOWER_ERR_OUT_OF_MEMORY
                           : ST_LOWER_ERR_IR_BUILD;
    set_diagnostic(lowerer->result, status, ST_LOWER_DIAG_IR_BUILD, node,
                   static_string("Anvil IR builder rejected the method"));
}

static anvil_value_t *frame_field(lowerer_t *lowerer, unsigned field,
                                  anvil_type_t *type, const char *name)
{
    anvil_value_t *address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->frame_type, lowerer->frame, field,
        "frame.field.addr");
    return address ? anvil_build_load(lowerer->ctx, type, address, name) : NULL;
}

static bool store_frame_field(lowerer_t *lowerer, anvil_value_t *frame,
                              unsigned field, anvil_value_t *value)
{
    anvil_value_t *address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->frame_type, frame, field, "frame.store.addr");
    return address != NULL
        && anvil_build_store(lowerer->ctx, value, address);
}

static anvil_value_t *root_address(lowerer_t *lowerer, uint32_t slot,
                                   const char *name)
{
    anvil_value_t *index = anvil_const_u32(lowerer->ctx, slot);
    anvil_value_t *indices[] = { index };
    return index ? anvil_build_gep(lowerer->ctx, lowerer->u64, lowerer->roots,
                                   indices, 1u, name) : NULL;
}

static bool root_store(lowerer_t *lowerer, uint32_t slot,
                       anvil_value_t *value)
{
    anvil_value_t *address = root_address(lowerer, slot, "root.addr");
    return address != NULL
        && anvil_build_store(lowerer->ctx, value, address);
}

static bool clear_scratch_roots(lowerer_t *lowerer, uint32_t first,
                                uint32_t count)
{
    anvil_value_t *nil_value = anvil_const_u64(
        lowerer->ctx, ST_VALUE_TAG_SPECIAL);
    if (nil_value == NULL) return false;
    for (uint32_t index = 0u; index < count; index++) {
        if (!root_store(lowerer,
                        lowerer->scratch_root_offset + first + index,
                        nil_value))
            return false;
    }
    return true;
}

static lowered_value_t lower_value(lowerer_t *lowerer,
                                   const st_ast_node_t *node);

static lowered_value_t terminated_value(void)
{
    lowered_value_t result = { NULL, true };
    return result;
}

static lowered_value_t failed_value(void)
{
    lowered_value_t result = { NULL, false };
    return result;
}

static lowered_value_t normal_value(anvil_value_t *value)
{
    lowered_value_t result = { value, false };
    return result;
}

static anvil_block_t *control_block_create(lowerer_t *lowerer,
                                           const char *stem)
{
    char name[80];
    int length = snprintf(name, sizeof(name), "%s.%zu", stem,
                          lowerer->next_control_block++);
    return length > 0 && (size_t)length < sizeof(name)
        ? anvil_block_create(lowerer->function, name) : NULL;
}

static bool control_status_or_abort(lowerer_t *lowerer,
                                    anvil_value_t *status,
                                    const char *stem)
{
    anvil_value_t *ok = status ? anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_CONTROL_OK), stem) : NULL;
    anvil_block_t *valid = control_block_create(lowerer, "control.valid");
    anvil_block_t *fatal = control_block_create(lowerer, "control.fatal");
    if (!ok || !valid || !fatal
            || !anvil_build_br_cond(lowerer->ctx, ok, valid, fatal)
            || !anvil_set_insert_point(lowerer->ctx, fatal))
        return false;
    anvil_value_t *arguments[] = { status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->control_fatal_function),
            arguments, 2u, "control.contract.abort", &fatal_value)
            || !fatal_value || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, valid))
        return false;
    return true;
}

static bool emit_control_return(lowerer_t *lowerer, anvil_value_t *value)
{
    return lowerer->control_scope && value != NULL
        && root_store(lowerer, lowerer->control_return_root, value)
        && anvil_build_store(lowerer->ctx, value,
                             lowerer->control_return_address)
        && anvil_build_br(lowerer->ctx, lowerer->control_epilogue);
}

static bool check_control_pending(lowerer_t *lowerer,
                                  const st_ast_node_t *node)
{
    anvil_value_t *pending_address;
    anvil_value_t *pending_value_address;
    anvil_value_t *status = NULL;
    anvil_value_t *pending;
    anvil_value_t *is_pending;
    anvil_block_t *propagate;
    anvil_block_t *continuation;
    if (!lowerer->control_scope) return true;
    pending_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u32, "control.pending.addr");
    pending_value_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "control.pending.value.addr");
    anvil_value_t *arguments[] = {
        lowerer->frame, pending_address, pending_value_address
    };
    if (!pending_address || !pending_value_address
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->control_pending_function),
                arguments, 3u, "control.pending.status", &status)
            || !control_status_or_abort(lowerer, status,
                                        "control.pending.valid")) {
        fail_ir(lowerer, node);
        return false;
    }
    pending = anvil_build_load(lowerer->ctx, lowerer->u32,
                               pending_address, "control.pending");
    is_pending = pending ? anvil_build_cmp_ne(
        lowerer->ctx, pending, anvil_const_u32(lowerer->ctx, 0u),
        "control.is.pending") : NULL;
    propagate = control_block_create(lowerer, "control.propagate");
    continuation = control_block_create(lowerer, "control.continue");
    if (!is_pending || !propagate || !continuation
            || !anvil_build_br_cond(lowerer->ctx, is_pending,
                                    propagate, continuation)
            || !anvil_set_insert_point(lowerer->ctx, propagate)) {
        fail_ir(lowerer, node);
        return false;
    }
    anvil_value_t *pending_value = anvil_build_load(
        lowerer->ctx, lowerer->u64, pending_value_address,
        "control.pending.value");
    if (!pending_value || !emit_control_return(lowerer, pending_value)
            || !anvil_set_insert_point(lowerer->ctx, continuation)) {
        fail_ir(lowerer, node);
        return false;
    }
    return true;
}

static lowered_value_t lower_expression(lowerer_t *lowerer,
                                        const st_ast_node_t *expression);

static lowered_value_t lower_inline_block(lowerer_t *lowerer,
                                          const st_ast_node_t *block)
{
    lowered_value_t result = normal_value(anvil_const_u64(
        lowerer->ctx, ST_VALUE_TAG_SPECIAL));
    if (result.value == NULL) return failed_value();
    for (size_t index = 0u; index < block->as.block.expressions.count; index++) {
        result = lower_expression(lowerer,
            block->as.block.expressions.items[index]);
        if (result.value == NULL || result.terminated) return result;
    }
    return result;
}

static lowered_value_t lower_boolean_send(lowerer_t *lowerer,
                                          const st_ast_node_t *expression,
                                          anvil_value_t *receiver)
{
    const st_ast_node_t *message = expression->as.expression.messages.items[0];
    const st_ast_node_t *then_ast = NULL;
    const st_ast_node_t *else_ast = NULL;
    if (string_is(message->as.message.selector, "ifTrue:")) {
        then_ast = message->as.message.arguments.items[0];
    } else if (string_is(message->as.message.selector, "ifFalse:")) {
        else_ast = message->as.message.arguments.items[0];
    } else {
        then_ast = message->as.message.arguments.items[0];
        else_ast = message->as.message.arguments.items[1];
    }

    anvil_value_t *true_value = anvil_const_u64(
        lowerer->ctx, (UINT64_C(2) << ST_VALUE_TAG_BITS)
                    | ST_VALUE_TAG_SPECIAL);
    anvil_value_t *condition = true_value
        ? anvil_build_cmp_eq(lowerer->ctx, receiver, true_value,
                             "boolean.is.true") : NULL;
    anvil_block_t *then_block = anvil_block_create(lowerer->function,
                                                    "boolean.true");
    anvil_block_t *else_block = anvil_block_create(lowerer->function,
                                                    "boolean.false");
    if (!condition || !then_block || !else_block
            || !anvil_build_br_cond(lowerer->ctx, condition,
                                    then_block, else_block)) {
        fail_ir(lowerer, message);
        return failed_value();
    }

    if (!anvil_set_insert_point(lowerer->ctx, then_block)) {
        fail_ir(lowerer, message);
        return failed_value();
    }
    lowered_value_t then_value = then_ast
        ? lower_inline_block(lowerer, then_ast)
        : normal_value(anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL));
    anvil_block_t *then_end = anvil_get_insert_block(lowerer->ctx);
    if ((!then_value.terminated && then_value.value == NULL)
            || lowerer->result->status != ST_LOWER_OK) {
        fail_ir(lowerer, message);
        return failed_value();
    }

    if (!anvil_set_insert_point(lowerer->ctx, else_block)) {
        fail_ir(lowerer, message);
        return failed_value();
    }
    lowered_value_t else_value = else_ast
        ? lower_inline_block(lowerer, else_ast)
        : normal_value(anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL));
    anvil_block_t *else_end = anvil_get_insert_block(lowerer->ctx);
    if ((!else_value.terminated && else_value.value == NULL)
            || lowerer->result->status != ST_LOWER_OK) {
        fail_ir(lowerer, message);
        return failed_value();
    }

    if (then_value.terminated && else_value.terminated)
        return terminated_value();

    anvil_block_t *merge = anvil_block_create(lowerer->function,
                                               "boolean.merge");
    if (merge == NULL
            || (!then_value.terminated
                && (!anvil_set_insert_point(lowerer->ctx, then_end)
                    || !anvil_build_br(lowerer->ctx, merge)))
            || (!else_value.terminated
                && (!anvil_set_insert_point(lowerer->ctx, else_end)
                    || !anvil_build_br(lowerer->ctx, merge)))
            || !anvil_set_insert_point(lowerer->ctx, merge)) {
        fail_ir(lowerer, message);
        return failed_value();
    }
    if (then_value.terminated) return else_value;
    if (else_value.terminated) return then_value;
    anvil_value_t *phi = anvil_build_phi(lowerer->ctx, lowerer->u64,
                                          "boolean.result");
    if (!phi || !anvil_phi_add_incoming(phi, then_value.value, then_end)
             || !anvil_phi_add_incoming(phi, else_value.value, else_end)) {
        fail_ir(lowerer, message);
        return failed_value();
    }
    return normal_value(phi);
}

static bool expression_uses_boolean_intrinsic(
    const st_ast_node_t *expression)
{
    if (expression->as.expression.messages.count != 1u
            || (expression->as.expression.receiver->kind != ST_AST_TRUE
                && expression->as.expression.receiver->kind != ST_AST_FALSE))
        return false;
    const st_ast_node_t *message =
        expression->as.expression.messages.items[0];
    return message != NULL && message->kind == ST_AST_MESSAGE
        && (string_is(message->as.message.selector, "ifTrue:")
            || string_is(message->as.message.selector, "ifFalse:")
            || string_is(message->as.message.selector,
                         "ifTrue:ifFalse:"));
}

static anvil_value_t *load_root(lowerer_t *lowerer, uint32_t slot,
                                const char *name)
{
    anvil_value_t *address = root_address(lowerer, slot, "root.reload.addr");
    return address ? anvil_build_load(lowerer->ctx, lowerer->u64,
                                      address, name) : NULL;
}

static anvil_block_t *send_block_create(lowerer_t *lowerer,
                                        size_t send_ordinal,
                                        const char *stem)
{
    char name[96];
    int length = snprintf(name, sizeof(name), "send.%zu.%s",
                          send_ordinal, stem);
    return length > 0 && (size_t)length < sizeof(name)
        ? anvil_block_create(lowerer->function, name) : NULL;
}

static lowered_value_t lower_general_send(
    lowerer_t *lowerer, const st_ast_node_t *message,
    anvil_value_t *receiver)
{
    size_t arity = message->as.message.arguments.count;
    size_t send_ordinal;
    st_selector_id_t selector_id = 0u;
    if (!st_selector_lookup(lowerer->selectors,
                            message->as.message.selector.data,
                            message->as.message.selector.length,
                            &selector_id)
            || arity > UINT32_MAX
            || lowerer->scratch_depth > UINT32_MAX - (uint32_t)arity - 1u) {
        set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, message,
                       static_string("selector/root plan changed after preflight"));
        return failed_value();
    }
    uint32_t saved_depth = lowerer->scratch_depth;
    uint32_t send_base = saved_depth;
    lowerer->scratch_depth += (uint32_t)arity + 1u;
    if (!root_store(lowerer, lowerer->scratch_root_offset + send_base,
                    receiver)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    for (size_t index = 0u; index < arity; index++) {
        lowered_value_t argument = lower_value(
            lowerer, message->as.message.arguments.items[index]);
        if (argument.terminated) {
            lowerer->scratch_depth = saved_depth;
            return argument;
        }
        if (argument.value == NULL
                || !root_store(
                    lowerer,
                    lowerer->scratch_root_offset + send_base
                        + (uint32_t)index + 1u,
                    argument.value)) {
            if (lowerer->result->status == ST_LOWER_OK)
                fail_ir(lowerer, message->as.message.arguments.items[index]);
            lowerer->scratch_depth = saved_depth;
            return failed_value();
        }
    }

    /* Evaluating an argument may allocate and move the receiver.  The root
     * slot is authoritative across those safepoints; never keep using the
     * pre-argument SSA value. */
    receiver = load_root(
        lowerer, lowerer->scratch_root_offset + send_base,
        "send.receiver");
    if (receiver == NULL) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }

    anvil_value_t *argv = anvil_const_null(
        lowerer->ctx, lowerer->value_ptr);
    if (arity != 0u) {
        anvil_type_t *argv_array_type = anvil_type_array(
            lowerer->ctx, lowerer->u64, arity);
        anvil_value_t *argv_array = argv_array_type
            ? anvil_build_alloca(lowerer->ctx, argv_array_type, "send.argv")
            : NULL;
        if (!argv_array) {
            fail_ir(lowerer, message);
            lowerer->scratch_depth = saved_depth;
            return failed_value();
        }
        for (size_t index = 0u; index < arity; index++) {
            anvil_value_t *zero = anvil_const_u32(lowerer->ctx, 0u);
            anvil_value_t *element = anvil_const_u32(
                lowerer->ctx, (uint32_t)index);
            anvil_value_t *indices[] = { zero, element };
            anvil_value_t *address = zero && element
                ? anvil_build_gep(lowerer->ctx, argv_array_type, argv_array,
                                  indices, 2u, "send.argv.element") : NULL;
            anvil_value_t *argument = load_root(
                lowerer,
                lowerer->scratch_root_offset + send_base
                    + (uint32_t)index + 1u,
                "send.argument");
            if (!address || !argument
                    || !anvil_build_store(lowerer->ctx, argument, address)) {
                fail_ir(lowerer, message);
                lowerer->scratch_depth = saved_depth;
                return failed_value();
            }
            if (index == 0u) argv = address;
        }
    }

    uint32_t lexical_owner = message->as.message.super_send
        ? runtime_class_id(lowerer, lowerer->graph_method->owner) : 0u;
    /* Arguments are lowered before the outer send site is created and may
     * contain sends of their own.  Reserve the ordinal only now so the site
     * global and every local block created for this send share one unique,
     * post-order ordinal. */
    send_ordinal = lowerer->next_send_site;
    anvil_value_t *site_global = create_send_site(
        lowerer, selector_id, lexical_owner);
    anvil_value_t *site = site_global
        ? anvil_const_symbol_addr(site_global) : NULL;
    anvil_value_t *target = anvil_build_alloca(
        lowerer->ctx, lowerer->send_target_type, "send.target");
    uint32_t safepoint_id = ++lowerer->next_safepoint_id;
    if (!site || !target
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, safepoint_id))) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *resolve_arguments[] = {
        lowerer->frame, site, receiver,
        anvil_const_u32(lowerer->ctx, (uint32_t)arity), target
    };
    anvil_value_t *status = NULL;
    if (!resolve_arguments[3]
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->send_resolve_function),
                resolve_arguments, 5u, "send.status", &status)
            || !status) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *resolved = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_AOT_SEND_OK), "send.resolved");
    anvil_block_t *hit = send_block_create(
        lowerer, send_ordinal, "hit");
    anvil_block_t *miss = send_block_create(
        lowerer, send_ordinal, "miss");
    if (!resolved || !hit || !miss
            || !anvil_build_br_cond(lowerer->ctx, resolved, hit, miss)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }

    if (!anvil_set_insert_point(lowerer->ctx, hit)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *code_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->send_target_type, target, 0u,
        "send.code.addr");
    anvil_value_t *descriptor_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->send_target_type, target, 1u,
        "send.descriptor.addr");
    anvil_value_t *capacity_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->send_target_type, target, 2u,
        "send.root.capacity.addr");
    anvil_value_t *code = code_address
        ? anvil_build_load(lowerer->ctx, lowerer->method_ptr, code_address,
                           "send.code") : NULL;
    anvil_value_t *descriptor = descriptor_address
        ? anvil_build_load(lowerer->ctx, lowerer->byte_ptr,
                           descriptor_address, "send.descriptor") : NULL;
    anvil_value_t *capacity = capacity_address
        ? anvil_build_load(lowerer->ctx, lowerer->u32, capacity_address,
                           "send.root.capacity") : NULL;
    anvil_value_t *zero32 = anvil_const_u32(lowerer->ctx, 0u);
    anvil_value_t *capacity_is_zero = capacity && zero32
        ? anvil_build_cmp_eq(lowerer->ctx, capacity, zero32,
                             "send.roots.empty") : NULL;
    anvil_value_t *null_roots = anvil_const_null(
        lowerer->ctx, lowerer->value_ptr);
    anvil_block_t *roots_empty = send_block_create(
        lowerer, send_ordinal, "roots.empty");
    anvil_block_t *roots_nonempty = send_block_create(
        lowerer, send_ordinal, "roots.nonempty");
    anvil_block_t *roots_merge = send_block_create(
        lowerer, send_ordinal, "roots.merge");
    if (!capacity_is_zero || !null_roots || !roots_empty || !roots_nonempty
            || !roots_merge
            || !anvil_build_br_cond(lowerer->ctx, capacity_is_zero,
                                    roots_empty, roots_nonempty)
            || !anvil_set_insert_point(lowerer->ctx, roots_empty)
            || !anvil_build_br(lowerer->ctx, roots_merge)
            || !anvil_set_insert_point(lowerer->ctx, roots_nonempty)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *allocated_roots = anvil_build_alloca_dyn(
        lowerer->ctx, lowerer->u64, capacity, "send.roots");
    if (!allocated_roots || !anvil_build_br(lowerer->ctx, roots_merge)
            || !anvil_set_insert_point(lowerer->ctx, roots_merge)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *child_roots = anvil_build_phi(
        lowerer->ctx, lowerer->value_ptr, "send.roots.canonical");
    if (!child_roots
            || !anvil_phi_add_incoming(child_roots, null_roots, roots_empty)
            || !anvil_phi_add_incoming(child_roots, allocated_roots,
                                       roots_nonempty)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *child = anvil_build_alloca(
        lowerer->ctx, lowerer->frame_type, "send.frame");
    anvil_value_t *thread = frame_field(
        lowerer, ST_FRAME_THREAD_FIELD, lowerer->byte_ptr, "thread");
    anvil_value_t *null_byte = anvil_const_null(
        lowerer->ctx, lowerer->byte_ptr);
    anvil_value_t *root_init_arguments[] = { child_roots, capacity };
    anvil_value_t *root_init_status = NULL;
    bool child_ready = code && descriptor && capacity && child && thread
        && null_byte && child_roots && zero32
        && anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->frame_roots_initialize_function),
            root_init_arguments, 2u, "send.roots.status", &root_init_status)
        && root_init_status;
    anvil_value_t *roots_ready = child_ready ? anvil_build_cmp_eq(
        lowerer->ctx, root_init_status,
        anvil_const_u32(lowerer->ctx, ST_AOT_SEND_OK),
        "send.roots.ready") : NULL;
    anvil_block_t *roots_valid = send_block_create(
        lowerer, send_ordinal, "roots.valid");
    anvil_block_t *roots_invalid = send_block_create(
        lowerer, send_ordinal, "roots.invalid");
    if (!roots_ready || !roots_valid || !roots_invalid
            || !anvil_build_br_cond(lowerer->ctx, roots_ready,
                                    roots_valid, roots_invalid)
            || !anvil_set_insert_point(lowerer->ctx, roots_invalid)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *root_failure_arguments[] = {
        lowerer->frame, site, receiver, argv, resolve_arguments[3],
        root_init_status
    };
    anvil_value_t *root_failure = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->send_failure_function),
            root_failure_arguments, 6u, "send.roots.abort", &root_failure)
            || !root_failure || !anvil_build_ret(lowerer->ctx, root_failure)
            || !anvil_set_insert_point(lowerer->ctx, roots_valid)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    child_ready = store_frame_field(
            lowerer, child, ST_FRAME_THREAD_FIELD, thread)
        && store_frame_field(lowerer, child, ST_FRAME_CALLER_FIELD,
                             lowerer->frame)
        && store_frame_field(lowerer, child, ST_FRAME_METHOD_FIELD,
                             descriptor)
        && store_frame_field(lowerer, child, ST_FRAME_HOME_FIELD, null_byte)
        && store_frame_field(lowerer, child, ST_FRAME_RECEIVER_FIELD, receiver)
        && store_frame_field(lowerer, child, ST_FRAME_ARGV_FIELD, argv)
        && store_frame_field(lowerer, child, ST_FRAME_ROOTS_FIELD, child_roots)
        && store_frame_field(lowerer, child, ST_FRAME_ARGC_FIELD,
                             resolve_arguments[3])
        && store_frame_field(lowerer, child, ST_FRAME_ROOT_COUNT_FIELD,
                             capacity)
        && store_frame_field(lowerer, child, ST_FRAME_SAFEPOINT_FIELD, zero32)
        && store_frame_field(lowerer, child, ST_FRAME_FLAGS_FIELD, zero32);
    anvil_value_t *hit_value = NULL;
    if (!child_ready
            || !anvil_build_call_checked(lowerer->ctx, code, &child, 1u,
                                         "send.result", &hit_value)
            || !hit_value
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD, zero32)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_block_t *hit_end = anvil_get_insert_block(lowerer->ctx);

    if (!anvil_set_insert_point(lowerer->ctx, miss)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *failure_arguments[] = {
        lowerer->frame, site, receiver, argv, resolve_arguments[3], status
    };
    anvil_value_t *miss_value = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->send_failure_function),
            failure_arguments, 6u, "send.failure", &miss_value)
            || !miss_value
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD, zero32)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_block_t *miss_end = anvil_get_insert_block(lowerer->ctx);
    anvil_block_t *merge = send_block_create(
        lowerer, send_ordinal, "merge");
    if (!merge
            || !anvil_set_insert_point(lowerer->ctx, hit_end)
            || !anvil_build_br(lowerer->ctx, merge)
            || !anvil_set_insert_point(lowerer->ctx, miss_end)
            || !anvil_build_br(lowerer->ctx, merge)
            || !anvil_set_insert_point(lowerer->ctx, merge)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *result = anvil_build_phi(
        lowerer->ctx, lowerer->u64, "send.value");
    if (!result || !anvil_phi_add_incoming(result, hit_value, hit_end)
            || !anvil_phi_add_incoming(result, miss_value, miss_end)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    /* Published maps are intentionally all-live.  Clear this completed send
     * slice so a later safepoint cannot retain its old receiver/arguments;
     * an enclosing send's active slice remains rooted. */
    if (!clear_scratch_roots(lowerer, send_base, (uint32_t)arity + 1u)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    lowerer->scratch_depth = saved_depth;
    if (!check_control_pending(lowerer, message))
        return failed_value();
    return normal_value(result);
}

static lowered_value_t lower_image_load(
    lowerer_t *lowerer, anvil_func_t *function, uint32_t index,
    const st_ast_node_t *node, const char *stem)
{
    size_t ordinal = lowerer->next_image_load++;
    anvil_value_t *result_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "image.load.result.addr");
    anvil_value_t *index_value = anvil_const_u32(lowerer->ctx, index);
    anvil_value_t *invalid = anvil_const_u64(lowerer->ctx, 0u);
    anvil_value_t *arguments[] = {
        lowerer->frame, index_value, result_address
    };
    anvil_value_t *status = NULL;
    if (!result_address || !index_value || !invalid
            || !anvil_build_store(lowerer->ctx, invalid, result_address)
            || !anvil_build_call_checked(
                lowerer->ctx, anvil_func_get_value(function), arguments, 3u,
                stem, &status)
            || !status) {
        fail_ir(lowerer, node);
        return failed_value();
    }
    anvil_value_t *ok = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_IMAGE_RUNTIME_OK),
        "image.load.ok");
    char valid_name[96];
    char fatal_name[96];
    int valid_length = snprintf(valid_name, sizeof(valid_name),
                                "image.load.%zu.valid", ordinal);
    int fatal_length = snprintf(fatal_name, sizeof(fatal_name),
                                "image.load.%zu.fatal", ordinal);
    anvil_block_t *valid = valid_length > 0
            && (size_t)valid_length < sizeof(valid_name)
        ? anvil_block_create(lowerer->function, valid_name) : NULL;
    anvil_block_t *fatal = fatal_length > 0
            && (size_t)fatal_length < sizeof(fatal_name)
        ? anvil_block_create(lowerer->function, fatal_name) : NULL;
    if (!ok || !valid || !fatal
            || !anvil_build_br_cond(lowerer->ctx, ok, valid, fatal)
            || !anvil_set_insert_point(lowerer->ctx, fatal)) {
        fail_ir(lowerer, node);
        return failed_value();
    }
    anvil_value_t *fatal_arguments[] = { status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->image_fatal_function),
            fatal_arguments, 2u, "image.load.contract.abort", &fatal_value)
            || !fatal_value || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, valid)) {
        fail_ir(lowerer, node);
        return failed_value();
    }
    anvil_value_t *value = anvil_build_load(
        lowerer->ctx, lowerer->u64, result_address, "image.load.value");
    if (!value) {
        fail_ir(lowerer, node);
        return failed_value();
    }
    return normal_value(value);
}

static bool build_instance_variable_arguments(
    lowerer_t *lowerer,
    const st_sema_binding_t *binding,
    anvil_value_t *stored_value,
    anvil_value_t **arguments_out,
    size_t *argument_count_out)
{
    size_t argument_count = stored_value == NULL ? 1u : 2u;
    anvil_type_t *array_type = anvil_type_array(
        lowerer->ctx, lowerer->u64, argument_count);
    anvil_value_t *array = array_type != NULL
        ? anvil_build_alloca(
            lowerer->ctx, array_type, "ivar.arguments")
        : NULL;
    uint64_t encoded_index;

    *arguments_out = NULL;
    *argument_count_out = 0u;
    if (binding->slot == ST_SEMA_INVALID_ID
            || !st_value_from_small_integer(
                (int64_t)binding->slot + 1, &encoded_index)
            || array == NULL) {
        return false;
    }

    anvil_value_t *values[2] = {
        anvil_const_u64(lowerer->ctx, encoded_index),
        stored_value
    };
    for (size_t index = 0u; index < argument_count; index++) {
        anvil_value_t *zero = anvil_const_u32(lowerer->ctx, 0u);
        anvil_value_t *element = anvil_const_u32(
            lowerer->ctx, (uint32_t)index);
        anvil_value_t *indices[] = { zero, element };
        anvil_value_t *address = zero != NULL && element != NULL
            ? anvil_build_gep(
                lowerer->ctx, array_type, array, indices, 2u,
                "ivar.argument")
            : NULL;

        if (values[index] == NULL || address == NULL
                || !anvil_build_store(
                    lowerer->ctx, values[index], address)) {
            return false;
        }
        if (index == 0u) {
            *arguments_out = address;
        }
    }
    *argument_count_out = argument_count;
    return true;
}

static lowered_value_t lower_instance_variable_access(
    lowerer_t *lowerer,
    const st_sema_binding_t *binding,
    anvil_value_t *stored_value,
    const st_ast_node_t *node)
{
    /* These two fixed-slot intrinsics are non-allocating. They authenticate
     * the receiver and shape through the heap primitive context, and stores
     * execute the heap write barrier. Consequently this call is not a GC
     * safepoint and a value already held by the activation remains valid. */
    size_t ordinal = lowerer->next_heap_access++;
    uint32_t intrinsic_id = stored_value == NULL
        ? ST_INTRINSIC_INST_VAR_AT
        : ST_INTRINSIC_INST_VAR_AT_PUT;
    anvil_value_t *arguments = NULL;
    size_t argument_count = 0u;
    anvil_value_t *result_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "ivar.result.addr");
    anvil_value_t *invalid = anvil_const_u64(
        lowerer->ctx, ST_VALUE_INVALID);
    anvil_value_t *intrinsic = anvil_const_u32(
        lowerer->ctx, intrinsic_id);
    anvil_value_t *status = NULL;

    if (result_address == NULL || invalid == NULL || intrinsic == NULL
            || !build_instance_variable_arguments(
                lowerer, binding, stored_value,
                &arguments, &argument_count)
            || !anvil_build_store(
                lowerer->ctx, invalid, result_address)) {
        fail_ir(lowerer, node);
        return failed_value();
    }

    anvil_value_t *execute_arguments[] = {
        lowerer->frame,
        intrinsic,
        lowerer->self,
        arguments,
        anvil_const_u64(lowerer->ctx, argument_count),
        result_address
    };
    if (execute_arguments[4] == NULL
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(
                    lowerer->heap_primitive_execute_function),
                execute_arguments, 6u, "ivar.status", &status)
            || status == NULL) {
        fail_ir(lowerer, node);
        return failed_value();
    }

    anvil_value_t *succeeded = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_HEAP_PRIMITIVE_OK),
        "ivar.succeeded");
    char success_name[80];
    char failure_name[80];
    int success_length = snprintf(
        success_name, sizeof(success_name),
        "ivar.access.%zu.success", ordinal);
    int failure_length = snprintf(
        failure_name, sizeof(failure_name),
        "ivar.access.%zu.contract.failure", ordinal);
    anvil_block_t *success = success_length > 0
            && (size_t)success_length < sizeof(success_name)
        ? anvil_block_create(lowerer->function, success_name)
        : NULL;
    anvil_block_t *failure = failure_length > 0
            && (size_t)failure_length < sizeof(failure_name)
        ? anvil_block_create(lowerer->function, failure_name)
        : NULL;
    if (succeeded == NULL || success == NULL || failure == NULL
            || !anvil_build_br_cond(
                lowerer->ctx, succeeded, success, failure)
            || !anvil_set_insert_point(lowerer->ctx, failure)) {
        fail_ir(lowerer, node);
        return failed_value();
    }

    anvil_value_t *fatal_arguments[] = {
        intrinsic, status, lowerer->frame
    };
    anvil_value_t *fatal_value = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(
                lowerer->heap_primitive_fatal_function),
            fatal_arguments, 3u,
            "ivar.contract.abort", &fatal_value)
            || fatal_value == NULL
            || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, success)) {
        fail_ir(lowerer, node);
        return failed_value();
    }

    anvil_value_t *result = anvil_build_load(
        lowerer->ctx, lowerer->u64,
        result_address, "ivar.result");
    if (result == NULL) {
        fail_ir(lowerer, node);
        return failed_value();
    }
    return normal_value(result);
}

static lowered_value_t lower_variable(lowerer_t *lowerer,
                                      const st_ast_node_t *node)
{
    const st_sema_reference_t *reference = st_sema_reference_for_node(
        lowerer->sema, node);
    if (reference == NULL || reference->binding >= lowerer->locations_count) {
        set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, node,
                       static_string("missing semantic variable reference"));
        return failed_value();
    }
    const st_sema_binding_t *binding =
        &lowerer->sema->bindings[reference->binding];
    if (binding->kind == ST_SEMA_BIND_SELF
            || binding->kind == ST_SEMA_BIND_SUPER)
        return normal_value(lowerer->self);
    if (binding->kind == ST_SEMA_BIND_GLOBAL
            || binding->kind == ST_SEMA_BIND_FORWARD_GLOBAL) {
        const st_lower_global_binding_t *mapped = find_global_binding(
            lowerer->globals, lowerer->global_count, binding->external_id);
        if ((binding->flags & ST_SEMA_BINDING_EXTERNAL) == 0u
                || mapped == NULL) {
            set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                           ST_LOWER_DIAG_INVALID_INPUT, node,
                           static_string("global binding plan changed after preflight"));
            return failed_value();
        }
        return lower_image_load(
            lowerer, lowerer->image_global_load_function,
            mapped->runtime_index, node, "image.global.status");
    }
    if (binding->kind == ST_SEMA_BIND_INSTANCE_VARIABLE) {
        return lower_instance_variable_access(
            lowerer, binding, NULL, node);
    }
    binding_location_t location = lowerer->locations[reference->binding];
    if (location.value == NULL) {
        set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, node,
                       static_string("semantic binding has no lowered location"));
        return failed_value();
    }
    if (location.cell) {
        anvil_value_t *result_address = anvil_build_alloca(
            lowerer->ctx, lowerer->u64, "cell.load.result");
        anvil_value_t *arguments[] = {
            lowerer->frame, location.value, result_address
        };
        anvil_value_t *status = NULL;
        if (!result_address || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->closure_cell_load_function),
                arguments, 3u, "cell.load.status", &status)
                || !status || !closure_status_or_abort(
                    lowerer, status, node, "cell.load.ok"))
            return failed_value();
        anvil_value_t *loaded = anvil_build_load(
            lowerer->ctx, lowerer->u64, result_address, "cell.value");
        return loaded ? normal_value(loaded) : failed_value();
    }
    if (!location.address) return normal_value(location.value);
    anvil_value_t *value = anvil_build_load(lowerer->ctx, lowerer->u64,
                                             location.value, "temporary");
    if (!value) fail_ir(lowerer, node);
    return normal_value(value);
}

static anvil_value_t *capture_value(lowerer_t *lowerer,
                                    st_sema_binding_id_t binding_id)
{
    if (binding_id >= lowerer->locations_count) return NULL;
    const st_sema_binding_t *binding = &lowerer->sema->bindings[binding_id];
    if (binding->kind == ST_SEMA_BIND_SELF) return lowerer->self;
    binding_location_t location = lowerer->locations[binding_id];
    if (!location.value) return NULL;
    return location.address
        ? anvil_build_load(lowerer->ctx, lowerer->u64, location.value,
                           "closure.capture")
        : location.value;
}

static bool closure_status_or_abort(lowerer_t *lowerer,
                                    anvil_value_t *status,
                                    const st_ast_node_t *node,
                                    const char *stem)
{
    anvil_value_t *ok = status ? anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_AOT_CLOSURE_OK), stem) : NULL;
    anvil_block_t *valid = control_block_create(lowerer, "closure.valid");
    anvil_block_t *fatal = control_block_create(lowerer, "closure.fatal");
    if (!ok || !valid || !fatal
            || !anvil_build_br_cond(lowerer->ctx, ok, valid, fatal)
            || !anvil_set_insert_point(lowerer->ctx, fatal)
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, 0u))) {
        fail_ir(lowerer, node);
        return false;
    }
    anvil_value_t *arguments[] = { status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    if (!anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->closure_fatal_function),
            arguments, 2u, "closure.contract.abort", &fatal_value)
            || !fatal_value || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, valid)) {
        fail_ir(lowerer, node);
        return false;
    }
    return true;
}

static lowered_value_t lower_escaping_block(lowerer_t *lowerer,
                                            const st_ast_node_t *block)
{
    const st_sema_block_t *info = st_sema_block_for_node(
        lowerer->sema, block);
    if (!info) return failed_value();
    size_t semantic_index = (size_t)(info - lowerer->sema->blocks);
    size_t block_index;
    if (!sema_block_artifact_index(
            lowerer->sema, lowerer->graph_method->node,
            semantic_index, &block_index)
            || block_index >= lowerer->block_count)
        return failed_value();
    size_t capture_count = info->capture_count;
    uint32_t saved_depth = lowerer->scratch_depth;
    uint32_t scratch_count = (uint32_t)capture_count + 2u;
    uint32_t base = saved_depth;
    lowerer->scratch_depth += scratch_count;
    anvil_value_t *nil = anvil_const_u64(
        lowerer->ctx, ST_VALUE_TAG_SPECIAL);
    if (!nil || !root_store(lowerer, lowerer->scratch_root_offset + base,
                            lowerer->self)) {
        fail_ir(lowerer, block);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *captures_pointer = anvil_const_null(
        lowerer->ctx, lowerer->value_ptr);
    anvil_value_t *captures_array = NULL;
    anvil_type_t *captures_type = NULL;
    if (capture_count != 0u) {
        captures_type = anvil_type_array(
            lowerer->ctx, lowerer->u64, capture_count);
        captures_array = captures_type ? anvil_build_alloca(
            lowerer->ctx, captures_type, "closure.captures") : NULL;
        if (!captures_type || !captures_array) {
            fail_ir(lowerer, block);
            lowerer->scratch_depth = saved_depth;
            return failed_value();
        }
        for (size_t index = 0u; index < capture_count; index++) {
            const st_sema_capture_t *capture = &lowerer->sema->captures[
                info->capture_offset + index];
            anvil_value_t *value = capture_value(lowerer, capture->binding);
            anvil_value_t *zero = anvil_const_u32(lowerer->ctx, 0u);
            anvil_value_t *element = anvil_const_u32(
                lowerer->ctx, (uint32_t)index);
            anvil_value_t *indices[] = { zero, element };
            anvil_value_t *address = value && zero && element
                ? anvil_build_gep(lowerer->ctx, captures_type,
                                  captures_array, indices, 2u,
                                  "closure.capture.addr") : NULL;
            if (!address
                    || !anvil_build_store(lowerer->ctx, value, address)
                    || !root_store(
                        lowerer,
                        lowerer->scratch_root_offset + base
                            + (uint32_t)index + 1u,
                        value)) {
                fail_ir(lowerer, block);
                lowerer->scratch_depth = saved_depth;
                return failed_value();
            }
            if (index == 0u) captures_pointer = address;
        }
    }
    uint32_t result_slot = lowerer->scratch_root_offset + base
        + (uint32_t)capture_count + 1u;
    anvil_value_t *result_address = root_address(
        lowerer, result_slot, "closure.result.addr");
    anvil_value_t *descriptor = lowerer->closure_descriptor_globals
            && lowerer->closure_descriptor_globals[block_index]
        ? anvil_const_symbol_addr(
            lowerer->closure_descriptor_globals[block_index]) : NULL;
    uint32_t safepoint = ++lowerer->next_safepoint_id;
    anvil_value_t *arguments[] = {
        lowerer->frame, descriptor, lowerer->self, captures_pointer,
        anvil_const_u32(lowerer->ctx, (uint32_t)capture_count), result_address
    };
    anvil_value_t *status = NULL;
    if (!captures_pointer || !result_address || !descriptor || !arguments[4]
            || !anvil_build_store(lowerer->ctx, nil, result_address)
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, safepoint))
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->closure_create_function),
                arguments, 6u, "closure.create.status", &status)
            || !status
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, 0u))
            || !closure_status_or_abort(
                lowerer, status, block, "closure.create.ok")) {
        fail_ir(lowerer, block);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *closure = anvil_build_load(
        lowerer->ctx, lowerer->u64, result_address, "closure.value");
    if (!closure || !clear_scratch_roots(lowerer, base, scratch_count)) {
        fail_ir(lowerer, block);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    lowerer->scratch_depth = saved_depth;
    return normal_value(closure);
}

static lowered_value_t lower_direct_closure_call(
    lowerer_t *lowerer, const st_ast_node_t *message,
    anvil_value_t *closure, uint32_t arity,
    const st_lower_block_artifact_t *literal_artifact)
{
    uint32_t saved_depth = lowerer->scratch_depth;
    uint32_t base = saved_depth;
    lowerer->scratch_depth += arity + 1u;
    if (!root_store(lowerer, lowerer->scratch_root_offset + base, closure)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *argv = anvil_const_null(
        lowerer->ctx, lowerer->value_ptr);
    anvil_type_t *argv_type = NULL;
    anvil_value_t *argv_array = NULL;
    if (arity != 0u) {
        argv_type = anvil_type_array(lowerer->ctx, lowerer->u64, arity);
        argv_array = argv_type ? anvil_build_alloca(
            lowerer->ctx, argv_type, "closure.argv") : NULL;
        if (!argv_type || !argv_array) {
            fail_ir(lowerer, message);
            lowerer->scratch_depth = saved_depth;
            return failed_value();
        }
        for (uint32_t index = 0u; index < arity; index++) {
            lowered_value_t argument = lower_value(
                lowerer, message->as.message.arguments.items[index]);
            anvil_value_t *zero = anvil_const_u32(lowerer->ctx, 0u);
            anvil_value_t *element = anvil_const_u32(lowerer->ctx, index);
            anvil_value_t *indices[] = { zero, element };
            anvil_value_t *address = argument.value && !argument.terminated
                && zero && element
                ? anvil_build_gep(lowerer->ctx, argv_type, argv_array,
                                  indices, 2u, "closure.argv.addr") : NULL;
            if (!address || !anvil_build_store(
                    lowerer->ctx, argument.value, address)
                    || !root_store(
                        lowerer, lowerer->scratch_root_offset + base
                            + index + 1u, argument.value)) {
                fail_ir(lowerer, message);
                lowerer->scratch_depth = saved_depth;
                return failed_value();
            }
            if (index == 0u) argv = address;
        }
    }
    anvil_value_t *target = anvil_build_alloca(
        lowerer->ctx, lowerer->closure_target_type, "closure.target");
    uint32_t safepoint = ++lowerer->next_safepoint_id;
    anvil_value_t *resolve_arguments[] = {
        lowerer->frame, closure, anvil_const_u32(lowerer->ctx, arity), target
    };
    anvil_value_t *status = NULL;
    if (!argv || !target || !resolve_arguments[2]
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, safepoint))
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->closure_resolve_function),
                resolve_arguments, 4u, "closure.resolve.status", &status)
            || !status
            || !closure_status_or_abort(
                lowerer, status, message, "closure.resolve.ok")) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *code_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->closure_target_type, target, 0u,
        "closure.code.addr");
    anvil_value_t *method_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->closure_target_type, target, 1u,
        "closure.method.addr");
    anvil_value_t *home_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->closure_target_type, target, 2u,
        "closure.home.addr");
    anvil_value_t *capacity_address = anvil_build_struct_gep(
        lowerer->ctx, lowerer->closure_target_type, target, 3u,
        "closure.capacity.addr");
    anvil_value_t *code = code_address ? anvil_build_load(
        lowerer->ctx, lowerer->method_ptr, code_address, "closure.code") : NULL;
    anvil_value_t *method = method_address ? anvil_build_load(
        lowerer->ctx, lowerer->byte_ptr, method_address, "closure.method") : NULL;
    anvil_value_t *home = home_address ? anvil_build_load(
        lowerer->ctx, lowerer->byte_ptr, home_address, "closure.home") : NULL;
    anvil_value_t *capacity = capacity_address ? anvil_build_load(
        lowerer->ctx, lowerer->u32, capacity_address, "closure.capacity") : NULL;
    uint32_t expected_capacity = literal_artifact->required_root_capacity;
    anvil_value_t *capacity_ok = capacity ? anvil_build_cmp_eq(
        lowerer->ctx, capacity,
        anvil_const_u32(lowerer->ctx, expected_capacity),
        "closure.capacity.ok") : NULL;
    anvil_block_t *capacity_valid = control_block_create(
        lowerer, "closure.capacity.valid");
    anvil_block_t *capacity_fatal = control_block_create(
        lowerer, "closure.capacity.fatal");
    if (!code || !method || !home || !capacity_ok || !capacity_valid
            || !capacity_fatal
            || !anvil_build_br_cond(lowerer->ctx, capacity_ok,
                                    capacity_valid, capacity_fatal)
            || !anvil_set_insert_point(lowerer->ctx, capacity_fatal)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *invalid_status = anvil_const_u32(
        lowerer->ctx, ST_AOT_CLOSURE_ERR_INVALID_DESCRIPTOR);
    anvil_value_t *fatal_arguments[] = { invalid_status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    if (!invalid_status || !store_frame_field(
            lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
            anvil_const_u32(lowerer->ctx, 0u))
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->closure_fatal_function),
                fatal_arguments, 2u, "closure.capacity.abort", &fatal_value)
            || !fatal_value || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, capacity_valid)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    anvil_value_t *child_roots = anvil_const_null(
        lowerer->ctx, lowerer->value_ptr);
    if (expected_capacity != 0u) {
        anvil_type_t *roots_type = anvil_type_array(
            lowerer->ctx, lowerer->u64, expected_capacity);
        anvil_value_t *roots_array = roots_type ? anvil_build_alloca(
            lowerer->ctx, roots_type, "closure.child.roots") : NULL;
        if (!roots_type || !roots_array) {
            fail_ir(lowerer, message);
            lowerer->scratch_depth = saved_depth;
            return failed_value();
        }
        for (uint32_t index = 0u; index < expected_capacity; index++) {
            anvil_value_t *zero = anvil_const_u32(lowerer->ctx, 0u);
            anvil_value_t *element = anvil_const_u32(lowerer->ctx, index);
            anvil_value_t *indices[] = { zero, element };
            anvil_value_t *address = zero && element ? anvil_build_gep(
                lowerer->ctx, roots_type, roots_array, indices, 2u,
                "closure.child.root.addr") : NULL;
            anvil_value_t *initial = index == 0u ? closure
                : index <= arity
                    ? load_root(lowerer,
                        lowerer->scratch_root_offset + base + index,
                        "closure.child.argument")
                    : anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL);
            if (!address || !initial
                    || !anvil_build_store(lowerer->ctx, initial, address)) {
                fail_ir(lowerer, message);
                lowerer->scratch_depth = saved_depth;
                return failed_value();
            }
            if (index == 0u) child_roots = address;
        }
    }
    anvil_value_t *child = anvil_build_alloca(
        lowerer->ctx, lowerer->frame_type, "closure.frame");
    anvil_value_t *thread = frame_field(
        lowerer, ST_FRAME_THREAD_FIELD, lowerer->byte_ptr, "thread");
    anvil_value_t *zero32 = anvil_const_u32(lowerer->ctx, 0u);
    bool ready = child && thread && child_roots && zero32
        && store_frame_field(lowerer, child, ST_FRAME_THREAD_FIELD, thread)
        && store_frame_field(lowerer, child, ST_FRAME_CALLER_FIELD,
                             lowerer->frame)
        && store_frame_field(lowerer, child, ST_FRAME_METHOD_FIELD, method)
        && store_frame_field(lowerer, child, ST_FRAME_HOME_FIELD, home)
        && store_frame_field(lowerer, child, ST_FRAME_RECEIVER_FIELD, closure)
        && store_frame_field(lowerer, child, ST_FRAME_ARGV_FIELD, argv)
        && store_frame_field(lowerer, child, ST_FRAME_ROOTS_FIELD, child_roots)
        && store_frame_field(lowerer, child, ST_FRAME_ARGC_FIELD,
                             resolve_arguments[2])
        && store_frame_field(lowerer, child, ST_FRAME_ROOT_COUNT_FIELD,
                             anvil_const_u32(lowerer->ctx, expected_capacity))
        && store_frame_field(lowerer, child, ST_FRAME_SAFEPOINT_FIELD, zero32)
        && store_frame_field(lowerer, child, ST_FRAME_FLAGS_FIELD, zero32);
    anvil_value_t *result = NULL;
    if (!ready || !anvil_build_call_checked(
            lowerer->ctx, code, &child, 1u, "closure.result", &result)
            || !result || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD, zero32)
            || !clear_scratch_roots(lowerer, base, arity + 1u)) {
        fail_ir(lowerer, message);
        lowerer->scratch_depth = saved_depth;
        return failed_value();
    }
    lowerer->scratch_depth = saved_depth;
    if (!check_control_pending(lowerer, message)) return failed_value();
    return normal_value(result);
}

static lowered_value_t lower_string_literal(lowerer_t *lowerer,
                                             const st_ast_node_t *node)
{
    if (lowerer->string_literals == NULL
            || lowerer->next_string_literal
                >= lowerer->string_literal_capacity
            || lowerer->next_string_literal > UINT32_MAX
            || lowerer->literal_base_index
                > UINT32_MAX - (uint32_t)lowerer->next_string_literal
            || lowerer->next_string_byte
                > lowerer->string_literal_byte_capacity
            || node->as.text.length
                > lowerer->string_literal_byte_capacity
                    - lowerer->next_string_byte) {
        set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, node,
                       static_string("String literal plan changed after preflight"));
        return failed_value();
    }
    st_lower_string_literal_artifact_t *artifact =
        &lowerer->string_literals[lowerer->next_string_literal];
    artifact->literal_id = lowerer->literal_base_index
        + (uint32_t)lowerer->next_string_literal;
    artifact->length = node->as.text.length;
    if (node->as.text.length != 0u) {
        artifact->bytes = lowerer->string_literal_bytes
            + lowerer->next_string_byte;
        memcpy(lowerer->string_literal_bytes + lowerer->next_string_byte,
               node->as.text.data, node->as.text.length);
    }
    lowerer->next_string_byte += node->as.text.length;
    lowerer->next_string_literal++;
    return lower_image_load(
        lowerer, lowerer->image_literal_load_function,
        artifact->literal_id, node, "image.literal.status");
}

static lowered_value_t lower_value(lowerer_t *lowerer,
                                   const st_ast_node_t *node)
{
    uint64_t encoded;
    switch (node->kind) {
    case ST_AST_EXPRESSION:
        return lower_expression(lowerer, node);
    case ST_AST_VARIABLE:
        return lower_variable(lowerer, node);
    case ST_AST_NIL:
        encoded = ST_VALUE_TAG_SPECIAL;
        break;
    case ST_AST_FALSE:
        encoded = (UINT64_C(1) << ST_VALUE_TAG_BITS) | ST_VALUE_TAG_SPECIAL;
        break;
    case ST_AST_TRUE:
        encoded = (UINT64_C(2) << ST_VALUE_TAG_BITS) | ST_VALUE_TAG_SPECIAL;
        break;
    case ST_AST_INTEGER:
        if (!parse_small_integer(node, &encoded)) return failed_value();
        break;
    case ST_AST_CHARACTER:
        encoded = ((uint64_t)node->as.character << ST_VALUE_TAG_BITS)
                | ST_VALUE_TAG_CHARACTER;
        break;
    case ST_AST_STRING:
        return lower_string_literal(lowerer, node);
    case ST_AST_BLOCK:
        return lower_escaping_block(lowerer, node);
    default:
        return failed_value();
    }
    return normal_value(anvil_const_u64(lowerer->ctx, encoded));
}

static lowered_value_t lower_expression(lowerer_t *lowerer,
                                        const st_ast_node_t *expression)
{
    bool has_cascade = false;
    uint32_t cascade_depth = lowerer->scratch_depth;
    lowered_value_t result = lower_value(
        lowerer, expression->as.expression.receiver);
    if (result.value == NULL || result.terminated) {
        if (result.value == NULL && lowerer->result->status == ST_LOWER_OK)
            fail_ir(lowerer, expression);
        return result;
    }
    for (size_t index = 0u;
         index < expression->as.expression.messages.count; index++) {
        const st_ast_node_t *message =
            expression->as.expression.messages.items[index];
        if (message != NULL && message->kind == ST_AST_MESSAGE
                && message->as.message.starts_cascade)
            has_cascade = true;
    }
    if (has_cascade) {
        if (cascade_depth == UINT32_MAX
                || !root_store(lowerer,
                    lowerer->scratch_root_offset + cascade_depth,
                    result.value)) {
            fail_ir(lowerer, expression);
            return failed_value();
        }
        lowerer->scratch_depth = cascade_depth + 1u;
    }
    if (expression->as.expression.messages.count != 0u) {
        if (expression_uses_direct_closure_call(expression)) {
            const st_ast_node_t *message =
                expression->as.expression.messages.items[0];
            const st_sema_block_t *info = st_sema_block_for_node(
                lowerer->sema, expression->as.expression.receiver);
            size_t artifact_index;
            if (!info || !sema_block_artifact_index(
                    lowerer->sema, lowerer->graph_method->node,
                    (size_t)(info - lowerer->sema->blocks),
                    &artifact_index)
                    || artifact_index >= lowerer->block_count) {
                fail_ir(lowerer, expression);
                return failed_value();
            }
            result = lower_direct_closure_call(
                lowerer, message, result.value,
                (uint32_t)expression->as.expression.receiver
                    ->as.block.arguments.count,
                &lowerer->block_artifacts[artifact_index]);
            if (result.value == NULL || result.terminated) goto finish_cascade;
        } else if (expression_uses_boolean_intrinsic(expression)) {
            result = lower_boolean_send(lowerer, expression, result.value);
            if (result.value == NULL || result.terminated) goto finish_cascade;
        } else {
            for (size_t index = 0u;
                 index < expression->as.expression.messages.count; index++) {
                const st_ast_node_t *message =
                    expression->as.expression.messages.items[index];
                if (message->as.message.starts_cascade) {
                    result.value = load_root(
                        lowerer,
                        lowerer->scratch_root_offset + cascade_depth,
                        "cascade.receiver");
                    if (result.value == NULL) {
                        fail_ir(lowerer, message);
                        result = failed_value();
                        goto finish_cascade;
                    }
                }
                result = lower_general_send(
                    lowerer, message, result.value);
                if (result.value == NULL || result.terminated)
                    goto finish_cascade;
            }
        }
    }
finish_cascade:
    if (has_cascade) {
        if (result.value != NULL && !result.terminated
                && !clear_scratch_roots(lowerer, cascade_depth, 1u)) {
            if (lowerer->result->status == ST_LOWER_OK)
                fail_ir(lowerer, expression);
            result = failed_value();
        }
        lowerer->scratch_depth = cascade_depth;
    }
    if (result.value == NULL || result.terminated) return result;
    for (size_t index = 0u;
         index < expression->as.expression.assignments.count; index++) {
        const st_ast_node_t *target =
            expression->as.expression.assignments.items[index];
        const st_sema_reference_t *reference = st_sema_reference_for_node(
            lowerer->sema, target);
        if (reference == NULL || reference->binding >= lowerer->locations_count) {
            fail_ir(lowerer, target);
            return failed_value();
        }
        const st_sema_binding_t *binding =
            &lowerer->sema->bindings[reference->binding];
        if (binding->kind == ST_SEMA_BIND_INSTANCE_VARIABLE) {
            lowered_value_t stored = lower_instance_variable_access(
                lowerer, binding, result.value, target);
            if (stored.value == NULL || stored.terminated) {
                return failed_value();
            }
            continue;
        }
        binding_location_t *location =
            &lowerer->locations[reference->binding];
        if (location->cell) {
            anvil_value_t *arguments[] = {
                lowerer->frame, location->value, result.value
            };
            anvil_value_t *status = NULL;
            if (!anvil_build_call_checked(
                    lowerer->ctx,
                    anvil_func_get_value(lowerer->closure_cell_store_function),
                    arguments, 3u, "cell.store.status", &status)
                    || !status || !closure_status_or_abort(
                        lowerer, status, target, "cell.store.ok")) {
                fail_ir(lowerer, target);
                return failed_value();
            }
        } else if (!location->address || !anvil_build_store(
                lowerer->ctx, result.value, location->value)) {
            fail_ir(lowerer, target);
            return failed_value();
        }
    }
    if (expression->as.expression.returns) {
        const st_sema_return_t *return_info = NULL;
        for (size_t index = 0u; index < lowerer->sema->return_count; index++) {
            if (lowerer->sema->returns[index].expression == expression) {
                return_info = &lowerer->sema->returns[index];
                break;
            }
        }
        if (return_info == NULL) {
            set_diagnostic(lowerer->result, ST_LOWER_ERR_INVALID_ARGUMENT,
                           ST_LOWER_DIAG_INVALID_INPUT, expression,
                           static_string("return is absent from semantic result"));
            return failed_value();
        }
        if (return_info->kind == ST_SEMA_RETURN_HOME_METHOD) {
            anvil_value_t *home = frame_field(
                lowerer, ST_FRAME_HOME_FIELD, lowerer->byte_ptr,
                "control.home");
            anvil_value_t *arguments[] = {
                lowerer->frame, home, result.value
            };
            anvil_value_t *status = NULL;
            if (!lowerer->control_scope || !home
                    || !anvil_build_call_checked(
                        lowerer->ctx,
                        anvil_func_get_value(lowerer->control_nlr_function),
                        arguments, 3u, "control.nlr.status", &status)
                    || !control_status_or_abort(
                        lowerer, status, "control.nlr.valid")
                    || !emit_control_return(lowerer, result.value)) {
                fail_ir(lowerer, expression);
                return failed_value();
            }
            return terminated_value();
        }
        if (return_info->kind != ST_SEMA_RETURN_LOCAL_METHOD
                || !(lowerer->control_scope
                     ? emit_control_return(lowerer, result.value)
                     : anvil_build_ret(lowerer->ctx, result.value))) {
            fail_ir(lowerer, expression);
            return failed_value();
        }
        return terminated_value();
    }
    return result;
}

static bool frame_layout_matches(anvil_type_t *frame_type)
{
    return frame_type != NULL
        && anvil_type_struct_field_count(frame_type) == 11u
        && anvil_type_size(frame_type) == sizeof(StFrame)
        && anvil_type_align(frame_type) == _Alignof(StFrame)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_THREAD_FIELD)
            == offsetof(StFrame, thread)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_CALLER_FIELD)
            == offsetof(StFrame, caller)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_METHOD_FIELD)
            == offsetof(StFrame, method)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_HOME_FIELD)
            == offsetof(StFrame, home)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_RECEIVER_FIELD)
            == offsetof(StFrame, receiver)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_ARGV_FIELD)
            == offsetof(StFrame, argv)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_ROOTS_FIELD)
            == offsetof(StFrame, roots)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_ARGC_FIELD)
            == offsetof(StFrame, argc)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_ROOT_COUNT_FIELD)
            == offsetof(StFrame, root_count)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_SAFEPOINT_FIELD)
            == offsetof(StFrame, safepoint_id)
        && anvil_type_struct_field_offset(frame_type, ST_FRAME_FLAGS_FIELD)
            == offsetof(StFrame, flags);
}

static bool build_types(lowerer_t *lowerer, anvil_type_t **method_type_out)
{
    anvil_ctx_t *ctx = lowerer->ctx;
    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *u32 = anvil_type_u32(ctx);
    anvil_type_t *byte_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *value_ptr = u64 ? anvil_type_ptr(ctx, u64) : NULL;
    anvil_type_t *frame_type = anvil_type_named_struct(ctx, "StFrame");
    anvil_type_t *frame_ptr = frame_type
        ? anvil_type_ptr(ctx, frame_type) : NULL;
    if (!u64 || !u32 || !byte_ptr || !value_ptr || !frame_type || !frame_ptr)
        return false;
    anvil_type_t *fields[] = {
        byte_ptr, frame_ptr, byte_ptr, byte_ptr, u64, value_ptr, value_ptr,
        u32, u32, u32, u32
    };
    if (anvil_type_struct_is_opaque(frame_type)) {
        if (!anvil_type_struct_set_body(frame_type, fields, 11u, false))
            return false;
    } else {
        if (anvil_type_struct_field_count(frame_type) != 11u) return false;
    }
    if (!frame_layout_matches(frame_type)) return false;
    anvil_type_t *parameters[] = { frame_ptr };
    anvil_type_t *method_type = anvil_type_func(
        ctx, u64, parameters, 1u, false);
    anvil_type_t *method_ptr = method_type
        ? anvil_type_ptr(ctx, method_type) : NULL;
    if (!method_ptr) return false;
    lowerer->frame_type = frame_type;
    lowerer->u64 = u64;
    lowerer->u32 = u32;
    lowerer->byte_ptr = byte_ptr;
    lowerer->value_ptr = value_ptr;
    lowerer->method_ptr = method_ptr;
    *method_type_out = method_type;
    return true;
}

static bool set_named_struct_body(anvil_type_t *type,
                                  anvil_type_t **fields, size_t count)
{
    if (anvil_type_struct_is_opaque(type))
        return anvil_type_struct_set_body(type, fields, count, false);
    return anvil_type_struct_field_count(type) == count;
}

static bool build_send_abi(lowerer_t *lowerer)
{
    anvil_ctx_t *ctx = lowerer->ctx;
    anvil_type_t *pic_slot = anvil_type_named_struct(ctx, "StPicSlot");
    anvil_type_t *pic_fields[] = {
        lowerer->u64, lowerer->u32, lowerer->u32,
        lowerer->byte_ptr, lowerer->byte_ptr
    };
    if (!pic_slot || !set_named_struct_body(pic_slot, pic_fields, 5u))
        return false;
    anvil_type_t *pic_array = anvil_type_array(ctx, pic_slot, ST_PIC_WAYS);
    anvil_type_t *send_site = anvil_type_named_struct(ctx, "StSendSite");
    anvil_type_t *site_fields[] = {
        lowerer->u32, lowerer->u32, lowerer->u32, pic_array,
        anvil_type_u8(ctx)
    };
    if (!pic_array || !site_fields[4] || !send_site
            || !set_named_struct_body(send_site, site_fields, 5u)
            || anvil_type_size(pic_slot) != sizeof(st_pic_slot_t)
            || anvil_type_struct_field_offset(send_site, 3u)
                != offsetof(st_send_site_t, slots)
            || anvil_type_struct_field_offset(send_site, 4u)
                != offsetof(st_send_site_t, initialized)
            || anvil_type_size(send_site) != sizeof(st_send_site_t))
        return false;
    anvil_type_t *send_target = anvil_type_named_struct(
        ctx, "StAotSendTarget");
    anvil_type_t *target_fields[] = {
        lowerer->method_ptr, lowerer->byte_ptr, lowerer->u32, lowerer->u32
    };
    if (!send_target
            || !set_named_struct_body(send_target, target_fields, 4u)
            || anvil_type_size(send_target) != sizeof(st_aot_send_target_t))
        return false;
    lowerer->send_site_type = send_site;
    lowerer->send_site_ptr = anvil_type_ptr(ctx, send_site);
    lowerer->send_target_type = send_target;
    lowerer->send_target_ptr = anvil_type_ptr(ctx, send_target);
    anvil_type_t *frame_ptr = anvil_type_ptr(ctx, lowerer->frame_type);
    if (!lowerer->send_site_ptr || !lowerer->send_target_ptr || !frame_ptr)
        return false;

    anvil_type_t *validate_params[] = { frame_ptr, lowerer->u32 };
    anvil_type_t *resolve_params[] = {
        frame_ptr, lowerer->send_site_ptr, lowerer->u64, lowerer->u32,
        lowerer->send_target_ptr
    };
    anvil_type_t *failure_params[] = {
        frame_ptr, lowerer->send_site_ptr, lowerer->u64,
        lowerer->value_ptr, lowerer->u32, lowerer->u32
    };
    anvil_type_t *root_init_params[] = {
        lowerer->value_ptr, lowerer->u32
    };
    anvil_type_t *validate_type = anvil_type_func(
        ctx, lowerer->u32, validate_params, 2u, false);
    anvil_type_t *resolve_type = anvil_type_func(
        ctx, lowerer->u32, resolve_params, 5u, false);
    anvil_type_t *failure_type = anvil_type_func(
        ctx, lowerer->u64, failure_params, 6u, false);
    anvil_type_t *root_init_type = anvil_type_func(
        ctx, lowerer->u32, root_init_params, 2u, false);
    if (!validate_type || !resolve_type || !failure_type || !root_init_type)
        return false;
    lowerer->frame_validate_function = anvil_func_declare(
        lowerer->module, "st_aot_frame_validate", validate_type);
    lowerer->send_resolve_function = anvil_func_declare(
        lowerer->module, "st_aot_send_resolve", resolve_type);
    lowerer->send_failure_function = anvil_func_declare(
        lowerer->module, "st_aot_send_failure", failure_type);
    lowerer->frame_roots_initialize_function = anvil_func_declare(
        lowerer->module, "st_aot_frame_roots_initialize", root_init_type);
    return lowerer->frame_validate_function != NULL
        && lowerer->send_resolve_function != NULL
        && lowerer->send_failure_function != NULL
        && lowerer->frame_roots_initialize_function != NULL;
}

static bool build_primitive_abi(lowerer_t *lowerer)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->frame_type);
    anvil_type_t *execute_params[] = {
        lowerer->u32, lowerer->u64, lowerer->value_ptr, lowerer->u64,
        lowerer->value_ptr
    };
    anvil_type_t *fatal_params[] = {
        lowerer->u32, lowerer->u32, frame_ptr
    };
    anvil_type_t *execute_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, execute_params, 5u, false) : NULL;
    anvil_type_t *fatal_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u64, fatal_params, 3u, false) : NULL;
    if (!execute_type || !fatal_type) return false;
    lowerer->primitive_execute_function = anvil_func_declare(
        lowerer->module, "st_core_primitive_execute", execute_type);
    lowerer->primitive_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_core_primitive_contract_violation",
        fatal_type);
    return lowerer->primitive_execute_function != NULL
        && lowerer->primitive_fatal_function != NULL;
}

static bool build_heap_primitive_abi(lowerer_t *lowerer)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->frame_type);
    anvil_type_t *execute_params[] = {
        frame_ptr, lowerer->u32, lowerer->u64, lowerer->value_ptr,
        lowerer->u64, lowerer->value_ptr
    };
    anvil_type_t *fatal_params[] = {
        lowerer->u32, lowerer->u32, frame_ptr
    };
    anvil_type_t *execute_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, execute_params, 6u, false) : NULL;
    anvil_type_t *fatal_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u64, fatal_params, 3u, false) : NULL;
    if (!execute_type || !fatal_type) return false;
    lowerer->heap_primitive_execute_function = anvil_func_declare(
        lowerer->module, "st_aot_heap_primitive_execute", execute_type);
    lowerer->heap_primitive_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_heap_primitive_contract_violation",
        fatal_type);
    return lowerer->heap_primitive_execute_function != NULL
        && lowerer->heap_primitive_fatal_function != NULL;
}

static bool build_image_runtime_abi(lowerer_t *lowerer)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->frame_type);
    anvil_type_t *load_params[] = {
        frame_ptr, lowerer->u32, lowerer->value_ptr
    };
    anvil_type_t *fatal_params[] = { lowerer->u32, frame_ptr };
    anvil_type_t *load_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, load_params, 3u, false) : NULL;
    anvil_type_t *fatal_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u64, fatal_params, 2u, false) : NULL;
    if (!load_type || !fatal_type) return false;
    lowerer->image_global_load_function = anvil_func_declare(
        lowerer->module, "st_image_runtime_global_load", load_type);
    lowerer->image_literal_load_function = anvil_func_declare(
        lowerer->module, "st_image_runtime_literal_load", load_type);
    lowerer->image_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_image_runtime_contract_violation",
        fatal_type);
    return lowerer->image_global_load_function != NULL
        && lowerer->image_literal_load_function != NULL
        && lowerer->image_fatal_function != NULL;
}

static bool build_runtime_primitive_abi(lowerer_t *lowerer)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->frame_type);
    anvil_type_t *detail_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->u32);
    anvil_type_t *parameters[] = {
        frame_ptr, lowerer->u64, lowerer->value_ptr, lowerer->u64,
        lowerer->value_ptr, detail_ptr
    };
    anvil_type_t *fatal_parameters[] = {
        lowerer->u32, lowerer->u32, frame_ptr
    };
    anvil_type_t *type = frame_ptr && detail_ptr
        ? anvil_type_func(lowerer->ctx, lowerer->u32,
                          parameters, 6u, false)
        : NULL;
    anvil_type_t *fatal_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u64, fatal_parameters, 3u, false) : NULL;
    if (!type || !fatal_type || lowerer->runtime_primitive_symbol == NULL)
        return false;
    lowerer->runtime_primitive_function = anvil_func_declare(
        lowerer->module, lowerer->runtime_primitive_symbol, type);
    lowerer->runtime_primitive_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_runtime_primitive_contract_violation",
        fatal_type);
    return lowerer->runtime_primitive_function != NULL &&
        lowerer->runtime_primitive_fatal_function != NULL;
}

static bool build_control_abi(lowerer_t *lowerer)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(
        lowerer->ctx, lowerer->frame_type);
    anvil_type_t *enter_params[] = {
        frame_ptr, lowerer->byte_ptr, lowerer->u32
    };
    anvil_type_t *leave_params[] = {
        frame_ptr, lowerer->byte_ptr, lowerer->u64, lowerer->value_ptr
    };
    anvil_type_t *pending_params[] = {
        frame_ptr, anvil_type_ptr(lowerer->ctx, lowerer->u32),
        lowerer->value_ptr
    };
    anvil_type_t *nlr_params[] = {
        frame_ptr, lowerer->byte_ptr, lowerer->u64
    };
    anvil_type_t *fatal_params[] = { lowerer->u32, frame_ptr };
    anvil_type_t *enter_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, enter_params, 3u, false) : NULL;
    anvil_type_t *leave_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, leave_params, 4u, false) : NULL;
    anvil_type_t *pending_type = pending_params[1] ? anvil_type_func(
        lowerer->ctx, lowerer->u32, pending_params, 3u, false) : NULL;
    anvil_type_t *nlr_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u32, nlr_params, 3u, false) : NULL;
    anvil_type_t *fatal_type = frame_ptr ? anvil_type_func(
        lowerer->ctx, lowerer->u64, fatal_params, 2u, false) : NULL;
    if (!enter_type || !leave_type || !pending_type || !nlr_type
            || !fatal_type) return false;
    lowerer->control_enter_function = anvil_func_declare(
        lowerer->module, "st_aot_control_scope_enter", enter_type);
    lowerer->control_leave_function = anvil_func_declare(
        lowerer->module, "st_aot_control_scope_leave", leave_type);
    lowerer->control_pending_function = anvil_func_declare(
        lowerer->module, "st_aot_control_pending", pending_type);
    lowerer->control_nlr_function = anvil_func_declare(
        lowerer->module, "st_aot_control_non_local_return", nlr_type);
    lowerer->control_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_control_contract_violation", fatal_type);
    return lowerer->control_enter_function
        && lowerer->control_leave_function
        && lowerer->control_pending_function
        && lowerer->control_nlr_function
        && lowerer->control_fatal_function;
}

static bool build_closure_abi(lowerer_t *lowerer)
{
    anvil_ctx_t *ctx = lowerer->ctx;
    anvil_type_t *frame_ptr = anvil_type_ptr(ctx, lowerer->frame_type);
    anvil_type_t *target = anvil_type_named_struct(
        ctx, "StAotClosureTarget");
    anvil_type_t *target_fields[] = {
        lowerer->method_ptr, lowerer->byte_ptr, lowerer->byte_ptr,
        lowerer->u32, lowerer->u32, lowerer->u32
    };
    if (!frame_ptr || !target
            || !set_named_struct_body(target, target_fields, 6u)
            || anvil_type_size(target) != sizeof(st_aot_closure_target_t)
            || anvil_type_struct_field_offset(target, 0u)
                != offsetof(st_aot_closure_target_t, code)
            || anvil_type_struct_field_offset(target, 1u)
                != offsetof(st_aot_closure_target_t, method)
            || anvil_type_struct_field_offset(target, 2u)
                != offsetof(st_aot_closure_target_t, home)
            || anvil_type_struct_field_offset(target, 3u)
                != offsetof(st_aot_closure_target_t, frame_root_capacity))
        return false;
    anvil_type_t *target_ptr = anvil_type_ptr(ctx, target);
    anvil_type_t *create_params[] = {
        frame_ptr, lowerer->byte_ptr, lowerer->u64, lowerer->value_ptr,
        lowerer->u32, lowerer->value_ptr
    };
    anvil_type_t *resolve_params[] = {
        frame_ptr, lowerer->u64, lowerer->u32, target_ptr
    };
    anvil_type_t *capture_params[] = {
        frame_ptr, lowerer->u64, lowerer->u32, lowerer->value_ptr
    };
    anvil_type_t *cell_create_params[] = {
        frame_ptr, lowerer->u64, lowerer->value_ptr
    };
    anvil_type_t *cell_store_params[] = {
        frame_ptr, lowerer->u64, lowerer->u64
    };
    anvil_type_t *fatal_params[] = { lowerer->u32, frame_ptr };
    anvil_type_t *create_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u32, create_params, 6u, false) : NULL;
    anvil_type_t *resolve_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u32, resolve_params, 4u, false) : NULL;
    anvil_type_t *capture_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u32, capture_params, 4u, false) : NULL;
    anvil_type_t *cell_create_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u32, cell_create_params, 3u, false) : NULL;
    anvil_type_t *cell_store_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u32, cell_store_params, 3u, false) : NULL;
    anvil_type_t *fatal_type = target_ptr ? anvil_type_func(
        ctx, lowerer->u64, fatal_params, 2u, false) : NULL;
    if (!target_ptr || !create_type || !resolve_type || !capture_type
            || !cell_create_type || !cell_store_type
            || !fatal_type) return false;
    lowerer->closure_target_type = target;
    lowerer->closure_target_ptr = target_ptr;
    lowerer->closure_create_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_create", create_type);
    lowerer->closure_resolve_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_resolve", resolve_type);
    lowerer->closure_capture_load_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_capture_load", capture_type);
    lowerer->closure_cell_create_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_cell_create", cell_create_type);
    lowerer->closure_cell_load_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_cell_load", cell_create_type);
    lowerer->closure_cell_store_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_cell_store", cell_store_type);
    lowerer->closure_fatal_function = anvil_func_declare(
        lowerer->module, "st_aot_closure_contract_violation", fatal_type);
    anvil_type_t *byte = anvil_type_i8(ctx);
    if (!byte || !lowerer->closure_descriptor_globals) return false;
    for (size_t index = 0u; index < lowerer->block_count; index++) {
        lowerer->closure_descriptor_globals[index] =
            anvil_module_declare_global(
                lowerer->module,
                lowerer->block_artifacts[index].descriptor_symbol.bytes,
                byte, ANVIL_LINK_EXTERNAL);
        if (!lowerer->closure_descriptor_globals[index]) return false;
    }
    return lowerer->closure_create_function
        && lowerer->closure_resolve_function
        && lowerer->closure_capture_load_function
        && lowerer->closure_cell_create_function
        && lowerer->closure_cell_load_function
        && lowerer->closure_cell_store_function
        && lowerer->closure_fatal_function
        ;
}

static bool setup_control_scope(lowerer_t *lowerer)
{
    anvil_type_t *storage_type = anvil_type_array(
        lowerer->ctx, anvil_type_u8(lowerer->ctx),
        ST_AOT_CONTROL_SCOPE_SIZE);
    anvil_value_t *storage = storage_type ? anvil_build_alloca(
        lowerer->ctx, storage_type, "control.scope.storage") : NULL;
    anvil_value_t *scope = storage ? anvil_build_bitcast(
        lowerer->ctx, storage, lowerer->byte_ptr, "control.scope") : NULL;
    anvil_value_t *return_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "control.return.addr");
    anvil_value_t *leave_result_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "control.leave.result.addr");
    anvil_block_t *epilogue = anvil_block_create(
        lowerer->function, "control.epilogue");
    anvil_value_t *arguments[] = {
        lowerer->frame, scope,
        anvil_const_u32(lowerer->ctx, lowerer->establish_home ? 1u : 0u)
    };
    anvil_value_t *status = NULL;
    if (!storage || !scope || !return_address || !leave_result_address
            || !epilogue || !arguments[2]
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->control_enter_function),
                arguments, 3u, "control.enter.status", &status)
            || !control_status_or_abort(
                lowerer, status, "control.enter.valid"))
        return false;
    /* Every published map is currently all-live.  The epilogue result slot
     * must therefore contain a valid StValue even at earlier send/allocation
     * safepoints, before any language return has assigned it. */
    if (lowerer->required_root_capacity != 0u
            && !root_store(
                lowerer, lowerer->control_return_root,
                anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL)))
        return false;
    lowerer->control_scope_storage = scope;
    lowerer->control_return_address = return_address;
    lowerer->control_leave_result_address = leave_result_address;
    lowerer->control_epilogue = epilogue;
    return true;
}

static bool finish_control_epilogue(lowerer_t *lowerer)
{
    if (!anvil_set_insert_point(lowerer->ctx, lowerer->control_epilogue))
        return false;
    anvil_value_t *normal = anvil_build_load(
        lowerer->ctx, lowerer->u64, lowerer->control_return_address,
        "control.normal.result");
    uint32_t safepoint_id = ++lowerer->next_safepoint_id;
    anvil_value_t *arguments[] = {
        lowerer->frame, lowerer->control_scope_storage, normal,
        lowerer->control_leave_result_address
    };
    anvil_value_t *status = NULL;
    if (!normal
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, safepoint_id))
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->control_leave_function),
                arguments, 4u, "control.leave.status", &status)
            || !control_status_or_abort(
                lowerer, status, "control.leave.valid")
            || !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, 0u)))
        return false;
    anvil_value_t *value = anvil_build_load(
        lowerer->ctx, lowerer->u64,
        lowerer->control_leave_result_address, "control.leave.result");
    return value != NULL && anvil_build_ret(lowerer->ctx, value);
}

static bool validate_block_frame(lowerer_t *lowerer, uint32_t capacity,
                                 const st_ast_node_t *block)
{
    lowerer->frame = anvil_func_get_param(lowerer->function, 0u);
    anvil_value_t *arguments[] = {
        lowerer->frame, anvil_const_u32(lowerer->ctx, capacity)
    };
    anvil_value_t *status = NULL;
    if (!lowerer->frame || !arguments[1]
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->frame_validate_function),
                arguments, 2u, "block.frame.status", &status)
            || !status) return false;
    anvil_value_t *valid = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_AOT_SEND_OK), "block.frame.valid");
    anvil_block_t *valid_block = control_block_create(
        lowerer, "block.frame.valid");
    anvil_block_t *fatal_block = control_block_create(
        lowerer, "block.frame.fatal");
    if (!valid || !valid_block || !fatal_block
            || !anvil_build_br_cond(lowerer->ctx, valid, valid_block,
                                    fatal_block)
            || !anvil_set_insert_point(lowerer->ctx, fatal_block))
        return false;
    /* Frame validation belongs to the activation/control ABI.  Keeping this
     * failure on that ABI also means a code object that merely contains an
     * unreferenced inline-block function does not acquire a closure-runtime
     * dependency. */
    anvil_value_t *control_status = anvil_const_u32(
        lowerer->ctx, ST_CONTROL_ERR_INVALID_FRAME);
    anvil_value_t *fatal_arguments[] = { control_status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    if (!control_status || !anvil_build_call_checked(
            lowerer->ctx,
            anvil_func_get_value(lowerer->control_fatal_function),
            fatal_arguments, 2u, "block.frame.abort", &fatal_value)
            || !fatal_value || !anvil_build_ret(lowerer->ctx, fatal_value)
            || !anvil_set_insert_point(lowerer->ctx, valid_block)) {
        fail_ir(lowerer, block);
        return false;
    }
    lowerer->closure = frame_field(
        lowerer, ST_FRAME_RECEIVER_FIELD, lowerer->u64, "block.closure");
    lowerer->self = anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL);
    if (!lowerer->closure || !lowerer->self) return false;
    if (capacity != 0u) {
        lowerer->roots = frame_field(
            lowerer, ST_FRAME_ROOTS_FIELD, lowerer->value_ptr,
            "block.roots");
        if (!lowerer->roots || !root_store(lowerer, 0u, lowerer->closure))
            return false;
        anvil_value_t *argv = frame_field(
            lowerer, ST_FRAME_ARGV_FIELD, lowerer->value_ptr, "block.argv");
        if (lowerer->block_artifact->arity != 0u && !argv) return false;
        for (uint32_t index = 0u;
             index < lowerer->block_artifact->arity; index++) {
            anvil_value_t *element = anvil_const_u32(lowerer->ctx, index);
            anvil_value_t *indices[] = { element };
            anvil_value_t *address = element ? anvil_build_gep(
                lowerer->ctx, lowerer->u64, argv, indices, 1u,
                "block.argument.addr") : NULL;
            anvil_value_t *value = address ? anvil_build_load(
                lowerer->ctx, lowerer->u64, address,
                "block.argument.root") : NULL;
            if (!value || !root_store(lowerer, index + 1u, value))
                return false;
        }
    }
    return true;
}

static bool build_escaping_block_function(lowerer_t *factory,
                                          anvil_type_t *method_type)
{
    lowerer_t block = *factory;
    block.function = anvil_func_create(
        factory->module, factory->block_artifact->code_symbol.bytes,
        method_type, ANVIL_LINK_EXTERNAL);
    block.required_root_capacity = factory->block_artifact
        ->required_root_capacity;
    block.base_root_count = factory->base_root_count;
    block.scratch_root_offset = factory->scratch_root_offset;
    block.control_return_root = factory->control_return_root;
    block.control_scope = (factory->block_artifact->method_flags
                           & ST_METHOD_CAN_UNWIND) != 0u;
    block.establish_home = false;
    block.next_control_block = 0u;
    block.control_scope_storage = NULL;
    block.control_return_address = NULL;
    block.control_leave_result_address = NULL;
    block.control_epilogue = NULL;
    block.roots = NULL;
    block.closure = NULL;
    if (!block.function || !anvil_set_insert_point(
            block.ctx, anvil_func_get_entry(block.function))
            || !validate_block_frame(
                &block, block.required_root_capacity,
                factory->current_block)
            || !initialize_bindings(&block))
        return false;
    if (block.control_scope && !setup_control_scope(&block)) {
        return false;
    }
    lowered_value_t value = normal_value(anvil_const_u64(
        block.ctx, ST_VALUE_TAG_SPECIAL));
    bool terminated = false;
    for (size_t index = 0u;
         index < factory->current_block->as.block.expressions.count;
         index++) {
        value = lower_expression(
            &block, factory->current_block->as.block.expressions.items[index]);
        if (!value.value && !value.terminated) {
            return false;
        }
        if (value.terminated) {
            terminated = true;
            break;
        }
    }
    if (!terminated && (!value.value
            || !(block.control_scope
                ? emit_control_return(&block, value.value)
                : anvil_build_ret(block.ctx, value.value))))
        return false;
    if (block.control_scope && !finish_control_epilogue(&block)) {
        return false;
    }
    factory->next_safepoint_id = block.next_safepoint_id;
    factory->next_send_site = block.next_send_site;
    factory->next_string_literal = block.next_string_literal;
    factory->next_string_byte = block.next_string_byte;
    factory->next_image_load = block.next_image_load;
    factory->next_heap_access = block.next_heap_access;
    factory->block_artifact->function = block.function;
    return true;
}

static bool lower_primitive_prologue(lowerer_t *lowerer)
{
    const st_primitive_t *primitive = lowerer->primitive_binding->primitive;
    size_t arity = lowerer->graph_method->node->as.method.arguments.count;
    anvil_value_t *result_address = anvil_build_alloca(
        lowerer->ctx, lowerer->u64, "primitive.result.addr");
    anvil_value_t *detail_address = lowerer->runtime_symbol_primitive
        ? anvil_build_alloca(lowerer->ctx, lowerer->u32,
                             "primitive.detail.addr")
        : NULL;
    anvil_value_t *arguments = arity == 0u
        ? anvil_const_null(lowerer->ctx, lowerer->value_ptr)
        : frame_field(lowerer, ST_FRAME_ARGV_FIELD, lowerer->value_ptr,
                      "primitive.argv");
    anvil_value_t *intrinsic = anvil_const_u32(
        lowerer->ctx, primitive->intrinsic_id);
    anvil_value_t *arity_value = anvil_const_u64(lowerer->ctx, arity);
    anvil_value_t *core_arguments[] = {
        intrinsic, lowerer->self, arguments, arity_value, result_address
    };
    anvil_value_t *heap_arguments[] = {
        lowerer->frame, intrinsic, lowerer->self, arguments, arity_value,
        result_address
    };
    anvil_value_t *runtime_arguments[] = {
        lowerer->frame, lowerer->self, arguments, arity_value,
        result_address, detail_address
    };
    anvil_value_t *status = NULL;
    anvil_value_t *invalid_result = anvil_const_u64(lowerer->ctx, 0u);
    anvil_value_t *zero_detail = lowerer->runtime_symbol_primitive
        ? anvil_const_u32(lowerer->ctx, 0u) : NULL;
    if (!result_address || !arguments || !intrinsic || !arity_value
            || !invalid_result
            || !anvil_build_store(lowerer->ctx, invalid_result,
                                  result_address)
            || (lowerer->runtime_symbol_primitive
                && (!detail_address || !zero_detail
                    || !anvil_build_store(lowerer->ctx, zero_detail,
                                          detail_address))))
        return false;
    if (lowerer->heap_primitive || lowerer->runtime_symbol_primitive) {
        uint32_t safepoint_id = ++lowerer->next_safepoint_id;
        if (!store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, safepoint_id)))
            return false;
    }
    anvil_value_t *callee = anvil_func_get_value(
        lowerer->runtime_symbol_primitive
            ? lowerer->runtime_primitive_function
            : lowerer->heap_primitive
                ? lowerer->heap_primitive_execute_function
                : lowerer->primitive_execute_function);
    anvil_value_t **call_arguments = lowerer->runtime_symbol_primitive
        ? runtime_arguments
        : lowerer->heap_primitive ? heap_arguments : core_arguments;
    size_t call_argument_count = lowerer->runtime_symbol_primitive
        || lowerer->heap_primitive ? 6u : 5u;
    if (!callee || !anvil_build_call_checked(
                lowerer->ctx,
                callee, call_arguments, call_argument_count,
                "primitive.status", &status)
            || !status)
        return false;
    if ((lowerer->heap_primitive || lowerer->runtime_symbol_primitive)
            && !store_frame_field(
                lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                anvil_const_u32(lowerer->ctx, 0u)))
        return false;
    if (!check_control_pending(lowerer, lowerer->graph_method->node))
        return false;
    anvil_value_t *succeeded = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_CORE_PRIMITIVE_OK),
        "primitive.succeeded");
    anvil_block_t *success = anvil_block_create(
        lowerer->function, "primitive.success");
    anvil_block_t *failure = anvil_block_create(
        lowerer->function, primitive->failure_policy == ST_PRIMITIVE_FALL_THROUGH
            ? "primitive.fallthrough" : "primitive.contract.failure");
    if (!succeeded || !success || !failure
            || !anvil_build_br_cond(lowerer->ctx, succeeded, success, failure)
            || !anvil_set_insert_point(lowerer->ctx, success))
        return false;
    anvil_value_t *value = anvil_build_load(
        lowerer->ctx, lowerer->u64, result_address, "primitive.result");
    if (!value
            || !(lowerer->control_scope
                ? emit_control_return(lowerer, value)
                : anvil_build_ret(lowerer->ctx, value))
            || !anvil_set_insert_point(lowerer->ctx, failure))
        return false;
    if (primitive->failure_policy == ST_PRIMITIVE_FALL_THROUGH) return true;
    if (lowerer->runtime_symbol_primitive) {
        anvil_value_t *detail = anvil_build_load(
            lowerer->ctx, lowerer->u32, detail_address,
            "primitive.failure.detail");
        anvil_value_t *runtime_fatal_arguments[] = {
            status, detail, lowerer->frame
        };
        anvil_value_t *runtime_fatal_value = NULL;
        return detail != NULL && anvil_build_call_checked(
                   lowerer->ctx,
                   anvil_func_get_value(
                       lowerer->runtime_primitive_fatal_function),
                   runtime_fatal_arguments, 3u,
                   "primitive.contract.abort", &runtime_fatal_value)
            && runtime_fatal_value != NULL
            && anvil_build_ret(lowerer->ctx, runtime_fatal_value);
    }
    anvil_value_t *fatal_arguments[] = { intrinsic, status, lowerer->frame };
    anvil_value_t *fatal_value = NULL;
    return anvil_build_call_checked(
               lowerer->ctx,
               anvil_func_get_value(lowerer->heap_primitive
                   ? lowerer->heap_primitive_fatal_function
                   : lowerer->primitive_fatal_function),
               fatal_arguments, 3u, "primitive.contract.abort", &fatal_value)
        && fatal_value != NULL && anvil_build_ret(lowerer->ctx, fatal_value);
}

static anvil_value_t *create_send_site(lowerer_t *lowerer,
                                       st_selector_id_t selector_id,
                                       uint32_t lexical_owner_class_id)
{
    anvil_ctx_t *ctx = lowerer->ctx;
    anvil_type_t *slot_array = anvil_type_struct_field_type(
        lowerer->send_site_type, 3u);
    anvil_type_t *slot_type = anvil_type_named_struct(ctx, "StPicSlot");
    anvil_value_t *null_pointer = anvil_const_null(ctx, lowerer->byte_ptr);
    anvil_value_t *zero64 = anvil_const_u64(ctx, 0u);
    anvil_value_t *zero32 = anvil_const_u32(ctx, 0u);
    anvil_value_t *slot_fields[] = {
        zero64, zero32, zero32, null_pointer, null_pointer
    };
    anvil_value_t *slot = slot_type
        ? anvil_const_struct(ctx, slot_type, slot_fields, 5u) : NULL;
    anvil_value_t *slots[ST_PIC_WAYS] = { slot, slot, slot, slot };
    anvil_value_t *slot_values = slot && slot_array
        ? anvil_const_array(ctx, slot_type, slots, ST_PIC_WAYS) : NULL;
    anvil_value_t *site_fields[] = {
        anvil_const_u32(ctx, selector_id),
        anvil_const_u32(ctx, lexical_owner_class_id),
        zero32,
        slot_values,
        anvil_const_u8(ctx, 1u)
    };
    anvil_value_t *initializer = slot_values && site_fields[0]
            && site_fields[1] && site_fields[4]
        ? anvil_const_struct(ctx, lowerer->send_site_type, site_fields, 5u)
        : NULL;
    char name[64];
    int length = snprintf(name, sizeof(name), "st_send_site_%zu",
                          lowerer->next_send_site++);
    if (!initializer || length <= 0 || (size_t)length >= sizeof(name))
        return NULL;
    anvil_value_t *global = anvil_module_add_global(
        lowerer->module, name, lowerer->send_site_type, ANVIL_LINK_INTERNAL);
    return global && anvil_global_set_initializer(global, initializer)
        ? global : NULL;
}

static bool validate_entry_frame(lowerer_t *lowerer)
{
    lowerer->frame = anvil_func_get_param(lowerer->function, 0u);
    if (lowerer->frame == NULL) return false;
    if (lowerer->required_root_capacity == 0u) {
        lowerer->self = frame_field(lowerer, ST_FRAME_RECEIVER_FIELD,
                                    lowerer->u64, "self");
        return lowerer->self != NULL;
    }
    anvil_value_t *arguments[] = {
        lowerer->frame,
        anvil_const_u32(lowerer->ctx, lowerer->required_root_capacity)
    };
    anvil_value_t *status = NULL;
    if (!arguments[1]
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->frame_validate_function),
                arguments, 2u, "frame.validation", &status)
            || !status) return false;
    anvil_value_t *valid = anvil_build_cmp_eq(
        lowerer->ctx, status,
        anvil_const_u32(lowerer->ctx, ST_AOT_SEND_OK), "frame.valid");
    anvil_block_t *valid_block = anvil_block_create(
        lowerer->function, "frame.valid");
    anvil_block_t *invalid_block = anvil_block_create(
        lowerer->function, "frame.invalid");
    if (!valid || !valid_block || !invalid_block
            || !anvil_build_br_cond(lowerer->ctx, valid, valid_block,
                                    invalid_block)
            || !anvil_set_insert_point(lowerer->ctx, invalid_block))
        return false;
    anvil_value_t *failure_arguments[] = {
        lowerer->frame,
        anvil_const_null(lowerer->ctx, lowerer->send_site_ptr),
        anvil_const_u64(lowerer->ctx, ST_VALUE_TAG_SPECIAL),
        anvil_const_null(lowerer->ctx, lowerer->value_ptr),
        anvil_const_u32(lowerer->ctx, 0u),
        status
    };
    anvil_value_t *failure_value = NULL;
    if (!failure_arguments[1] || !failure_arguments[3]
            || !failure_arguments[4]
            || !anvil_build_call_checked(
                lowerer->ctx,
                anvil_func_get_value(lowerer->send_failure_function),
                failure_arguments, 6u, "frame.failure", &failure_value)
            || !failure_value || !anvil_build_ret(lowerer->ctx, failure_value)
            || !anvil_set_insert_point(lowerer->ctx, valid_block))
        return false;
    lowerer->self = frame_field(lowerer, ST_FRAME_RECEIVER_FIELD,
                                lowerer->u64, "self");
    lowerer->roots = frame_field(lowerer, ST_FRAME_ROOTS_FIELD,
                                 lowerer->value_ptr, "roots");
    return lowerer->self != NULL && lowerer->roots != NULL;
}

static bool initialize_bindings(lowerer_t *lowerer)
{
    anvil_value_t *argv = NULL;
    anvil_value_t *nil = anvil_const_u64(
        lowerer->ctx, ST_VALUE_TAG_SPECIAL);
    st_sema_scope_id_t active_scope = lowerer->current_block_info
        ? lowerer->current_block_info->scope
        : (lowerer->sema->scope_count != 0u ? 0u : ST_SEMA_INVALID_ID);
    if (!nil) return false;
    memset(lowerer->locations, 0,
           lowerer->locations_count * sizeof(*lowerer->locations));
    for (size_t index = 0u; index < lowerer->locations_count; index++)
        lowerer->locations[index].root_slot = UINT32_MAX;
    if (lowerer->required_root_capacity != 0u) {
        for (uint32_t slot = 0u; slot < lowerer->required_root_capacity;
             slot++)
            if (!root_store(lowerer, slot, nil)) return false;
        if (!root_store(lowerer, 0u,
                lowerer->closure ? lowerer->closure : lowerer->self))
            return false;
    }

    /* Authenticate and publish the closure's captured lexical environment. */
    if (lowerer->current_block_info) {
        const st_sema_block_t *info = lowerer->current_block_info;
        for (size_t capture_index = 0u;
             capture_index < info->capture_count; capture_index++) {
            const st_sema_capture_t *capture = &lowerer->sema->captures[
                info->capture_offset + capture_index];
            anvil_value_t *result_address = anvil_build_alloca(
                lowerer->ctx, lowerer->u64, "block.capture.result");
            anvil_value_t *arguments[] = {
                lowerer->frame, lowerer->closure,
                anvil_const_u32(lowerer->ctx, (uint32_t)capture_index),
                result_address
            };
            anvil_value_t *status = NULL;
            if (!result_address || !arguments[2]
                    || !anvil_build_call_checked(
                        lowerer->ctx,
                        anvil_func_get_value(
                            lowerer->closure_capture_load_function),
                        arguments, 4u, "block.capture.status", &status)
                    || !status || !closure_status_or_abort(
                        lowerer, status, info->node, "block.capture.ok"))
                return false;
            anvil_value_t *value = anvil_build_load(
                lowerer->ctx, lowerer->u64, result_address,
                "block.capture.value");
            if (!value || capture->binding >= lowerer->locations_count)
                return false;
            binding_location_t *location =
                &lowerer->locations[capture->binding];
            location->value = value;
            location->cell = capture->mode == ST_SEMA_CAPTURE_CELL;
            location->root_slot = (uint32_t)capture->binding + 1u;
            if (lowerer->required_root_capacity != 0u
                    && !root_store(lowerer, location->root_slot, value))
                return false;
            if (capture->mode == ST_SEMA_CAPTURE_SELF)
                lowerer->self = value;
        }
    }

    for (size_t index = 0u; index < lowerer->sema->binding_count; index++) {
        const st_sema_binding_t *binding = &lowerer->sema->bindings[index];
        if (binding->scope != active_scope) continue;
        anvil_value_t *value = NULL;
        bool argument = binding->kind == ST_SEMA_BIND_METHOD_ARGUMENT
            || binding->kind == ST_SEMA_BIND_BLOCK_ARGUMENT;
        if (argument) {
            if (argv == NULL) {
                argv = frame_field(lowerer, ST_FRAME_ARGV_FIELD,
                                   lowerer->value_ptr, "argv");
                if (!argv) return false;
            }
            anvil_value_t *slot = anvil_const_u64(lowerer->ctx, binding->slot);
            anvil_value_t *indices[] = { slot };
            anvil_value_t *address = slot ? anvil_build_gep(
                lowerer->ctx, lowerer->u64, argv, indices, 1u,
                "argument.addr") : NULL;
            value = address ? anvil_build_load(
                lowerer->ctx, lowerer->u64, address, "argument") : NULL;
        } else if (binding->kind == ST_SEMA_BIND_TEMPORARY) {
            value = nil;
        } else {
            continue;
        }
        if (!value) return false;
        uint32_t root_slot = (uint32_t)index + 1u;
        anvil_value_t *address = lowerer->required_root_capacity != 0u
            ? root_address(lowerer, root_slot, "binding.root.addr")
            : anvil_build_alloca(lowerer->ctx, lowerer->u64,
                                 "binding.addr");
        if (!address || !anvil_build_store(lowerer->ctx, value, address))
            return false;
        lowerer->locations[index] = (binding_location_t) {
            address, true, false, root_slot
        };
        if ((binding->flags & ST_SEMA_BINDING_NEEDS_CELL) != 0u) {
            uint32_t base = lowerer->scratch_root_offset;
            anvil_value_t *cell_out = root_address(
                lowerer, base + 1u, "cell.create.result");
            anvil_value_t *cell_arguments[] = {
                lowerer->frame, value, cell_out
            };
            anvil_value_t *status = NULL;
            uint32_t safepoint = ++lowerer->next_safepoint_id;
            if (!root_store(lowerer, base, value) || !cell_out
                    || !store_frame_field(
                        lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                        anvil_const_u32(lowerer->ctx, safepoint))
                    || !anvil_build_call_checked(
                        lowerer->ctx,
                        anvil_func_get_value(
                            lowerer->closure_cell_create_function),
                        cell_arguments, 3u, "cell.create.status", &status)
                    || !status || !store_frame_field(
                        lowerer, lowerer->frame, ST_FRAME_SAFEPOINT_FIELD,
                        anvil_const_u32(lowerer->ctx, 0u))
                    || !closure_status_or_abort(
                        lowerer, status, binding->declaration,
                        "cell.create.ok"))
                return false;
            anvil_value_t *cell = anvil_build_load(
                lowerer->ctx, lowerer->u64, cell_out, "cell");
            if (!cell || !anvil_build_store(lowerer->ctx, cell, address)
                    || !clear_scratch_roots(lowerer, 0u, 2u))
                return false;
            lowerer->locations[index].value = cell;
            lowerer->locations[index].address = false;
            lowerer->locations[index].cell = true;
        }
    }
    return true;
}

void st_lower_result_init(st_lower_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = ST_LOWER_OK;
    }
}

void st_lower_result_destroy(st_lower_result_t *result)
{
    lower_result_impl_t *implementation;
    if (result == NULL) return;
    if (result->module != NULL) anvil_module_destroy(result->module);
    implementation = result->implementation;
    if (implementation != NULL) {
        for (size_t index = 0u; index < implementation->block_count; index++) {
            if (implementation->block_symbols
                    && implementation->block_symbols[index])
                implementation->allocator.deallocate(
                    implementation->allocator.user,
                    implementation->block_symbols[index]);
            if (implementation->block_captures
                    && implementation->block_captures[index])
                implementation->allocator.deallocate(
                    implementation->allocator.user,
                    implementation->block_captures[index]);
        }
        if (implementation->block_symbols)
            implementation->allocator.deallocate(
                implementation->allocator.user,
                implementation->block_symbols);
        if (implementation->block_captures)
            implementation->allocator.deallocate(
                implementation->allocator.user,
                implementation->block_captures);
        if (implementation->blocks)
            implementation->allocator.deallocate(
                implementation->allocator.user, implementation->blocks);
        if (implementation->bitmap != NULL)
            implementation->allocator.deallocate(
                implementation->allocator.user, implementation->bitmap);
        if (implementation->root_maps != NULL)
            implementation->allocator.deallocate(
                implementation->allocator.user, implementation->root_maps);
        if (implementation->string_literal_bytes != NULL)
            implementation->allocator.deallocate(
                implementation->allocator.user,
                implementation->string_literal_bytes);
        if (implementation->string_literals != NULL)
            implementation->allocator.deallocate(
                implementation->allocator.user,
                implementation->string_literals);
        implementation->allocator.deallocate(
            implementation->allocator.user, implementation);
    }
    st_lower_result_init(result);
}

st_lower_status_t st_lower_method(
    st_lower_result_t *result, anvil_ctx_t *ctx,
    const st_class_graph_result_t *graph,
    st_class_graph_method_id_t method_id,
    const st_sema_result_t *sema,
    const st_lower_options_t *options)
{
    st_lower_allocator_t allocator = { default_allocate, default_deallocate,
                                       NULL };
    binding_location_t *locations = NULL;
    lower_result_impl_t *implementation = NULL;
    anvil_module_t *module = NULL;
    const st_class_graph_method_t *graph_method;
    lowerer_t lowerer;
    anvil_type_t *method_type = NULL;
    char verify_error[256] = {0};
    char *runtime_primitive_symbol = NULL;
    anvil_value_t **closure_descriptor_globals = NULL;
    size_t artifact_block_count = 0u;
    size_t control_block_count = 0u;

    if (result == NULL) return ST_LOWER_ERR_INVALID_ARGUMENT;
    if (result->status != ST_LOWER_OK || result->module != NULL
            || result->function != NULL) {
        result->status = ST_LOWER_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    if (ctx == NULL || graph == NULL || !st_class_graph_succeeded(graph)
            || sema == NULL || !st_sema_succeeded(sema) || options == NULL
            || !portable_symbol_is_valid(options->symbol_name)
            || (unsigned)options->linkage > (unsigned)ANVIL_LINK_WEAK) {
        set_diagnostic(result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, NULL,
                       static_string("invalid lowering input"));
        return result->status;
    }
    graph_method = st_class_graph_method(graph, method_id);
    if (graph_method == NULL || graph_method->node == NULL) {
        set_diagnostic(result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, NULL,
                       static_string("method ID is not present in class graph"));
        return result->status;
    }
    if ((options->allocator.allocate == NULL)
            != (options->allocator.deallocate == NULL)) {
        set_diagnostic(result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("allocator callbacks must be paired"));
        return result->status;
    }
    if (!global_bindings_are_valid(options->globals,
                                   options->global_count)) {
        set_diagnostic(result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("global binding table is not canonical"));
        return result->status;
    }
    if (!runtime_class_map_is_valid(graph, options)) {
        set_diagnostic(result, ST_LOWER_ERR_INVALID_ARGUMENT,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("runtime class ID map is not canonical"));
        return result->status;
    }
    if (options->allocator.allocate != NULL) allocator = options->allocator;

    const anvil_data_layout_t *layout = anvil_ctx_get_data_layout(ctx);
    if (layout == NULL || layout->pointer.size != sizeof(uint64_t)) {
        set_diagnostic(result, ST_LOWER_ERR_UNSUPPORTED,
                       ST_LOWER_DIAG_UNSUPPORTED_TARGET, graph_method->node,
                       static_string("Smalltalk AOT ABI requires a 64-bit target"));
        return result->status;
    }

    preflight_t flight = {
        .result = result,
        .sema = sema,
        .method = graph_method->node,
        .graph_method = graph_method,
        .selectors = options->selectors,
        .primitive_binding = options->primitive_binding,
        .globals = options->globals,
        .global_count = options->global_count
    };
    if (!preflight_method(&flight)) return result->status;
    for (size_t index = 0u; index < sema->block_count; index++)
        if (sema_block_needs_artifact(sema, graph_method->node, index)) {
            artifact_block_count++;
            if (sema_block_needs_home(sema, index)
                    || activation_may_call(sema->blocks[index].node))
                control_block_count++;
        }
    if (flight.string_literal_count != 0u
            && options->literal_base_index
                > UINT32_MAX
                    - (uint32_t)(flight.string_literal_count - 1u)) {
        set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("String literal ID range exceeds image ABI"));
        return result->status;
    }
    size_t cell_binding_count = 0u;
    for (size_t index = 0u; index < sema->binding_count; index++)
        if ((sema->bindings[index].flags & ST_SEMA_BINDING_NEEDS_CELL) != 0u)
            cell_binding_count++;
    if (cell_binding_count > UINT32_MAX - flight.safepoint_count) {
        set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("cell safepoint plan exceeds ABI"));
        return result->status;
    }
    flight.safepoint_count += cell_binding_count;
    if (cell_binding_count != 0u && flight.maximum_scratch_roots < 2u)
        flight.maximum_scratch_roots = 2u;
    bool control_scope = sema->requires_context || flight.send_count != 0u
        || flight.closure_call_count != 0u
        || flight.runtime_control_primitive || artifact_block_count != 0u;
    if (control_scope) {
        if (control_block_count
                > UINT32_MAX - flight.safepoint_count - 1u) {
            set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                           static_string("control safepoint ID space exhausted"));
            return result->status;
        }
        flight.safepoint_count += 1u + control_block_count;
    }

    size_t method_arity = graph_method->node->as.method.arguments.count;
    if (method_arity > UINT32_MAX
            || sema->binding_count > UINT32_MAX - 1u
            || flight.maximum_scratch_roots > UINT32_MAX
            || (flight.send_count != 0u
                && flight.maximum_scratch_roots == 0u)) {
        set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("method frame-root plan exceeds ABI"));
        return result->status;
    }
    uint32_t base_root_count = 1u + (uint32_t)sema->binding_count;
    uint32_t required_root_capacity = 0u;
    if (flight.safepoint_count != 0u) {
        uint32_t activation_scratch_roots =
            (uint32_t)flight.maximum_scratch_roots;
        /* The control epilogue runs only after the call/create scratch range
         * is dead, so its return value may reuse a scratch root. */
        if (control_scope && !flight.has_cascade
                && options->primitive_binding == NULL) {
            if (activation_scratch_roots == UINT32_MAX) {
                set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                               ST_LOWER_DIAG_INVALID_INPUT,
                               graph_method->node,
                               static_string("control root plan exceeds ABI"));
                return result->status;
            }
            activation_scratch_roots++;
        }
        if (base_root_count > UINT32_MAX - activation_scratch_roots) {
            set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                           static_string("method frame-root plan exceeds ABI"));
            return result->status;
        }
        required_root_capacity = base_root_count + activation_scratch_roots;
    }
    uint32_t control_return_offset =
        (uint32_t)flight.maximum_scratch_roots;
    if ((flight.has_cascade || options->primitive_binding != NULL)
            && control_return_offset != 0u)
        control_return_offset--;

    if (sema->binding_count > SIZE_MAX / sizeof(*locations)) {
        set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                       ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                       static_string("binding-location table overflows size_t"));
        return result->status;
    }
    if (flight.runtime_symbol_primitive) {
        size_t length = options->primitive_binding->primitive
            ->runtime_symbol.length;
        if (length == SIZE_MAX) {
            set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                           static_string("runtime primitive symbol exceeds size_t"));
            return result->status;
        }
        runtime_primitive_symbol = allocator.allocate(
            allocator.user, length + 1u);
        if (runtime_primitive_symbol == NULL) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot copy runtime primitive symbol"));
            return result->status;
        }
        memcpy(runtime_primitive_symbol,
               options->primitive_binding->primitive->runtime_symbol.data,
               length);
        runtime_primitive_symbol[length] = '\0';
    }
    size_t location_count = sema->binding_count ? sema->binding_count : 1u;
    locations = allocator.allocate(allocator.user,
                                   location_count * sizeof(*locations));
    if (locations == NULL) {
        set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                       ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                       static_string("cannot allocate lowering plan"));
        goto done;
    }
    memset(locations, 0, location_count * sizeof(*locations));
    for (size_t index = 0u; index < location_count; index++)
        locations[index].root_slot = UINT32_MAX;

    if (flight.safepoint_count != 0u
            || flight.string_literal_count != 0u) {
        implementation = allocator.allocate(
            allocator.user, sizeof(*implementation));
        if (implementation != NULL) {
            memset(implementation, 0, sizeof(*implementation));
            implementation->allocator = allocator;
        }
        if (implementation == NULL) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate lowering artifact owner"));
            goto done;
        }
    }
    if (artifact_block_count != 0u) {
        if (implementation == NULL) {
            implementation = allocator.allocate(
                allocator.user, sizeof(*implementation));
            if (implementation != NULL) {
                memset(implementation, 0, sizeof(*implementation));
                implementation->allocator = allocator;
            }
        }
        if (implementation == NULL
                || artifact_block_count > SIZE_MAX / sizeof(*implementation->blocks)
                || artifact_block_count > SIZE_MAX / sizeof(*implementation->block_captures)
                || artifact_block_count > SIZE_MAX / sizeof(*implementation->block_symbols)) {
            set_diagnostic(result, implementation ? ST_LOWER_ERR_OVERFLOW
                                                   : ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate block artifact arrays"));
            goto done;
        }
        implementation->blocks = allocator.allocate(
            allocator.user, artifact_block_count * sizeof(*implementation->blocks));
        implementation->block_captures = allocator.allocate(
            allocator.user, artifact_block_count
                * sizeof(*implementation->block_captures));
        implementation->block_symbols = allocator.allocate(
            allocator.user, artifact_block_count
                * sizeof(*implementation->block_symbols));
        if (!implementation->blocks || !implementation->block_captures
                || !implementation->block_symbols) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate block artifact arrays"));
            goto done;
        }
        memset(implementation->blocks, 0,
               artifact_block_count * sizeof(*implementation->blocks));
        memset(implementation->block_captures, 0,
               artifact_block_count * sizeof(*implementation->block_captures));
        memset(implementation->block_symbols, 0,
               artifact_block_count * sizeof(*implementation->block_symbols));
        implementation->block_count = artifact_block_count;
    }
    if (flight.safepoint_count != 0u) {
        size_t bitmap_words = ((size_t)required_root_capacity + 63u) / 64u;
        if (flight.safepoint_count > SIZE_MAX / sizeof(st_lower_root_map_t)
                || bitmap_words > SIZE_MAX / sizeof(uint64_t)) {
            set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                           static_string("root metadata exceeds size_t"));
            goto done;
        }
        implementation->root_maps = allocator.allocate(
            allocator.user,
            flight.safepoint_count * sizeof(*implementation->root_maps));
        if (implementation != NULL && implementation->root_maps != NULL) {
            implementation->bitmap = allocator.allocate(
                allocator.user, bitmap_words * sizeof(*implementation->bitmap));
        }
        if (implementation == NULL || implementation->root_maps == NULL
                || implementation->bitmap == NULL) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate root-map metadata"));
            goto done;
        }
        for (size_t word = 0u; word < bitmap_words; word++)
            implementation->bitmap[word] = UINT64_MAX;
        unsigned remainder = required_root_capacity & 63u;
        if (remainder != 0u)
            implementation->bitmap[bitmap_words - 1u]
                = UINT64_MAX >> (64u - remainder);
        for (size_t index = 0u; index < flight.safepoint_count; index++) {
            implementation->root_maps[index] = (st_lower_root_map_t) {
                .safepoint_id = (uint32_t)index + 1u,
                .root_count = required_root_capacity,
                .bitmap_word_count = bitmap_words,
                .live_root_bitmap = implementation->bitmap
            };
        }
        if (artifact_block_count != 0u) {
            unsigned char *needs_home = allocator.allocate(
                allocator.user, sema->block_count);
            if (!needs_home) {
                set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                               ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                               static_string("cannot allocate block home plan"));
                goto done;
            }
            for (size_t index = 0u; index < sema->block_count; index++)
                needs_home[index] = sema->blocks[index].has_nonlocal_return;
            for (size_t index = sema->block_count; index-- != 0u; ) {
                st_sema_block_id_t parent = sema->blocks[index].parent;
                if (needs_home[index] && parent != ST_SEMA_INVALID_ID
                        && parent < sema->block_count)
                    needs_home[parent] = 1u;
            }
            bool artifacts_ok = true;
            size_t artifact_index = 0u;
            for (size_t index = 0u; index < sema->block_count; index++) {
                if (!sema_block_needs_artifact(
                        sema, graph_method->node, index))
                    continue;
                artifacts_ok = artifacts_ok && prepare_block_artifact(
                    implementation, allocator, options->symbol_name,
                    graph_method->id, sema, &sema->blocks[index],
                    artifact_index, (uint32_t)index,
                    required_root_capacity, implementation->root_maps,
                    flight.safepoint_count, needs_home[index] != 0u,
                    needs_home[index] != 0u
                        || activation_may_call(sema->blocks[index].node));
                artifact_index++;
            }
            allocator.deallocate(allocator.user, needs_home);
            if (!artifacts_ok) {
                set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                               ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                               static_string("cannot allocate block artifact metadata"));
                goto done;
            }
        }
    }
    if (flight.string_literal_count != 0u) {
        if (flight.string_literal_count
                > SIZE_MAX / sizeof(*implementation->string_literals)) {
            set_diagnostic(result, ST_LOWER_ERR_OVERFLOW,
                           ST_LOWER_DIAG_INVALID_INPUT, graph_method->node,
                           static_string("String literal artifacts exceed size_t"));
            goto done;
        }
        implementation->string_literals = allocator.allocate(
            allocator.user, flight.string_literal_count
                * sizeof(*implementation->string_literals));
        if (implementation->string_literals != NULL)
            memset(implementation->string_literals, 0,
                   flight.string_literal_count
                       * sizeof(*implementation->string_literals));
        if (flight.string_literal_bytes != 0u)
            implementation->string_literal_bytes = allocator.allocate(
                allocator.user, flight.string_literal_bytes);
        if (implementation->string_literals == NULL
                || (flight.string_literal_bytes != 0u
                    && implementation->string_literal_bytes == NULL)) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate String literal artifacts"));
            goto done;
        }
    }

    memset(&lowerer, 0, sizeof(lowerer));
    lowerer.result = result;
    lowerer.ctx = ctx;
    lowerer.sema = sema;
    lowerer.graph_method = graph_method;
    lowerer.selectors = options->selectors;
    lowerer.primitive_binding = options->primitive_binding;
    lowerer.runtime_primitive_symbol = runtime_primitive_symbol;
    lowerer.globals = options->globals;
    lowerer.global_count = options->global_count;
    lowerer.runtime_class_ids_by_entity =
        options->runtime_class_ids_by_entity;
    lowerer.runtime_class_id_count = options->runtime_class_id_count;
    lowerer.locations = locations;
    lowerer.locations_count = sema->binding_count;
    lowerer.required_root_capacity = required_root_capacity;
    lowerer.base_root_count = base_root_count;
    lowerer.scratch_root_offset = base_root_count;
    lowerer.heap_primitive = flight.heap_primitive;
    lowerer.runtime_symbol_primitive = flight.runtime_symbol_primitive;
    lowerer.control_scope = control_scope;
    lowerer.establish_home = sema->requires_context
        || sema->may_be_nonlocal_return_home;
    lowerer.control_return_root = base_root_count + control_return_offset;
    lowerer.block_artifacts = implementation ? implementation->blocks : NULL;
    lowerer.block_count = artifact_block_count;
    if (artifact_block_count != 0u) {
        closure_descriptor_globals = allocator.allocate(
            allocator.user, artifact_block_count
                * sizeof(*closure_descriptor_globals));
        if (!closure_descriptor_globals) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot allocate descriptor extern table"));
            goto done;
        }
        memset(closure_descriptor_globals, 0,
               artifact_block_count * sizeof(*closure_descriptor_globals));
    }
    lowerer.closure_descriptor_globals = closure_descriptor_globals;
    lowerer.string_literals = implementation
        ? implementation->string_literals : NULL;
    lowerer.string_literal_bytes = implementation
        ? implementation->string_literal_bytes : NULL;
    lowerer.literal_base_index = options->literal_base_index;
    lowerer.string_literal_capacity = flight.string_literal_count;
    lowerer.string_literal_byte_capacity = flight.string_literal_bytes;
    anvil_ctx_clear_error(ctx);
    if (!build_types(&lowerer, &method_type)) {
        if (anvil_ctx_get_last_error(ctx) == ANVIL_ERR_NOMEM) {
            set_diagnostic(result, ST_LOWER_ERR_OUT_OF_MEMORY,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("cannot construct Smalltalk ABI types"));
        } else {
            set_diagnostic(result, ST_LOWER_ERR_IR_BUILD,
                           ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                           static_string("StFrame type conflicts with ABI"));
        }
        goto done;
    }
    module = anvil_module_create(ctx, options->symbol_name);
    if (module == NULL) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    lowerer.module = module;
    if (flight.uses_image_runtime && !build_image_runtime_abi(&lowerer)) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    if (required_root_capacity != 0u && !build_send_abi(&lowerer)) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    if (control_scope && !build_control_abi(&lowerer)) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    if (artifact_block_count != 0u && !build_closure_abi(&lowerer)) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    if ((flight.heap_primitive || flight.heap_access_count != 0u)
            && !build_heap_primitive_abi(&lowerer)) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    if (lowerer.primitive_binding != NULL
            && !(lowerer.runtime_symbol_primitive
                 ? build_runtime_primitive_abi(&lowerer)
                 : lowerer.heap_primitive
                    ? true
                    : build_primitive_abi(&lowerer))) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }
    lowerer.function = anvil_func_create(
        module, options->symbol_name, method_type, options->linkage);
    if (lowerer.function == NULL
            || !anvil_set_insert_point(ctx,
                                       anvil_func_get_entry(lowerer.function))
            || !validate_entry_frame(&lowerer)
            || !initialize_bindings(&lowerer)
            || (control_scope && !setup_control_scope(&lowerer))) {
        fail_ir(&lowerer, graph_method->node);
        goto done;
    }

    bool terminated = false;
    if (lowerer.primitive_binding != NULL) {
        if (!lower_primitive_prologue(&lowerer)) {
            fail_ir(&lowerer, graph_method->node);
            goto done;
        }
        terminated = lowerer.primitive_binding->primitive->failure_policy
            == ST_PRIMITIVE_CANNOT_FAIL;
    }
    const st_ast_node_t *body = graph_method->node->as.method.body;
    for (size_t index = 0u; !terminated
         && index < body->as.block.expressions.count; index++) {
        lowered_value_t value = lower_expression(
            &lowerer, body->as.block.expressions.items[index]);
        if (result->status != ST_LOWER_OK) goto done;
        if (value.terminated) {
            terminated = true;
            break;
        }
        if (value.value == NULL) {
            fail_ir(&lowerer, body->as.block.expressions.items[index]);
            goto done;
        }
    }
    if (!terminated
            && !(control_scope
                ? emit_control_return(&lowerer, lowerer.self)
                : anvil_build_ret(ctx, lowerer.self))) {
        fail_ir(&lowerer, body);
        goto done;
    }
    if (control_scope && !finish_control_epilogue(&lowerer)) {
        fail_ir(&lowerer, body);
        goto done;
    }
    size_t artifact_index = 0u;
    for (size_t index = 0u; index < sema->block_count; index++) {
        if (!sema_block_needs_artifact(sema, graph_method->node, index))
            continue;
        lowerer.current_block = sema->blocks[index].node;
        lowerer.current_block_info = &sema->blocks[index];
        lowerer.block_artifact = &implementation->blocks[artifact_index++];
        if (!build_escaping_block_function(&lowerer, method_type)) {
            fail_ir(&lowerer, sema->blocks[index].node);
            goto done;
        }
    }
    if (lowerer.next_safepoint_id != flight.safepoint_count
            || lowerer.next_send_site != flight.send_count
            || lowerer.scratch_depth != 0u
            || lowerer.next_string_literal != flight.string_literal_count
            || lowerer.next_string_byte != flight.string_literal_bytes
            || lowerer.next_image_load != flight.image_load_count
            || lowerer.next_heap_access != flight.heap_access_count) {
        set_diagnostic(result, ST_LOWER_ERR_IR_BUILD,
                       ST_LOWER_DIAG_IR_BUILD, graph_method->node,
                       static_string("send/root plan changed during lowering"));
        goto done;
    }
    if (!anvil_module_verify(module, verify_error, sizeof(verify_error))) {
        st_lower_status_t status = anvil_ctx_get_last_error(ctx)
            == ANVIL_ERR_NOMEM ? ST_LOWER_ERR_OUT_OF_MEMORY
                               : ST_LOWER_ERR_VERIFY;
        set_diagnostic(result, status, ST_LOWER_DIAG_IR_VERIFY,
                       graph_method->node,
                       static_string("generated Smalltalk IR failed verification"));
        goto done;
    }

    result->module = module;
    result->function = lowerer.function;
    result->frame_type = lowerer.frame_type;
    result->method_type = method_type;
    result->required_root_capacity = required_root_capacity;
    result->safepoint_count = lowerer.next_safepoint_id;
    result->send_site_count = lowerer.next_send_site;
    result->method_flags = control_scope ? ST_METHOD_CAN_UNWIND : 0u;
    if (sema->may_be_nonlocal_return_home)
        result->method_flags |= ST_METHOD_HAS_NON_LOCAL_RETURN;
    result->root_maps = implementation
        ? implementation->root_maps : NULL;
    result->root_map_count = implementation ? flight.safepoint_count : 0u;
    result->blocks = artifact_block_count != 0u
        ? implementation->blocks : NULL;
    result->block_count = artifact_block_count;
    result->string_literals = flight.string_literal_count != 0u
        ? implementation->string_literals : NULL;
    result->string_literal_count = flight.string_literal_count;
    if (lowerer.primitive_binding != NULL) {
        result->primitive_intrinsic_id =
            lowerer.primitive_binding->primitive->intrinsic_id;
        result->primitive_failure_policy =
            lowerer.primitive_binding->primitive->failure_policy;
        result->has_primitive = true;
    }
    result->implementation = implementation;
    implementation = NULL;
    module = NULL;
done:
    if (module != NULL) anvil_module_destroy(module);
    if (locations != NULL) allocator.deallocate(allocator.user, locations);
    if (runtime_primitive_symbol != NULL)
        allocator.deallocate(allocator.user, runtime_primitive_symbol);
    if (closure_descriptor_globals != NULL)
        allocator.deallocate(allocator.user, closure_descriptor_globals);
    if (implementation != NULL) {
        for (size_t index = 0u; index < implementation->block_count; index++) {
            if (implementation->block_symbols
                    && implementation->block_symbols[index])
                allocator.deallocate(
                    allocator.user, implementation->block_symbols[index]);
            if (implementation->block_captures
                    && implementation->block_captures[index])
                allocator.deallocate(
                    allocator.user, implementation->block_captures[index]);
        }
        if (implementation->block_symbols)
            allocator.deallocate(allocator.user, implementation->block_symbols);
        if (implementation->block_captures)
            allocator.deallocate(allocator.user, implementation->block_captures);
        if (implementation->blocks)
            allocator.deallocate(allocator.user, implementation->blocks);
        if (implementation->bitmap != NULL)
            allocator.deallocate(allocator.user, implementation->bitmap);
        if (implementation->root_maps != NULL)
            allocator.deallocate(allocator.user, implementation->root_maps);
        if (implementation->string_literal_bytes != NULL)
            allocator.deallocate(allocator.user,
                                 implementation->string_literal_bytes);
        if (implementation->string_literals != NULL)
            allocator.deallocate(allocator.user,
                                 implementation->string_literals);
        allocator.deallocate(allocator.user, implementation);
    }
    return result->status;
}

const char *st_lower_status_string(st_lower_status_t status)
{
    switch (status) {
    case ST_LOWER_OK: return "ok";
    case ST_LOWER_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_LOWER_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_LOWER_ERR_OVERFLOW: return "overflow";
    case ST_LOWER_ERR_UNSUPPORTED: return "unsupported Smalltalk construct";
    case ST_LOWER_ERR_IR_BUILD: return "Anvil IR construction failed";
    case ST_LOWER_ERR_VERIFY: return "Anvil IR verification failed";
    default: return "invalid lowering status";
    }
}

const char *st_lower_diagnostic_string(st_lower_diagnostic_code_t code)
{
    switch (code) {
    case ST_LOWER_DIAG_NONE: return "no diagnostic";
    case ST_LOWER_DIAG_INVALID_INPUT: return "invalid lowering input";
    case ST_LOWER_DIAG_UNSUPPORTED_TARGET: return "unsupported target ABI";
    case ST_LOWER_DIAG_UNSUPPORTED_PRIMITIVE:
        return "primitive/pragma lowering is not implemented";
    case ST_LOWER_DIAG_UNSUPPORTED_NODE: return "unsupported AST node";
    case ST_LOWER_DIAG_UNSUPPORTED_SEND: return "unsupported message send";
    case ST_LOWER_DIAG_UNSUPPORTED_BINDING: return "unsupported binding storage";
    case ST_LOWER_DIAG_LITERAL_OUT_OF_RANGE: return "literal is not immediate";
    case ST_LOWER_DIAG_IR_BUILD: return "IR builder failure";
    case ST_LOWER_DIAG_IR_VERIFY: return "IR verifier failure";
    default: return "invalid lowering diagnostic";
    }
}
