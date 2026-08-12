#include "st_aot_compile.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    st_aot_allocator_t allocator;
} aot_result_impl_t;

typedef struct {
    const st_ast_node_t *method;
    const st_primitive_binding_t *binding;
} binding_slot_t;

typedef struct {
    uint64_t hash;
    const char *symbol;
    size_t length;
} symbol_slot_t;

typedef struct {
    const char *bytes;
    size_t length;
} global_name_ref_t;

typedef struct {
    st_aot_allocator_t allocator;
    anvil_ctx_t *context;
    st_aot_method_result_t *methods;
    size_t method_count;
    st_sema_result_t *semas;
    size_t sema_count;
    st_image_layout_result_t layout;
    st_image_emit_result_t metadata;
    st_aot_diagnostic_t *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    binding_slot_t *binding_slots;
    size_t binding_capacity;
    symbol_slot_t *symbol_slots;
    size_t symbol_capacity;
    size_t symbol_count;
    st_aot_global_result_t *globals;
    size_t global_count;
    st_lower_global_binding_t *lower_globals;
    size_t string_literal_count;
    size_t *external_name_slots;
    size_t external_name_capacity;
    uint32_t *external_seen;
    uint32_t external_generation;
} aot_builder_t;

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

static void release(st_aot_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) allocator.deallocate(allocator.user, pointer);
}

static void *allocate_array(st_aot_allocator_t allocator, size_t count,
                            size_t element_size)
{
    if (count == 0u) return NULL;
    if (element_size != 0u && count > SIZE_MAX / element_size) return NULL;
    return allocator.allocate(allocator.user, count * element_size);
}

static bool result_is_empty(const st_aot_compile_result_t *result)
{
    return result != NULL && result->status == ST_AOT_COMPILE_OK
        && result->provenance.target == ANVIL_ARCH_NONE
        && result->provenance.abi == ANVIL_ABI_DEFAULT
        && result->provenance.syntax == ANVIL_SYNTAX_DEFAULT
        && result->provenance.optimization == ANVIL_OPT_NONE
        && result->provenance.symbol_prefix == NULL
        && result->provenance.symbol_prefix_length == 0u
        && result->context == NULL && result->methods == NULL
        && result->method_count == 0u && result->metadata.module == NULL
        && result->globals == NULL && result->global_count == 0u
        && result->string_literal_count == 0u
        && result->layout.implementation == NULL
        && result->diagnostics == NULL && result->diagnostic_count == 0u
        && result->implementation == NULL;
}

void st_aot_compile_result_init(st_aot_compile_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->provenance.target = ANVIL_ARCH_NONE;
    st_image_layout_result_init(&result->layout);
    st_image_emit_result_init(&result->metadata);
}

static void diagnostic_destroy(st_aot_allocator_t allocator,
                               st_aot_diagnostic_t *diagnostic)
{
    release(allocator, diagnostic->source_name);
    release(allocator, diagnostic->detail);
    memset(diagnostic, 0, sizeof(*diagnostic));
}

static void method_destroy(st_aot_allocator_t allocator,
                           st_aot_method_result_t *method)
{
    if (method == NULL) return;
    /* The adapter borrows bitmaps from lowering.  Neither destructor reads
     * them, but destroying lowering first makes the ownership boundary clear. */
    st_lower_result_destroy(&method->lowering);
    release(allocator, method->root_maps);
    release(allocator, method->block_root_maps);
    release(allocator, method->block_captures);
    release(allocator, method->block_artifacts);
    release(allocator, method->string_literals);
    release(allocator, method->symbol);
    release(allocator, method->selector);
    memset(method, 0, sizeof(*method));
}

void st_aot_compile_result_destroy(st_aot_compile_result_t *result)
{
    aot_result_impl_t *implementation;
    st_aot_allocator_t allocator = { default_allocate, default_deallocate,
                                     NULL };
    size_t index;
    if (result == NULL) return;
    implementation = result->implementation;
    if (implementation != NULL) allocator = implementation->allocator;

    /* Modules must die before their shared Anvil context. */
    st_image_emit_result_destroy(&result->metadata);
    st_image_layout_result_destroy(&result->layout);
    for (index = 0u; index < result->method_count; index++)
        method_destroy(allocator, &result->methods[index]);
    release(allocator, result->methods);
    for (index = 0u; index < result->global_count; index++)
        release(allocator, result->globals[index].name);
    release(allocator, result->globals);
    for (index = 0u; index < result->diagnostic_count; index++)
        diagnostic_destroy(allocator, &result->diagnostics[index]);
    release(allocator, result->diagnostics);
    release(allocator, result->provenance.symbol_prefix);
    if (result->context != NULL) anvil_ctx_destroy(result->context);
    release(allocator, implementation);
    st_aot_compile_result_init(result);
}

static bool copy_bytes(aot_builder_t *builder, const char *bytes,
                       size_t length, char **copy_out)
{
    char *copy;
    *copy_out = NULL;
    if (bytes == NULL && length != 0u) return false;
    if (length == SIZE_MAX) return false;
    copy = builder->allocator.allocate(builder->allocator.user, length + 1u);
    if (copy == NULL) return false;
    if (length != 0u) memcpy(copy, bytes, length);
    copy[length] = '\0';
    *copy_out = copy;
    return true;
}

static bool grow_diagnostics(aot_builder_t *builder)
{
    st_aot_diagnostic_t *replacement;
    size_t capacity;
    if (builder->diagnostic_count < builder->diagnostic_capacity) return true;
    capacity = builder->diagnostic_capacity == 0u
        ? 8u : builder->diagnostic_capacity;
    if (builder->diagnostic_capacity != 0u) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    replacement = allocate_array(builder->allocator, capacity,
                                  sizeof(*replacement));
    if (replacement == NULL) return false;
    memset(replacement, 0, capacity * sizeof(*replacement));
    if (builder->diagnostic_count != 0u)
        memcpy(replacement, builder->diagnostics,
               builder->diagnostic_count * sizeof(*replacement));
    release(builder->allocator, builder->diagnostics);
    builder->diagnostics = replacement;
    builder->diagnostic_capacity = capacity;
    return true;
}

static bool append_diagnostic(aot_builder_t *builder,
                              st_aot_diagnostic_stage_t stage,
                              const st_class_graph_method_t *method,
                              const st_ast_node_t *site,
                              const char *detail, size_t detail_length,
                              st_aot_diagnostic_t **out)
{
    st_aot_diagnostic_t diagnostic;
    st_ast_string_t source = {0};
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.stage = stage;
    if (method != NULL) {
        diagnostic.has_method = true;
        diagnostic.method_id = method->id;
        diagnostic.unit_index = method->origin.unit_index;
        source = method->origin.source_name;
        diagnostic.span = site != NULL ? site->span : method->origin.span;
        diagnostic.has_span = true;
    } else if (site != NULL) {
        diagnostic.span = site->span;
        diagnostic.has_span = true;
    }
    if ((source.length != 0u
            && !copy_bytes(builder, source.data, source.length,
                           &diagnostic.source_name))
            || !copy_bytes(builder, detail, detail_length,
                           &diagnostic.detail)
            || !grow_diagnostics(builder)) {
        diagnostic_destroy(builder->allocator, &diagnostic);
        return false;
    }
    diagnostic.source_name_length = source.length;
    diagnostic.detail_length = detail_length;
    builder->diagnostics[builder->diagnostic_count] = diagnostic;
    if (out != NULL) *out = &builder->diagnostics[builder->diagnostic_count];
    builder->diagnostic_count++;
    return true;
}

static bool append_c_string_diagnostic(aot_builder_t *builder,
                                       st_aot_diagnostic_stage_t stage,
                                       const st_class_graph_method_t *method,
                                       const st_ast_node_t *site,
                                       const char *detail,
                                       st_aot_diagnostic_t **out)
{
    return append_diagnostic(builder, stage, method, site, detail,
                             detail != NULL ? strlen(detail) : 0u, out);
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

static uint64_t hash_bytes(const void *bytes, size_t length)
{
    const unsigned char *cursor = bytes;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t index;
    for (index = 0u; index < length; index++) {
        hash ^= cursor[index];
        hash *= UINT64_C(0x100000001b3);
    }
    return mix64(hash ^ (uint64_t)length);
}

static uint64_t hash_pointer(const void *pointer)
{
    return mix64((uint64_t)(uintptr_t)pointer);
}

static bool table_capacity(size_t count, size_t *capacity_out)
{
    size_t capacity = 8u;
    if (count > SIZE_MAX / 2u) return false;
    while (capacity < count * 2u) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    *capacity_out = capacity;
    return true;
}

static int global_id_compare(const void *left, const void *right)
{
    const st_aot_global_result_t *a = left;
    const st_aot_global_result_t *b = right;
    return a->semantic_external_id < b->semantic_external_id ? -1
        : a->semantic_external_id != b->semantic_external_id;
}

static int global_name_compare(const void *left, const void *right)
{
    const global_name_ref_t *a = left;
    const global_name_ref_t *b = right;
    size_t common = a->length < b->length ? a->length : b->length;
    int order = common != 0u ? memcmp(a->bytes, b->bytes, common) : 0;
    if (order != 0) return order;
    return a->length < b->length ? -1 : a->length != b->length;
}

static bool exact_name(const char *bytes, size_t length)
{
    return bytes != NULL && length != 0u && length != SIZE_MAX
        && bytes[length] == '\0' && memchr(bytes, '\0', length) == NULL;
}

static st_aot_compile_status_t build_global_table(
    aot_builder_t *builder, const st_aot_compile_options_t *options)
{
    const st_class_graph_result_t *graph = options->graph;
    size_t graph_count = graph->catalog_entry_count;
    size_t external_count = options->external_global_count;
    size_t total, index, cursor = 0u, next_runtime = 0u;
    unsigned char *runtime_used = NULL;
    global_name_ref_t *names = NULL;
    st_aot_compile_status_t status = ST_AOT_COMPILE_OK;

    if ((!options->external_globals) != (external_count == 0u)) {
        return append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT,
                NULL, NULL,
                "external global table pointer/count mismatch", NULL)
            ? ST_AOT_COMPILE_ERR_INVALID_ARGUMENT
            : ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    }
    if (external_count > SIZE_MAX - graph_count)
        return ST_AOT_COMPILE_ERR_OVERFLOW;
    total = graph_count + external_count;
    if (total > UINT32_MAX) return ST_AOT_COMPILE_ERR_OVERFLOW;
    builder->globals = allocate_array(builder->allocator, total,
                                      sizeof(*builder->globals));
    runtime_used = allocate_array(builder->allocator, total,
                                  sizeof(*runtime_used));
    names = allocate_array(builder->allocator, total, sizeof(*names));
    if (total != 0u && (!builder->globals || !runtime_used || !names)) {
        status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    if (builder->globals) memset(builder->globals, 0,
                                 total * sizeof(*builder->globals));
    builder->global_count = total;
    if (runtime_used) memset(runtime_used, 0, total);
    if (!table_capacity(external_count, &builder->external_name_capacity)) {
        status = ST_AOT_COMPILE_ERR_OVERFLOW;
        goto done;
    }
    builder->external_name_slots = allocate_array(builder->allocator,
        builder->external_name_capacity,
        sizeof(*builder->external_name_slots));
    builder->external_seen = allocate_array(builder->allocator,
        external_count, sizeof(*builder->external_seen));
    if (!builder->external_name_slots
            || (external_count != 0u && !builder->external_seen)) {
        status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(builder->external_name_slots, 0,
           builder->external_name_capacity
               * sizeof(*builder->external_name_slots));
    if (builder->external_seen)
        memset(builder->external_seen, 0,
               external_count * sizeof(*builder->external_seen));

    for (index = 0u; index < external_count; index++) {
        const st_aot_external_global_t *external =
            &options->external_globals[index];
        st_aot_global_result_t *target = &builder->globals[cursor++];
        if (!exact_name(external->name, external->name_length)
                || external->semantic_external_id == 0u
                || external->semantic_external_id == ST_SEMA_INVALID_ID
                || external->runtime_index >= total
                || runtime_used[external->runtime_index]) {
            status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
            goto invalid;
        }
        if (!copy_bytes(builder, external->name, external->name_length,
                        &target->name)) {
            status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            goto done;
        }
        target->name_length = external->name_length;
        target->semantic_external_id = external->semantic_external_id;
        target->runtime_index = external->runtime_index;
        names[cursor - 1u] = (global_name_ref_t){
            target->name, target->name_length
        };
        runtime_used[external->runtime_index] = 1u;
        {
            size_t slot = (size_t)hash_bytes(external->name,
                external->name_length)
                & (builder->external_name_capacity - 1u);
            while (builder->external_name_slots[slot] != 0u)
                slot = (slot + 1u)
                    & (builder->external_name_capacity - 1u);
            builder->external_name_slots[slot] = index + 1u;
        }
    }
    for (index = 0u; index < graph_count; index++) {
        const st_sema_external_t *source = &graph->catalog_entries[index];
        st_aot_global_result_t *target = &builder->globals[cursor++];
        if (source->kind != ST_SEMA_EXTERNAL_GLOBAL
                || source->external_id == 0u
                || source->external_id == ST_SEMA_INVALID_ID
                || source->name.data == NULL || source->name.length == 0u) {
            status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
            goto invalid;
        }
        while (next_runtime < total && runtime_used[next_runtime])
            next_runtime++;
        if (next_runtime >= total) {
            status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
            goto invalid;
        }
        if (!copy_bytes(builder, source->name.data, source->name.length,
                        &target->name)) {
            status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            goto done;
        }
        target->name_length = source->name.length;
        target->semantic_external_id = source->external_id;
        target->runtime_index = (uint32_t)next_runtime;
        names[cursor - 1u] = (global_name_ref_t){
            target->name, target->name_length
        };
        runtime_used[next_runtime++] = 1u;
    }
    if (cursor != total) {
        status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        goto invalid;
    }
    if (total > 1u) {
        qsort(builder->globals, total, sizeof(*builder->globals),
              global_id_compare);
        qsort(names, total, sizeof(*names), global_name_compare);
    }
    for (index = 0u; index < total; index++) {
        if ((index != 0u && builder->globals[index - 1u].semantic_external_id
                >= builder->globals[index].semantic_external_id)
                || (index != 0u && names[index - 1u].length
                        == names[index].length
                    && memcmp(names[index - 1u].bytes, names[index].bytes,
                              names[index].length) == 0)
                || runtime_used[index] != 1u) {
            status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
            goto invalid;
        }
    }
    goto done;

invalid:
    if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT, NULL, NULL,
            "invalid external/global image-runtime mapping", NULL))
        status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
done:
    release(builder->allocator, names);
    release(builder->allocator, runtime_used);
    return status;
}

static st_aot_compile_status_t build_binding_index(
    aot_builder_t *builder, const st_aot_compile_options_t *options)
{
    const st_primitive_result_t *primitives = options->primitives;
    const st_class_graph_result_t *graph = options->graph;
    size_t index;
    if (primitives == NULL || !primitives->resolved
            || primitives->status != ST_PRIMITIVE_OK)
        return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
    if (!table_capacity(primitives->binding_count,
                        &builder->binding_capacity))
        return ST_AOT_COMPILE_ERR_OVERFLOW;
    builder->binding_slots = allocate_array(
        builder->allocator, builder->binding_capacity,
        sizeof(*builder->binding_slots));
    if (builder->binding_slots == NULL)
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    memset(builder->binding_slots, 0,
           builder->binding_capacity * sizeof(*builder->binding_slots));
    for (index = 0u; index < primitives->binding_count; index++) {
        const st_primitive_binding_t *binding = &primitives->bindings[index];
        const st_class_graph_method_t *method;
        size_t slot;
        st_aot_diagnostic_t *diagnostic;
        if (binding->method == NULL || binding->primitive == NULL
                || binding->pragma == NULL) {
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT,
                    NULL, binding->method,
                    "malformed primitive binding", NULL))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        }
        method = st_class_graph_method_for_node(graph, binding->method);
        if (method == NULL) {
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT,
                    NULL, binding->method,
                    "primitive binding method is outside the class graph",
                    NULL))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        }
        slot = (size_t)hash_pointer(binding->method)
            & (builder->binding_capacity - 1u);
        while (builder->binding_slots[slot].method != NULL
                && builder->binding_slots[slot].method != binding->method)
            slot = (slot + 1u) & (builder->binding_capacity - 1u);
        if (builder->binding_slots[slot].method != NULL) {
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT,
                    method, binding->pragma,
                    "duplicate primitive binding for method", &diagnostic))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            diagnostic->primitive_code = ST_PRIMITIVE_DIAG_DUPLICATE_PRAGMA;
            return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        }
        builder->binding_slots[slot].method = binding->method;
        builder->binding_slots[slot].binding = binding;
    }
    return ST_AOT_COMPILE_OK;
}

static const st_primitive_binding_t *binding_for_method(
    const aot_builder_t *builder, const st_ast_node_t *method)
{
    size_t slot;
    if (builder->binding_capacity == 0u || method == NULL) return NULL;
    slot = (size_t)hash_pointer(method) & (builder->binding_capacity - 1u);
    while (builder->binding_slots[slot].method != NULL) {
        if (builder->binding_slots[slot].method == method)
            return builder->binding_slots[slot].binding;
        slot = (slot + 1u) & (builder->binding_capacity - 1u);
    }
    return NULL;
}

static st_aot_compile_status_t collect_primitive_diagnostics(
    aot_builder_t *builder, const st_aot_compile_options_t *options)
{
    const st_primitive_result_t *primitives = options->primitives;
    size_t index;
    for (index = 0u; index < primitives->diagnostic_count; index++) {
        const st_primitive_diagnostic_t *source =
            &primitives->diagnostics[index];
        const st_class_graph_method_t *method =
            st_class_graph_method_for_node(options->graph, source->method);
        const char *message = st_primitive_diagnostic_string(source->code);
        const char *detail = message;
        size_t detail_length = strlen(message);
        char *expanded = NULL;
        st_aot_diagnostic_t *diagnostic;
        if (source->requested_name.data != NULL
                && source->requested_name.length != 0u) {
            if (detail_length > SIZE_MAX - source->requested_name.length - 3u)
                return ST_AOT_COMPILE_ERR_OVERFLOW;
            detail_length += source->requested_name.length + 2u;
            expanded = builder->allocator.allocate(builder->allocator.user,
                                                    detail_length + 1u);
            if (expanded == NULL) return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            memcpy(expanded, message, strlen(message));
            memcpy(expanded + strlen(message), ": ", 2u);
            memcpy(expanded + strlen(message) + 2u,
                   source->requested_name.data, source->requested_name.length);
            expanded[detail_length] = '\0';
            detail = expanded;
        }
        if (!append_diagnostic(builder, ST_AOT_DIAG_PRIMITIVE,
                method, source->pragma != NULL ? source->pragma : source->method,
                detail, detail_length, &diagnostic)) {
            release(builder->allocator, expanded);
            return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        }
        release(builder->allocator, expanded);
        diagnostic->primitive_code = source->code;
    }
    return primitives->diagnostic_count == 0u
        ? ST_AOT_COMPILE_OK : ST_AOT_COMPILE_ERR_PRIMITIVES;
}

static bool count_ast_variables(const st_ast_node_t *node, size_t *count)
{
    const st_ast_list_t *lists[5];
    size_t list_count = 0u, list_index, item_index;
    if (!node) return true;
    if (node->kind == ST_AST_VARIABLE) {
        if (*count == SIZE_MAX) return false;
        (*count)++;
        return true;
    }
    switch (node->kind) {
    case ST_AST_CLASS:
        lists[list_count++] = &node->as.class_decl.pragmas;
        lists[list_count++] = &node->as.class_decl.variables;
        lists[list_count++] = &node->as.class_decl.methods;
        lists[list_count++] = &node->as.class_decl.members;
        if (!count_ast_variables(node->as.class_decl.name, count)
                || !count_ast_variables(node->as.class_decl.super_name, count))
            return false;
        break;
    case ST_AST_METHOD:
        lists[list_count++] = &node->as.method.arguments;
        lists[list_count++] = &node->as.method.pragmas;
        if (!count_ast_variables(node->as.method.body, count)) return false;
        break;
    case ST_AST_BLOCK:
        lists[list_count++] = &node->as.block.arguments;
        lists[list_count++] = &node->as.block.temporaries;
        lists[list_count++] = &node->as.block.expressions;
        break;
    case ST_AST_EXPRESSION:
        lists[list_count++] = &node->as.expression.assignments;
        lists[list_count++] = &node->as.expression.messages;
        if (!count_ast_variables(node->as.expression.receiver, count))
            return false;
        break;
    case ST_AST_MESSAGE:
        lists[list_count++] = &node->as.message.arguments;
        break;
    case ST_AST_LITERAL_ARRAY:
        lists[list_count++] = &node->as.array.elements;
        break;
    case ST_AST_VARIABLE:
        break;
    default:
        return true;
    }
    for (list_index = 0u; list_index < list_count; list_index++)
        for (item_index = 0u; item_index < lists[list_index]->count;
             item_index++)
            if (!count_ast_variables(lists[list_index]->items[item_index],
                                     count))
                return false;
    return true;
}

static size_t external_for_name(const aot_builder_t *builder,
                                const st_aot_compile_options_t *options,
                                st_ast_string_t name)
{
    size_t slot;
    if (options->external_global_count == 0u || name.data == NULL
            || name.length == 0u)
        return SIZE_MAX;
    slot = (size_t)hash_bytes(name.data, name.length)
        & (builder->external_name_capacity - 1u);
    while (builder->external_name_slots[slot] != 0u) {
        size_t index = builder->external_name_slots[slot] - 1u;
        const st_aot_external_global_t *external =
            &options->external_globals[index];
        if (external->name_length == name.length
                && memcmp(external->name, name.data, name.length) == 0)
            return index;
        slot = (slot + 1u) & (builder->external_name_capacity - 1u);
    }
    return SIZE_MAX;
}

static bool collect_referenced_externals(
    aot_builder_t *builder, const st_aot_compile_options_t *options,
    const st_ast_node_t *node, size_t *indices, size_t capacity,
    size_t *count)
{
    const st_ast_list_t *lists[5];
    size_t list_count = 0u, list_index, item_index;
    if (!node) return true;
    if (node->kind == ST_AST_VARIABLE) {
        size_t external = external_for_name(builder, options,
                                            node->as.variable.name);
        if (external != SIZE_MAX
                && builder->external_seen[external]
                    != builder->external_generation) {
            if (*count >= capacity) return false;
            builder->external_seen[external] = builder->external_generation;
            indices[(*count)++] = external;
        }
        return true;
    }
    switch (node->kind) {
    case ST_AST_CLASS:
        lists[list_count++] = &node->as.class_decl.pragmas;
        lists[list_count++] = &node->as.class_decl.variables;
        lists[list_count++] = &node->as.class_decl.methods;
        lists[list_count++] = &node->as.class_decl.members;
        if (!collect_referenced_externals(builder, options,
                node->as.class_decl.name, indices, capacity, count)
                || !collect_referenced_externals(builder, options,
                    node->as.class_decl.super_name, indices, capacity, count))
            return false;
        break;
    case ST_AST_METHOD:
        lists[list_count++] = &node->as.method.arguments;
        lists[list_count++] = &node->as.method.pragmas;
        if (!collect_referenced_externals(builder, options,
                node->as.method.body, indices, capacity, count)) return false;
        break;
    case ST_AST_BLOCK:
        lists[list_count++] = &node->as.block.arguments;
        lists[list_count++] = &node->as.block.temporaries;
        lists[list_count++] = &node->as.block.expressions;
        break;
    case ST_AST_EXPRESSION:
        lists[list_count++] = &node->as.expression.assignments;
        lists[list_count++] = &node->as.expression.messages;
        if (!collect_referenced_externals(builder, options,
                node->as.expression.receiver, indices, capacity, count))
            return false;
        break;
    case ST_AST_MESSAGE:
        lists[list_count++] = &node->as.message.arguments;
        break;
    case ST_AST_LITERAL_ARRAY:
        lists[list_count++] = &node->as.array.elements;
        break;
    case ST_AST_VARIABLE:
        break;
    default:
        return true;
    }
    for (list_index = 0u; list_index < list_count; list_index++)
        for (item_index = 0u; item_index < lists[list_index]->count;
             item_index++)
            if (!collect_referenced_externals(builder, options,
                    lists[list_index]->items[item_index], indices, capacity,
                    count))
                return false;
    return true;
}

static st_aot_compile_status_t analyze_methods(
    aot_builder_t *builder, const st_aot_compile_options_t *options)
{
    const st_class_graph_result_t *graph = options->graph;
    st_aot_compile_status_t overall = ST_AOT_COMPILE_OK;
    size_t index;
    builder->semas = allocate_array(builder->allocator, graph->method_count,
                                    sizeof(*builder->semas));
    if (graph->method_count != 0u && builder->semas == NULL)
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    if (builder->semas != NULL)
        memset(builder->semas, 0, graph->method_count * sizeof(*builder->semas));
    builder->sema_count = graph->method_count;
    for (index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *method = &graph->methods[index];
        st_class_graph_sema_view_t view;
        st_class_graph_status_t graph_status;
        st_sema_status_t sema_status;
        st_sema_catalog_t catalog;
        st_sema_external_t *merged = NULL;
        size_t *selected = NULL;
        size_t variable_count = 0u, selected_count = 0u;
        size_t diagnostic_index;
        st_sema_result_init(&builder->semas[index]);
        st_class_graph_sema_view_init(&view);
        graph_status = st_class_graph_sema_view_build_minimal(
            &view, graph, method->id);
        if (graph_status != ST_CLASS_GRAPH_OK) {
            st_aot_diagnostic_t *diagnostic;
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_SEMA_VIEW,
                    method, method->node,
                    st_class_graph_status_string(graph_status), &diagnostic)) {
                st_class_graph_sema_view_destroy(&view);
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            }
            diagnostic->graph_status = graph_status;
            st_class_graph_sema_view_destroy(&view);
            if (graph_status == ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY)
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            if (graph_status == ST_CLASS_GRAPH_ERR_OVERFLOW)
                return ST_AOT_COMPILE_ERR_OVERFLOW;
            overall = ST_AOT_COMPILE_ERR_SEMANTIC;
            continue;
        }
        catalog = view.catalog;
        catalog.allocator = (st_sema_allocator_t){
            builder->allocator.allocate, builder->allocator.deallocate,
            builder->allocator.user
        };
        if (!count_ast_variables(method->node, &variable_count)) {
            st_class_graph_sema_view_destroy(&view);
            return ST_AOT_COMPILE_ERR_OVERFLOW;
        }
        selected = allocate_array(builder->allocator, variable_count,
                                  sizeof(*selected));
        if (variable_count != 0u && !selected) {
            st_class_graph_sema_view_destroy(&view);
            return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        }
        builder->external_generation++;
        if (builder->external_generation == 0u) {
            if (options->external_global_count != 0u)
                memset(builder->external_seen, 0,
                       options->external_global_count
                           * sizeof(*builder->external_seen));
            builder->external_generation = 1u;
        }
        if (!collect_referenced_externals(builder, options, method->node,
                selected, variable_count, &selected_count)) {
            release(builder->allocator, selected);
            st_class_graph_sema_view_destroy(&view);
            return ST_AOT_COMPILE_ERR_OVERFLOW;
        }
        if (selected_count != 0u) {
            size_t merged_count;
            if (catalog.count > SIZE_MAX - selected_count) {
                release(builder->allocator, selected);
                st_class_graph_sema_view_destroy(&view);
                return ST_AOT_COMPILE_ERR_OVERFLOW;
            }
            merged_count = catalog.count + selected_count;
            merged = allocate_array(builder->allocator, merged_count,
                                    sizeof(*merged));
            if (!merged) {
                release(builder->allocator, selected);
                st_class_graph_sema_view_destroy(&view);
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            }
            if (catalog.count != 0u)
                memcpy(merged, catalog.entries,
                       catalog.count * sizeof(*merged));
            for (size_t selected_index = 0u;
                 selected_index < selected_count; selected_index++) {
                size_t external_index = selected[selected_index];
                const st_aot_external_global_t *source =
                    &options->external_globals[external_index];
                st_sema_external_t *target =
                    &merged[catalog.count + selected_index];
                memset(target, 0, sizeof(*target));
                target->name.data = source->name;
                target->name.length = source->name_length;
                target->kind = ST_SEMA_EXTERNAL_GLOBAL;
                target->external_id = source->semantic_external_id;
            }
            catalog.entries = merged;
            catalog.count = merged_count;
        }
        sema_status = st_sema_analyze_method(
            &builder->semas[index], method->node, &catalog);
        release(builder->allocator, selected);
        release(builder->allocator, merged);
        st_class_graph_sema_view_destroy(&view);
        if (sema_status != ST_SEMA_OK) {
            st_aot_diagnostic_t *diagnostic;
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_SEMANTIC,
                    method, method->node, st_sema_status_string(sema_status),
                    &diagnostic))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            diagnostic->sema_status = sema_status;
            if (sema_status == ST_SEMA_ERR_OUT_OF_MEMORY)
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            if (sema_status == ST_SEMA_ERR_OVERFLOW)
                return ST_AOT_COMPILE_ERR_OVERFLOW;
            overall = ST_AOT_COMPILE_ERR_SEMANTIC;
            continue;
        }
        for (diagnostic_index = 0u;
             diagnostic_index < builder->semas[index].diagnostic_count;
             diagnostic_index++) {
            const st_sema_diagnostic_t *source =
                &builder->semas[index].diagnostics[diagnostic_index];
            st_aot_diagnostic_t *diagnostic;
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_SEMANTIC,
                    method, NULL, st_sema_diagnostic_string(source->code),
                    &diagnostic))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            diagnostic->span = source->span;
            diagnostic->has_span = true;
            diagnostic->sema_status = ST_SEMA_OK;
            diagnostic->sema_code = source->code;
            overall = ST_AOT_COMPILE_ERR_SEMANTIC;
        }
    }
    return overall;
}

static bool portable_prefix(const char *prefix, size_t length)
{
    size_t index;
    unsigned char byte;
    if (prefix == NULL || length == 0u || length > ST_AOT_SYMBOL_PREFIX_MAX
            || prefix[length] != '\0')
        return false;
    byte = (unsigned char)prefix[0];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || byte == '_'))
        return false;
    for (index = 1u; index < length; index++) {
        byte = (unsigned char)prefix[index];
        if (!((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9') || byte == '_'))
            return false;
    }
    return true;
}

static bool target_configuration(anvil_arch_t target, anvil_abi_t requested,
                                 anvil_syntax_t syntax,
                                 anvil_abi_t *effective_out,
                                 anvil_syntax_t *syntax_out)
{
    bool mainframe = target == ANVIL_ARCH_S370
        || target == ANVIL_ARCH_S370_XA || target == ANVIL_ARCH_S390
        || target == ANVIL_ARCH_ZARCH;
    if ((mainframe && syntax == ANVIL_SYNTAX_GAS)
            || (!mainframe && syntax == ANVIL_SYNTAX_HLASM))
        return false;
    *syntax_out = syntax == ANVIL_SYNTAX_DEFAULT
        ? (mainframe ? ANVIL_SYNTAX_HLASM : ANVIL_SYNTAX_GAS) : syntax;
    switch (target) {
    case ANVIL_ARCH_X86:
    case ANVIL_ARCH_X86_64:
        if (requested != ANVIL_ABI_DEFAULT && requested != ANVIL_ABI_SYSV
                && requested != ANVIL_ABI_DARWIN
                && requested != ANVIL_ABI_WIN64)
            return false;
        *effective_out = requested == ANVIL_ABI_DEFAULT
            ? ANVIL_ABI_SYSV : requested;
        return true;
    case ANVIL_ARCH_ARM64:
        if (requested != ANVIL_ABI_DEFAULT && requested != ANVIL_ABI_SYSV
                && requested != ANVIL_ABI_DARWIN)
            return false;
        *effective_out = requested == ANVIL_ABI_DEFAULT
            ? ANVIL_ABI_SYSV : requested;
        return true;
    case ANVIL_ARCH_PPC32:
    case ANVIL_ARCH_PPC64:
    case ANVIL_ARCH_PPC64LE:
        if (requested != ANVIL_ABI_DEFAULT && requested != ANVIL_ABI_SYSV)
            return false;
        *effective_out = requested == ANVIL_ABI_DEFAULT
            ? ANVIL_ABI_SYSV : requested;
        return true;
    case ANVIL_ARCH_S370:
    case ANVIL_ARCH_S370_XA:
    case ANVIL_ARCH_S390:
    case ANVIL_ARCH_ZARCH:
        if (requested != ANVIL_ABI_DEFAULT && requested != ANVIL_ABI_MVS)
            return false;
        *effective_out = requested == ANVIL_ABI_DEFAULT
            ? ANVIL_ABI_MVS : requested;
        return true;
    case ANVIL_ARCH_NONE:
    case ANVIL_ARCH_COUNT:
        return false;
    }
    return false;
}

static st_aot_compile_status_t make_method_symbol(
    aot_builder_t *builder, const st_aot_compile_options_t *options,
    const st_class_graph_method_t *method, char **symbol_out,
    size_t *length_out)
{
    uint64_t selector_hash = hash_bytes(method->selector.data,
                                        method->selector.length);
    size_t length;
    char *symbol;
    int written;
    if (options->symbol_prefix_length > SIZE_MAX - 28u)
        return ST_AOT_COMPILE_ERR_OVERFLOW;
    length = options->symbol_prefix_length + 28u;
    symbol = builder->allocator.allocate(builder->allocator.user, length + 1u);
    if (symbol == NULL) return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    written = snprintf(symbol, length + 1u, "%.*s_m%08" PRIx32
                       "_s%016" PRIx64,
                       (int)options->symbol_prefix_length,
                       options->symbol_prefix, method->id, selector_hash);
    if (written < 0 || (size_t)written != length) {
        release(builder->allocator, symbol);
        return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
    }
    *symbol_out = symbol;
    *length_out = length;
    return ST_AOT_COMPILE_OK;
}

static st_aot_compile_status_t insert_symbol(aot_builder_t *builder,
                                              const char *symbol,
                                              size_t length)
{
    uint64_t hash;
    size_t slot;
    if (builder->symbol_count + 1u > builder->symbol_capacity / 2u) {
        symbol_slot_t *replacement;
        size_t old_index;
        size_t new_capacity;
        if (builder->symbol_capacity > SIZE_MAX / 2u)
            return ST_AOT_COMPILE_ERR_OVERFLOW;
        new_capacity = builder->symbol_capacity * 2u;
        replacement = allocate_array(builder->allocator, new_capacity,
                                     sizeof(*replacement));
        if (!replacement) return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        memset(replacement, 0, new_capacity * sizeof(*replacement));
        for (old_index = 0u; old_index < builder->symbol_capacity;
             old_index++) {
            symbol_slot_t entry = builder->symbol_slots[old_index];
            size_t new_slot;
            if (!entry.symbol) continue;
            new_slot = (size_t)entry.hash & (new_capacity - 1u);
            while (replacement[new_slot].symbol)
                new_slot = (new_slot + 1u) & (new_capacity - 1u);
            replacement[new_slot] = entry;
        }
        release(builder->allocator, builder->symbol_slots);
        builder->symbol_slots = replacement;
        builder->symbol_capacity = new_capacity;
    }
    hash = hash_bytes(symbol, length);
    slot = (size_t)hash & (builder->symbol_capacity - 1u);
    while (builder->symbol_slots[slot].symbol != NULL) {
        if (builder->symbol_slots[slot].hash == hash
                && builder->symbol_slots[slot].length == length
                && memcmp(builder->symbol_slots[slot].symbol,
                          symbol, length) == 0)
            return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        slot = (slot + 1u) & (builder->symbol_capacity - 1u);
    }
    builder->symbol_slots[slot].hash = hash;
    builder->symbol_slots[slot].symbol = symbol;
    builder->symbol_slots[slot].length = length;
    builder->symbol_count++;
    return ST_AOT_COMPILE_OK;
}

static st_aot_compile_status_t adapt_root_maps(
    aot_builder_t *builder, st_aot_method_result_t *method)
{
    const st_lower_result_t *lowering = &method->lowering;
    size_t index;
    method->root_maps = allocate_array(builder->allocator,
        lowering->root_map_count, sizeof(*method->root_maps));
    if (lowering->root_map_count != 0u && method->root_maps == NULL)
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    for (index = 0u; index < lowering->root_map_count; index++) {
        const st_lower_root_map_t *source = &lowering->root_maps[index];
        method->root_maps[index].safepoint_id = source->safepoint_id;
        method->root_maps[index].root_count = source->root_count;
        method->root_maps[index].bitmap_word_count = source->bitmap_word_count;
        method->root_maps[index].live_root_bitmap = source->live_root_bitmap;
    }
    return ST_AOT_COMPILE_OK;
}

static st_aot_compile_status_t adapt_string_literals(
    aot_builder_t *builder, st_aot_method_result_t *method)
{
    size_t index;
    const st_lower_result_t *lowering = &method->lowering;
    method->string_literals = allocate_array(builder->allocator,
        lowering->string_literal_count, sizeof(*method->string_literals));
    if (lowering->string_literal_count != 0u && !method->string_literals)
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    for (index = 0u; index < lowering->string_literal_count; index++) {
        const st_lower_string_literal_artifact_t *source =
            &lowering->string_literals[index];
        st_image_string_literal_artifact_t *target =
            &method->string_literals[index];
        target->literal_id = source->literal_id;
        target->method_id = method->method_id;
        target->bytes = source->bytes;
        target->length = source->length;
    }
    return ST_AOT_COMPILE_OK;
}

static st_aot_compile_status_t adapt_blocks(
    aot_builder_t *builder, st_aot_method_result_t *method)
{
    const st_lower_result_t *lowering = &method->lowering;
    size_t block_index, root_total = 0u, root_cursor = 0u;
    size_t capture_total = 0u, capture_cursor = 0u;
    if (lowering->status != ST_LOWER_OK
            || ((!lowering->blocks) != (lowering->block_count == 0u)))
        return ST_AOT_COMPILE_ERR_LOWERING;
    for (block_index = 0u; block_index < lowering->block_count; block_index++) {
        const st_lower_block_artifact_t *block = &lowering->blocks[block_index];
        if (block->root_map_count > SIZE_MAX - root_total)
            return ST_AOT_COMPILE_ERR_OVERFLOW;
        if (block->capture_count > SIZE_MAX - capture_total)
            return ST_AOT_COMPILE_ERR_OVERFLOW;
        root_total += block->root_map_count;
        capture_total += block->capture_count;
    }
    if (lowering->block_count > SIZE_MAX / sizeof(*method->block_artifacts)
            || root_total > SIZE_MAX / sizeof(*method->block_root_maps)
            || capture_total > SIZE_MAX / sizeof(*method->block_captures))
        return ST_AOT_COMPILE_ERR_OVERFLOW;
    method->block_artifacts = allocate_array(builder->allocator,
        lowering->block_count, sizeof(*method->block_artifacts));
    method->block_root_maps = allocate_array(builder->allocator, root_total,
        sizeof(*method->block_root_maps));
    method->block_captures = allocate_array(builder->allocator, capture_total,
        sizeof(*method->block_captures));
    if ((lowering->block_count != 0u && !method->block_artifacts)
            || (root_total != 0u && !method->block_root_maps)
            || (capture_total != 0u && !method->block_captures))
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    method->block_root_map_count = root_total;
    method->block_capture_count = capture_total;
    for (block_index = 0u; block_index < lowering->block_count; block_index++) {
        const st_lower_block_artifact_t *source =
            &lowering->blocks[block_index];
        st_image_aot_block_artifact_t *target =
            &method->block_artifacts[block_index];
        size_t map_index;
        st_aot_compile_status_t status;
        if (!source->function
                || ((!source->captures) != (source->capture_count == 0u))
                || ((!source->root_maps) != (source->root_map_count == 0u))
                || !source->code_symbol.bytes
                || !source->descriptor_symbol.bytes
                || !source->method_descriptor_symbol.bytes)
            return ST_AOT_COMPILE_ERR_LOWERING;
        status = insert_symbol(builder, source->code_symbol.bytes,
                               source->code_symbol.length);
        if (status == ST_AOT_COMPILE_OK)
            status = insert_symbol(builder, source->descriptor_symbol.bytes,
                                   source->descriptor_symbol.length);
        if (status == ST_AOT_COMPILE_OK)
            status = insert_symbol(builder,
                source->method_descriptor_symbol.bytes,
                source->method_descriptor_symbol.length);
        if (status != ST_AOT_COMPILE_OK) return status;
        target->lexical_ordinal = source->lexical_ordinal;
        target->arity = source->arity;
        target->flags = source->flags;
        target->method_flags = source->method_flags;
        target->frame_root_capacity = source->required_root_capacity;
        target->code_symbol = source->code_symbol.bytes;
        target->code_symbol_length = source->code_symbol.length;
        target->descriptor_symbol = source->descriptor_symbol.bytes;
        target->descriptor_symbol_length = source->descriptor_symbol.length;
        target->method_descriptor_symbol =
            source->method_descriptor_symbol.bytes;
        target->method_descriptor_symbol_length =
            source->method_descriptor_symbol.length;
        target->captures = source->capture_count != 0u
            ? &method->block_captures[capture_cursor] : NULL;
        target->capture_count = source->capture_count;
        if (source->capture_count != 0u)
            memcpy(&method->block_captures[capture_cursor], source->captures,
                   source->capture_count * sizeof(*source->captures));
        capture_cursor += source->capture_count;
        target->root_maps = source->root_map_count != 0u
            ? &method->block_root_maps[root_cursor] : NULL;
        target->root_map_count = source->root_map_count;
        for (map_index = 0u; map_index < source->root_map_count; map_index++) {
            const st_lower_root_map_t *source_map =
                &source->root_maps[map_index];
            st_image_root_map_metadata_t *target_map =
                &method->block_root_maps[root_cursor++];
            target_map->safepoint_id = source_map->safepoint_id;
            target_map->root_count = source_map->root_count;
            target_map->bitmap_word_count = source_map->bitmap_word_count;
            target_map->live_root_bitmap = source_map->live_root_bitmap;
        }
    }
    if (root_cursor != root_total || capture_cursor != capture_total)
        return ST_AOT_COMPILE_ERR_LOWERING;
    return ST_AOT_COMPILE_OK;
}

static st_aot_compile_status_t lower_methods(
    aot_builder_t *builder, const st_aot_compile_options_t *options)
{
    const st_class_graph_result_t *graph = options->graph;
    st_aot_compile_status_t overall = ST_AOT_COMPILE_OK;
    size_t index;
    uint32_t literal_cursor = 0u;
    if (builder->global_count != 0u) {
        builder->lower_globals = allocate_array(builder->allocator,
            builder->global_count, sizeof(*builder->lower_globals));
        if (!builder->lower_globals) return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        for (index = 0u; index < builder->global_count; index++) {
            builder->lower_globals[index].semantic_external_id =
                builder->globals[index].semantic_external_id;
            builder->lower_globals[index].runtime_index =
                builder->globals[index].runtime_index;
        }
    }
    builder->methods = allocate_array(builder->allocator, graph->method_count,
                                      sizeof(*builder->methods));
    if (graph->method_count != 0u && builder->methods == NULL) {
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    }
    if (builder->methods != NULL)
        memset(builder->methods, 0,
               graph->method_count * sizeof(*builder->methods));
    builder->method_count = graph->method_count;
    if (!table_capacity(graph->method_count, &builder->symbol_capacity))
        return ST_AOT_COMPILE_ERR_OVERFLOW;
    builder->symbol_slots = allocate_array(builder->allocator,
        builder->symbol_capacity, sizeof(*builder->symbol_slots));
    if (builder->symbol_slots == NULL)
        return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
    memset(builder->symbol_slots, 0,
           builder->symbol_capacity * sizeof(*builder->symbol_slots));

    for (index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *graph_method = &graph->methods[index];
        st_aot_method_result_t *method = &builder->methods[index];
        st_lower_options_t lower_options;
        st_aot_compile_status_t status;
        st_aot_diagnostic_t *diagnostic;
        method->method_id = graph_method->id;
        method->owner = graph_method->owner;
        method->arity = (uint32_t)graph_method->node->as.method.arguments.count;
        st_lower_result_init(&method->lowering);
        if (!copy_bytes(builder, graph_method->selector.data,
                        graph_method->selector.length, &method->selector))
            return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        method->selector_length = graph_method->selector.length;
        status = make_method_symbol(builder, options, graph_method,
                                    &method->symbol, &method->symbol_length);
        if (status != ST_AOT_COMPILE_OK) return status;
        status = insert_symbol(builder, method->symbol, method->symbol_length);
        if (status != ST_AOT_COMPILE_OK) {
            if (!append_c_string_diagnostic(builder, ST_AOT_DIAG_INPUT,
                    graph_method, graph_method->node,
                    "generated method symbol collision", NULL))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            return status;
        }
        memset(&lower_options, 0, sizeof(lower_options));
        lower_options.symbol_name = method->symbol;
        lower_options.linkage = ANVIL_LINK_EXTERNAL;
        lower_options.selectors = options->selectors;
        lower_options.primitive_binding = binding_for_method(
            builder, graph_method->node);
        lower_options.globals = builder->lower_globals;
        lower_options.global_count = builder->global_count;
        lower_options.runtime_class_ids_by_entity =
            builder->layout.entity_runtime_class_ids;
        lower_options.runtime_class_id_count = builder->layout.entity_count;
        lower_options.literal_base_index = literal_cursor;
        lower_options.allocator = (st_lower_allocator_t){
            builder->allocator.allocate, builder->allocator.deallocate,
            builder->allocator.user
        };
        if (st_lower_method(&method->lowering, builder->context, graph,
                graph_method->id, &builder->semas[index], &lower_options)
                != ST_LOWER_OK) {
            const st_lower_diagnostic_t *source =
                &method->lowering.diagnostic;
            const char *detail = source->detail.data != NULL
                ? source->detail.data
                : st_lower_diagnostic_string(source->code);
            size_t detail_length = source->detail.data != NULL
                ? source->detail.length : strlen(detail);
            if (!append_diagnostic(builder, ST_AOT_DIAG_LOWERING,
                    graph_method, NULL, detail, detail_length, &diagnostic))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            diagnostic->lower_status = method->lowering.status;
            diagnostic->lower_code = source->code;
            diagnostic->has_span = source->has_span;
            if (source->has_span) diagnostic->span = source->span;
            if (method->lowering.status == ST_LOWER_ERR_OUT_OF_MEMORY)
                overall = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            if (method->lowering.status == ST_LOWER_ERR_OVERFLOW)
                overall = ST_AOT_COMPILE_ERR_OVERFLOW;
            if (overall == ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
                    || overall == ST_AOT_COMPILE_ERR_OVERFLOW)
                break;
            overall = ST_AOT_COMPILE_ERR_LOWERING;
            continue;
        }
        if (method->lowering.string_literal_count > UINT32_MAX
                || method->lowering.string_literal_count
                    > UINT32_MAX - literal_cursor) {
            overall = ST_AOT_COMPILE_ERR_OVERFLOW;
            break;
        }
        literal_cursor += (uint32_t)method->lowering.string_literal_count;
        status = adapt_root_maps(builder, method);
        if (status != ST_AOT_COMPILE_OK) return status;
        status = adapt_string_literals(builder, method);
        if (status != ST_AOT_COMPILE_OK) return status;
        status = adapt_blocks(builder, method);
        if (status != ST_AOT_COMPILE_OK) {
            if (status == ST_AOT_COMPILE_ERR_INVALID_ARGUMENT
                    && !append_c_string_diagnostic(builder,
                        ST_AOT_DIAG_LOWERING, graph_method,
                        graph_method->node,
                        "lowering published colliding block symbols", NULL))
                return ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            return status;
        }
        method->artifact.method_id = graph_method->id;
        method->artifact.owner = method->owner;
        method->artifact.selector = method->selector;
        method->artifact.selector_length = method->selector_length;
        method->artifact.arity = method->arity;
        method->artifact.symbol = method->symbol;
        method->artifact.symbol_length = method->symbol_length;
        method->artifact.frame_root_capacity =
            method->lowering.required_root_capacity;
        method->artifact.flags = method->lowering.method_flags
            | (method->lowering.has_primitive ? ST_METHOD_PRIMITIVE : 0u);
        method->artifact.root_maps = method->root_maps;
        method->artifact.root_map_count = method->lowering.root_map_count;
        method->artifact.blocks = method->block_artifacts;
        method->artifact.block_count = method->lowering.block_count;
        method->artifact.string_literals = method->string_literals;
        method->artifact.string_literal_count =
            method->lowering.string_literal_count;
    }
    builder->string_literal_count = literal_cursor;
    return overall;
}

static void builder_destroy(aot_builder_t *builder, bool destroy_modules)
{
    size_t index;
    if (destroy_modules) {
        st_image_emit_result_destroy(&builder->metadata);
        st_image_layout_result_destroy(&builder->layout);
        for (index = 0u; index < builder->method_count; index++)
            method_destroy(builder->allocator, &builder->methods[index]);
        if (builder->context != NULL) anvil_ctx_destroy(builder->context);
    }
    for (index = 0u; index < builder->sema_count; index++)
        st_sema_result_destroy(&builder->semas[index]);
    release(builder->allocator, builder->semas);
    release(builder->allocator, builder->methods);
    release(builder->allocator, builder->binding_slots);
    release(builder->allocator, builder->symbol_slots);
    release(builder->allocator, builder->lower_globals);
    release(builder->allocator, builder->external_name_slots);
    release(builder->allocator, builder->external_seen);
    for (index = 0u; index < builder->global_count; index++)
        release(builder->allocator, builder->globals[index].name);
    release(builder->allocator, builder->globals);
    if (destroy_modules) {
        for (index = 0u; index < builder->diagnostic_count; index++)
            diagnostic_destroy(builder->allocator,
                               &builder->diagnostics[index]);
        release(builder->allocator, builder->diagnostics);
    }
    memset(builder, 0, sizeof(*builder));
}

static void publish_failure(st_aot_compile_result_t *result,
                            aot_builder_t *builder,
                            aot_result_impl_t *implementation,
                            st_aot_compile_status_t status)
{
    st_aot_diagnostic_t *diagnostics = builder->diagnostics;
    size_t diagnostic_count = builder->diagnostic_count;
    builder->diagnostics = NULL;
    builder->diagnostic_count = 0u;
    builder->diagnostic_capacity = 0u;
    builder_destroy(builder, true);
    result->status = status;
    result->diagnostics = diagnostics;
    result->diagnostic_count = diagnostic_count;
    result->implementation = implementation;
}

st_aot_compile_status_t st_aot_compile(
    st_aot_compile_result_t *result,
    const st_aot_compile_options_t *options)
{
    aot_builder_t builder;
    aot_result_impl_t *implementation;
    st_aot_allocator_t allocator = { default_allocate, default_deallocate,
                                     NULL };
    st_aot_compile_status_t status, primitive_status, semantic_status;
    anvil_abi_t effective_abi;
    anvil_syntax_t effective_syntax;
    st_image_aot_method_artifact_t *artifacts = NULL;
    st_image_global_artifact_t *global_artifacts = NULL;
    st_image_emit_options_t image_options;
    char *provenance_prefix = NULL;
    size_t index;
    _Static_assert(ST_IMAGE_METADATA_ABI_VERSION == UINT32_C(5),
                   "AOT driver requires image metadata ABI v5");

    if (!result_is_empty(result)) {
        if (result != NULL) result->status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        return ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
    }
    if (options == NULL || options->bundle == NULL || options->graph == NULL
            || options->selectors == NULL || options->primitives == NULL
            || !st_class_graph_succeeded(options->graph)
            || !st_selector_table_is_frozen(options->selectors)
            || (unsigned)options->target >= (unsigned)ANVIL_ARCH_COUNT
            || (unsigned)options->abi > (unsigned)ANVIL_ABI_MVS
            || (unsigned)options->syntax > (unsigned)ANVIL_SYNTAX_GAS
            || (unsigned)options->optimization
                > (unsigned)ANVIL_OPT_AGGRESSIVE
            || !portable_prefix(options->symbol_prefix,
                                options->symbol_prefix_length)
            || ((options->allocator.allocate == NULL)
                != (options->allocator.deallocate == NULL))) {
        result->status = ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    if (options->allocator.allocate != NULL) allocator = options->allocator;
    implementation = allocator.allocate(allocator.user,
                                         sizeof(*implementation));
    if (implementation == NULL) {
        result->status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    implementation->allocator = allocator;
    memset(&builder, 0, sizeof(builder));
    builder.allocator = allocator;
    st_image_emit_result_init(&builder.metadata);
    st_image_layout_result_init(&builder.layout);

    if (!target_configuration(options->target, options->abi,
                              options->syntax, &effective_abi,
                              &effective_syntax)) {
        status = append_c_string_diagnostic(&builder, ST_AOT_DIAG_INPUT,
                NULL, NULL,
                "requested ABI/assembly syntax is incompatible with target",
                NULL)
            ? ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET
            : ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }

    builder.context = anvil_ctx_create_for_target(options->target);
    if (builder.context == NULL) {
        publish_failure(result, &builder, implementation,
                        ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
        return result->status;
    }
    {
        const anvil_data_layout_t *layout =
            anvil_ctx_get_data_layout(builder.context);
        const anvil_arch_info_t *arch =
            anvil_ctx_get_arch_info(builder.context);
        if (!anvil_ctx_has_target(builder.context) || layout == NULL
                || arch == NULL || layout->pointer.size != sizeof(uint64_t)
                || arch->ptr_size != (int)sizeof(uint64_t)) {
            status = append_c_string_diagnostic(
                    &builder, ST_AOT_DIAG_INPUT, NULL, NULL,
                    "Smalltalk tagged64 AOT ABI requires a 64-bit target; "
                    "32/31/24-bit targets require a future target-specific "
                    "tagged representation", NULL)
                ? ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET
                : ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
            publish_failure(result, &builder, implementation,
                            status);
            return result->status;
        }
    }
    if (anvil_ctx_set_abi(builder.context, effective_abi) != ANVIL_OK
            || anvil_ctx_set_syntax(builder.context, effective_syntax)
                != ANVIL_OK) {
        status = anvil_ctx_get_last_error(builder.context) == ANVIL_ERR_NOMEM
            ? ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
            : ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET;
        if (status == ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET
                && !append_c_string_diagnostic(&builder, ST_AOT_DIAG_INPUT,
                    NULL, NULL,
                    "requested ABI/assembly syntax is incompatible with target",
                    NULL))
            status = ST_AOT_COMPILE_ERR_OUT_OF_MEMORY;
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }
    if (anvil_ctx_set_opt_level(builder.context, options->optimization)
            != ANVIL_OK) {
        status = anvil_ctx_get_last_error(builder.context) == ANVIL_ERR_NOMEM
            ? ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
            : ST_AOT_COMPILE_ERR_INVALID_ARGUMENT;
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }

    {
        st_image_layout_options_t layout_options = {
            .allocator = {
                allocator.allocate, allocator.deallocate, allocator.user
            }
        };
        st_image_layout_status_t layout_status = st_image_layout_build(
            &builder.layout, options->graph, &layout_options);
        if (layout_status != ST_IMAGE_LAYOUT_OK) {
            st_aot_diagnostic_t *diagnostic;
            if (!append_c_string_diagnostic(
                    &builder, ST_AOT_DIAG_LAYOUT, NULL,
                    builder.layout.diagnostic.pragma,
                    st_image_layout_status_string(layout_status),
                    &diagnostic)) {
                publish_failure(result, &builder, implementation,
                                ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
                return result->status;
            }
            diagnostic->layout_status = layout_status;
            status = layout_status == ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY
                ? ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
                : layout_status == ST_IMAGE_LAYOUT_ERR_OVERFLOW
                    ? ST_AOT_COMPILE_ERR_OVERFLOW
                    : ST_AOT_COMPILE_ERR_LAYOUT;
            publish_failure(result, &builder, implementation, status);
            return result->status;
        }
    }

    status = build_global_table(&builder, options);
    if (status != ST_AOT_COMPILE_OK) {
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }

    status = build_binding_index(&builder, options);
    if (status != ST_AOT_COMPILE_OK) {
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }
    primitive_status = collect_primitive_diagnostics(&builder, options);
    if (primitive_status == ST_AOT_COMPILE_ERR_OUT_OF_MEMORY) {
        publish_failure(result, &builder, implementation, primitive_status);
        return result->status;
    }
    semantic_status = analyze_methods(&builder, options);
    if (semantic_status == ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
            || semantic_status == ST_AOT_COMPILE_ERR_OVERFLOW) {
        publish_failure(result, &builder, implementation, semantic_status);
        return result->status;
    }
    if (primitive_status != ST_AOT_COMPILE_OK
            || semantic_status != ST_AOT_COMPILE_OK) {
        status = primitive_status != ST_AOT_COMPILE_OK
            ? primitive_status : semantic_status;
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }

    status = lower_methods(&builder, options);
    if (status != ST_AOT_COMPILE_OK) {
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }
    artifacts = allocate_array(allocator, builder.method_count,
                               sizeof(*artifacts));
    if (builder.method_count != 0u && artifacts == NULL) {
        publish_failure(result, &builder, implementation,
                        ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
        return result->status;
    }
    for (index = 0u; index < builder.method_count; index++)
        artifacts[index] = builder.methods[index].artifact;
    global_artifacts = allocate_array(allocator, builder.global_count,
                                      sizeof(*global_artifacts));
    if (builder.global_count != 0u && !global_artifacts) {
        release(allocator, artifacts);
        publish_failure(result, &builder, implementation,
                        ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
        return result->status;
    }
    for (index = 0u; index < builder.global_count; index++) {
        global_artifacts[index].semantic_external_id =
            builder.globals[index].semantic_external_id;
        global_artifacts[index].runtime_index =
            builder.globals[index].runtime_index;
        global_artifacts[index].name = builder.globals[index].name;
        global_artifacts[index].name_length =
            builder.globals[index].name_length;
    }
    memset(&image_options, 0, sizeof(image_options));
    image_options.allocator = (st_image_emit_allocator_t){
        allocator.allocate, allocator.deallocate, allocator.user
    };
    image_options.module_name = "smalltalk.aot.metadata";
    image_options.symbol_prefix = options->symbol_prefix;
    image_options.selectors = options->selectors;
    image_options.method_artifacts = artifacts;
    image_options.method_artifact_count = builder.method_count;
    image_options.globals = global_artifacts;
    image_options.global_count = builder.global_count;
    image_options.layout = &builder.layout;
    image_options.require_method_code = true;
    if (st_image_emit_metadata(&builder.metadata, builder.context,
            options->bundle, options->graph, &image_options)
            != ST_IMAGE_EMIT_OK) {
        st_image_emit_status_t image_status = builder.metadata.status;
        st_aot_diagnostic_t *diagnostic;
        release(allocator, artifacts);
        release(allocator, global_artifacts);
        if (!append_c_string_diagnostic(&builder, ST_AOT_DIAG_METADATA,
                NULL, NULL, st_image_emit_status_string(image_status),
                &diagnostic)) {
            publish_failure(result, &builder, implementation,
                            ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
            return result->status;
        }
        diagnostic->image_status = image_status;
        status = image_status == ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY
            ? ST_AOT_COMPILE_ERR_OUT_OF_MEMORY
            : image_status == ST_IMAGE_EMIT_ERR_OVERFLOW
                ? ST_AOT_COMPILE_ERR_OVERFLOW
                : ST_AOT_COMPILE_ERR_METADATA;
        publish_failure(result, &builder, implementation, status);
        return result->status;
    }
    release(allocator, artifacts);
    release(allocator, global_artifacts);

    if (!copy_bytes(&builder, options->symbol_prefix,
                    options->symbol_prefix_length, &provenance_prefix)) {
        publish_failure(result, &builder, implementation,
                        ST_AOT_COMPILE_ERR_OUT_OF_MEMORY);
        return result->status;
    }

    /* Publish every owned component in one commit. */
    result->status = ST_AOT_COMPILE_OK;
    result->provenance.target = anvil_ctx_get_target(builder.context);
    result->provenance.abi = anvil_ctx_get_abi(builder.context);
    result->provenance.syntax = effective_syntax;
    result->provenance.optimization =
        anvil_ctx_get_opt_level(builder.context);
    result->provenance.symbol_prefix = provenance_prefix;
    result->provenance.symbol_prefix_length = options->symbol_prefix_length;
    result->context = builder.context;
    result->methods = builder.methods;
    result->method_count = builder.method_count;
    result->globals = builder.globals;
    result->global_count = builder.global_count;
    result->string_literal_count = builder.string_literal_count;
    result->layout = builder.layout;
    result->metadata = builder.metadata;
    result->diagnostics = builder.diagnostics;
    result->diagnostic_count = builder.diagnostic_count;
    result->implementation = implementation;
    builder.context = NULL;
    builder.methods = NULL;
    builder.method_count = 0u;
    builder.globals = NULL;
    builder.global_count = 0u;
    st_image_layout_result_init(&builder.layout);
    st_image_emit_result_init(&builder.metadata);
    builder.diagnostics = NULL;
    builder.diagnostic_count = 0u;
    builder.diagnostic_capacity = 0u;
    builder_destroy(&builder, false);
    return result->status;
}

const char *st_aot_compile_status_string(st_aot_compile_status_t status)
{
    switch (status) {
    case ST_AOT_COMPILE_OK: return "ok";
    case ST_AOT_COMPILE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_AOT_COMPILE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_AOT_COMPILE_ERR_OVERFLOW: return "overflow";
    case ST_AOT_COMPILE_ERR_UNSUPPORTED_TARGET: return "unsupported target";
    case ST_AOT_COMPILE_ERR_PRIMITIVES: return "primitive binding failed";
    case ST_AOT_COMPILE_ERR_SEMANTIC: return "semantic analysis failed";
    case ST_AOT_COMPILE_ERR_LAYOUT: return "image layout failed";
    case ST_AOT_COMPILE_ERR_LOWERING: return "method lowering failed";
    case ST_AOT_COMPILE_ERR_METADATA: return "metadata emission failed";
    }
    return "unknown AOT compilation status";
}

const char *st_aot_diagnostic_stage_string(st_aot_diagnostic_stage_t stage)
{
    switch (stage) {
    case ST_AOT_DIAG_INPUT: return "input";
    case ST_AOT_DIAG_PRIMITIVE: return "primitive";
    case ST_AOT_DIAG_SEMA_VIEW: return "semantic view";
    case ST_AOT_DIAG_SEMANTIC: return "semantic analysis";
    case ST_AOT_DIAG_LAYOUT: return "image layout";
    case ST_AOT_DIAG_LOWERING: return "lowering";
    case ST_AOT_DIAG_METADATA: return "metadata";
    }
    return "unknown AOT diagnostic stage";
}
