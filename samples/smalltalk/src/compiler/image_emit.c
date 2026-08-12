#include "st_image_emit.h"

#include "st_selector.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *bytes;
    size_t length;
    size_t offset;
    uint64_t hash;
} string_record_t;

typedef struct {
    st_image_emit_allocator_t allocator;
    string_record_t *records;
    size_t count;
    size_t capacity;
    size_t *buckets; /* record index plus one; zero is empty */
    size_t bucket_capacity;
    size_t byte_count;
    bool overflow;
} string_pool_t;

typedef struct {
    const st_image_aot_block_artifact_t *artifact;
    size_t method_index;
    size_t root_map_offset;
    size_t capture_offset;
} prepared_block_t;

typedef struct {
    const char *bytes;
    size_t length;
} symbol_ref_t;

static int symbol_ref_compare(const void *left, const void *right)
{
    const symbol_ref_t *a = left;
    const symbol_ref_t *b = right;
    size_t common = a->length < b->length ? a->length : b->length;
    int order = common != 0u ? memcmp(a->bytes, b->bytes, common) : 0;
    if (order != 0) return order;
    return a->length < b->length ? -1 : a->length != b->length;
}

typedef struct {
    st_image_emit_allocator_t allocator;
    anvil_ctx_t *context;
    const st_source_bundle_t *bundle;
    const st_class_graph_result_t *graph;
    const st_selector_table_t *selectors;
    string_pool_t strings;
    st_selector_id_t *method_selector_ids;
    size_t *method_order;
    uint32_t *method_offsets;
    uint32_t *method_counts;
    const st_image_aot_method_artifact_t **method_artifacts;
    char **artifact_symbol_names;
    anvil_value_t **method_code_externs;
    size_t *root_map_offsets;
    size_t root_map_count;
    size_t root_bitmap_word_count;
    size_t method_root_map_count;
    size_t block_root_map_count;
    prepared_block_t *blocks;
    size_t block_count;
    size_t block_capture_count;
    const st_image_global_artifact_t *globals;
    size_t global_count;
    size_t string_literal_count;
    size_t string_literal_bytes;
    const st_image_layout_result_t *layout;
    st_image_layout_result_t owned_layout;
    char *symbol_names[26];
    bool has_method_code;
    bool allocation_failed;
    anvil_module_t *module;
} emit_builder_t;

enum {
    SYMBOL_STRINGS,
    SYMBOL_SELECTORS,
    SYMBOL_INSTANCE_SLOTS,
    SYMBOL_CLASS_VARIABLES,
    SYMBOL_ROOT_BITMAPS,
    SYMBOL_ROOT_MAPS,
    SYMBOL_METHODS,
    SYMBOL_ENTITIES,
    SYMBOL_DESCRIPTOR,
    SYMBOL_RUNTIME_METHODS,
    SYMBOL_BLOCK_CAPTURES,
    SYMBOL_BLOCK_DESCRIPTORS,
    SYMBOL_GLOBALS,
    SYMBOL_LITERAL_BYTES,
    SYMBOL_STRING_LITERALS,
    SYMBOL_RUNTIME_BINDINGS,
    SYMBOL_METHOD_ENTRIES,
    SYMBOL_METHOD_SLOTS,
    SYMBOL_SHAPE_BITMAPS,
    SYMBOL_CLASS_DESCRIPTORS,
    SYMBOL_SHAPE_DESCRIPTORS,
    SYMBOL_CLASS_POINTERS,
    SYMBOL_SHAPE_POINTERS,
    SYMBOL_RUNTIME_DESCRIPTORS,
    SYMBOL_ENTITY_RUNTIME_IDS,
    SYMBOL_RUNTIME_LAYOUTS,
    SYMBOL_COUNT
};

static anvil_value_t *target_size_constant(emit_builder_t *builder,
                                           size_t value);
static anvil_type_t *target_size_type(emit_builder_t *builder);
static anvil_value_t *table_index_address(anvil_ctx_t *context,
                                          anvil_value_t *global,
                                          anvil_type_t *array_type,
                                          size_t index);

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

static void release(st_image_emit_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) allocator.deallocate(allocator.user, pointer);
}

static void *allocate_array(st_image_emit_allocator_t allocator,
                            size_t count, size_t element_size)
{
    void *memory;
    if (count == 0u) return NULL;
    if (element_size == 0u || count > SIZE_MAX / element_size) return NULL;
    memory = allocator.allocate(allocator.user, count * element_size);
    if (memory != NULL) memset(memory, 0, count * element_size);
    return memory;
}

static uint64_t string_hash(const char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;
    for (index = 0u; index < length; index++) {
        hash ^= (unsigned char)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    hash *= UINT64_C(0xd6e8feb86659fd93);
    hash ^= hash >> 32;
    return hash;
}

static bool strings_equal(const string_record_t *record,
                          const char *bytes, size_t length, uint64_t hash)
{
    return record->hash == hash && record->length == length
        && (length == 0u || memcmp(record->bytes, bytes, length) == 0);
}

static bool string_pool_rehash(string_pool_t *pool, size_t new_capacity)
{
    size_t *replacement;
    size_t index;
    if (new_capacity < 16u || (new_capacity & (new_capacity - 1u)) != 0u)
        return false;
    replacement = allocate_array(pool->allocator, new_capacity,
                                 sizeof(*replacement));
    if (replacement == NULL) return false;
    for (index = 0u; index < pool->count; index++) {
        size_t bucket = (size_t)(pool->records[index].hash
                                 & (uint64_t)(new_capacity - 1u));
        while (replacement[bucket] != 0u)
            bucket = (bucket + 1u) & (new_capacity - 1u);
        replacement[bucket] = index + 1u;
    }
    release(pool->allocator, pool->buckets);
    pool->buckets = replacement;
    pool->bucket_capacity = new_capacity;
    return true;
}

static bool string_pool_reserve_records(string_pool_t *pool, size_t required)
{
    string_record_t *replacement;
    size_t capacity;
    if (required <= pool->capacity) return true;
    capacity = pool->capacity == 0u ? 16u : pool->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    replacement = allocate_array(pool->allocator, capacity,
                                 sizeof(*replacement));
    if (replacement == NULL) return false;
    if (pool->count != 0u)
        memcpy(replacement, pool->records,
               pool->count * sizeof(*replacement));
    release(pool->allocator, pool->records);
    pool->records = replacement;
    pool->capacity = capacity;
    return true;
}

static bool string_pool_intern(string_pool_t *pool, const char *bytes,
                               size_t length, size_t *offset_out)
{
    uint64_t hash;
    size_t bucket;
    size_t required_buckets;
    string_record_t *record;
    if (pool == NULL || bytes == NULL || length == 0u || offset_out == NULL)
        return false;
    hash = string_hash(bytes, length);
    if (pool->bucket_capacity == 0u && !string_pool_rehash(pool, 16u))
        return false;
    required_buckets = pool->bucket_capacity;
    if (pool->count + 1u > required_buckets - required_buckets / 4u) {
        if (required_buckets > SIZE_MAX / 2u) return false;
        required_buckets *= 2u;
        if (!string_pool_rehash(pool, required_buckets)) return false;
    }
    bucket = (size_t)(hash & (uint64_t)(pool->bucket_capacity - 1u));
    while (pool->buckets[bucket] != 0u) {
        record = &pool->records[pool->buckets[bucket] - 1u];
        if (strings_equal(record, bytes, length, hash)) {
            *offset_out = record->offset;
            return true;
        }
        bucket = (bucket + 1u) & (pool->bucket_capacity - 1u);
    }
    if (pool->count == SIZE_MAX || length == SIZE_MAX
            || pool->byte_count > SIZE_MAX - length - 1u) {
        pool->overflow = true;
        return false;
    }
    if (!string_pool_reserve_records(pool, pool->count + 1u))
        return false;
    record = &pool->records[pool->count];
    record->bytes = bytes;
    record->length = length;
    record->offset = pool->byte_count;
    record->hash = hash;
    pool->buckets[bucket] = pool->count + 1u;
    pool->count++;
    pool->byte_count += length + 1u;
    *offset_out = record->offset;
    return true;
}

static bool string_pool_lookup(const string_pool_t *pool, const char *bytes,
                               size_t length, size_t *offset_out)
{
    uint64_t hash;
    size_t bucket;
    if (pool == NULL || bytes == NULL || length == 0u || offset_out == NULL
            || pool->bucket_capacity == 0u) return false;
    hash = string_hash(bytes, length);
    bucket = (size_t)(hash & (uint64_t)(pool->bucket_capacity - 1u));
    while (pool->buckets[bucket] != 0u) {
        const string_record_t *record =
            &pool->records[pool->buckets[bucket] - 1u];
        if (strings_equal(record, bytes, length, hash)) {
            *offset_out = record->offset;
            return true;
        }
        bucket = (bucket + 1u) & (pool->bucket_capacity - 1u);
    }
    return false;
}

static void string_pool_destroy(string_pool_t *pool)
{
    if (pool == NULL) return;
    release(pool->allocator, pool->records);
    release(pool->allocator, pool->buckets);
    memset(pool, 0, sizeof(*pool));
}

static bool ast_string_equal(st_ast_string_t left, st_ast_string_t right)
{
    return left.length == right.length
        && (left.length == 0u
            || memcmp(left.data, right.data, left.length) == 0);
}

static bool ast_string_valid(st_ast_string_t string)
{
    return string.data != NULL && string.length != 0u;
}

static bool id_valid(st_class_graph_id_t id, size_t count, bool allow_zero)
{
    return (allow_zero && id == ST_CLASS_GRAPH_INVALID_ID)
        || (id != ST_CLASS_GRAPH_INVALID_ID && (size_t)id <= count);
}

static st_image_emit_status_t validate_inputs(
    anvil_ctx_t *context, const st_source_bundle_t *bundle,
    const st_class_graph_result_t *graph,
    const st_image_emit_options_t *options)
{
    const anvil_arch_info_t *arch;
    size_t index;
    if (context == NULL || bundle == NULL || graph == NULL || options == NULL
            || !st_selector_table_is_frozen(options->selectors))
        return ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT;
    if (!anvil_ctx_has_target(context))
        return ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET;
    arch = anvil_ctx_get_arch_info(context);
    if (arch == NULL || (arch->ptr_size != 4 && arch->ptr_size != 8))
        return ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET;
    if ((options != NULL)
            && ((options->allocator.allocate == NULL)
                != (options->allocator.deallocate == NULL)))
        return ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT;
    if (options != NULL &&
        ((options->method_artifacts == NULL) !=
         (options->method_artifact_count == 0u)))
        return ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT;
    if (bundle->count == 0u || bundle->files == NULL
            || bundle->image_count == 0u
            || bundle->image_count > bundle->count
            || bundle->diagnostic.status != ST_SOURCE_LOAD_OK)
        return ST_IMAGE_EMIT_ERR_INVALID_BUNDLE;
    for (index = 0u; index < bundle->count; index++) {
        const st_source_file_t *file = &bundle->files[index];
        st_source_origin_t expected = index < bundle->image_count
            ? ST_SOURCE_ORIGIN_IMAGE : ST_SOURCE_ORIGIN_APPLICATION;
        if (file->origin != expected || file->ordinal != index
                || file->source_name == NULL || file->path == NULL
                || file->source == NULL
                || file->ast.source_name.data == NULL
                || file->ast.source_name.length != strlen(file->source_name)
                || memcmp(file->ast.source_name.data, file->source_name,
                          file->ast.source_name.length) != 0)
            return ST_IMAGE_EMIT_ERR_INVALID_BUNDLE;
    }
    if (!st_class_graph_succeeded(graph) || graph->entity_count == 0u
            || graph->entities == NULL
            || (graph->method_count != 0u && graph->methods == NULL)
            || (graph->instance_slot_count != 0u
                && graph->instance_slots == NULL)
            || (graph->class_variable_count != 0u
                && graph->class_variables == NULL))
        return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    if (bundle->count > UINT32_MAX || bundle->image_count > UINT32_MAX
            || graph->entity_count > UINT32_MAX
            || graph->method_count > UINT32_MAX
            || graph->instance_slot_count > UINT32_MAX
            || graph->class_variable_count > UINT32_MAX)
        return ST_IMAGE_EMIT_ERR_OVERFLOW;
    if (options != NULL && options->method_artifact_count != 0u &&
        options->method_artifact_count != graph->method_count)
        return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
    if (options != NULL
            && ((!options->globals) != (options->global_count == 0u)))
        return ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT;
    if (options != NULL && options->require_method_code &&
        graph->method_count != 0u && options->method_artifact_count == 0u)
        return ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE;
    for (index = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];
        if (entity->id != (st_class_graph_id_t)(index + 1u)
                || entity->kind > ST_CLASS_GRAPH_NAMESPACE
                || !ast_string_valid(entity->name)
                || entity->origin.unit_index >= bundle->count
                || !ast_string_equal(entity->origin.source_name,
                    bundle->files[entity->origin.unit_index].ast.source_name)
                || entity->origin.span.begin.line > UINT32_MAX
                || entity->origin.span.begin.column > UINT32_MAX
                || !id_valid(entity->namespace_id, graph->entity_count, true)
                || !id_valid(entity->superclass_id, graph->entity_count, true)
                || !id_valid(entity->metaclass_id, graph->entity_count, true)
                || !id_valid(entity->instance_class_id,
                             graph->entity_count, true)
                || entity->instance_slot_offset > graph->instance_slot_count
                || entity->instance_slot_count
                    > graph->instance_slot_count - entity->instance_slot_offset
                || entity->class_variable_offset > graph->class_variable_count
                || entity->class_variable_count
                    > graph->class_variable_count - entity->class_variable_offset
                || entity->own_method_count > graph->method_count)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        if (entity->kind == ST_CLASS_GRAPH_CLASS) {
            const st_class_graph_entity_t *meta;
            if (!id_valid(entity->metaclass_id, graph->entity_count, false))
                return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
            meta = &graph->entities[entity->metaclass_id - 1u];
            if (entity->instance_class_id != ST_CLASS_GRAPH_INVALID_ID
                    || meta->kind != ST_CLASS_GRAPH_METACLASS
                    || meta->instance_class_id != entity->id
                    || (entity->superclass_id != ST_CLASS_GRAPH_INVALID_ID
                        && graph->entities[entity->superclass_id - 1u].kind
                            != ST_CLASS_GRAPH_CLASS))
                return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        } else if (entity->kind == ST_CLASS_GRAPH_METACLASS) {
            if (!id_valid(entity->instance_class_id,
                          graph->entity_count, false)
                    || graph->entities[entity->instance_class_id - 1u].kind
                        != ST_CLASS_GRAPH_CLASS
                    || graph->entities[entity->instance_class_id - 1u]
                           .metaclass_id != entity->id
                    || entity->metaclass_id != ST_CLASS_GRAPH_INVALID_ID
                    || (entity->superclass_id != ST_CLASS_GRAPH_INVALID_ID
                        && graph->entities[entity->superclass_id - 1u].kind
                            != ST_CLASS_GRAPH_METACLASS))
                return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        } else if (entity->metaclass_id != ST_CLASS_GRAPH_INVALID_ID
                || entity->instance_class_id != ST_CLASS_GRAPH_INVALID_ID
                || entity->superclass_id != ST_CLASS_GRAPH_INVALID_ID) {
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        }
    }
    for (index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *method = &graph->methods[index];
        const st_class_graph_entity_t *instance;
        const st_class_graph_entity_t *owner;
        st_selector_id_t selector_id;
        if (method->id != (st_class_graph_method_id_t)(index + 1u)
                || method->node == NULL || method->node->kind != ST_AST_METHOD
                || !ast_string_valid(method->selector)
                || method->origin.unit_index >= bundle->count
                || !ast_string_equal(method->origin.source_name,
                    bundle->files[method->origin.unit_index].ast.source_name)
                || !id_valid(method->owner, graph->entity_count, false)
                || !id_valid(method->instance_class,
                             graph->entity_count, false)
                || !id_valid(method->lexical_super,
                             graph->entity_count, true)
                || method->node->as.method.arguments.count > UINT32_MAX
                || method->origin.span.begin.line > UINT32_MAX
                || method->origin.span.begin.column > UINT32_MAX)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        if (!st_selector_lookup(options->selectors,
                                method->selector.data,
                                method->selector.length,
                                &selector_id)
                || selector_id == ST_SELECTOR_INVALID_ID) {
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        }
        instance = &graph->entities[method->instance_class - 1u];
        owner = &graph->entities[method->owner - 1u];
        if (instance->kind != ST_CLASS_GRAPH_CLASS
                || !ast_string_equal(method->selector,
                                     method->node->as.method.selector)
                || method->class_side != method->node->as.method.class_side
                || (method->class_side
                    ? (method->owner != instance->metaclass_id
                       || owner->kind != ST_CLASS_GRAPH_METACLASS)
                    : (method->owner != instance->id
                       || owner->kind != ST_CLASS_GRAPH_CLASS))
                || method->lexical_super != owner->superclass_id)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    }
    for (index = 0u; index < graph->instance_slot_count; index++) {
        const st_class_graph_slot_t *slot = &graph->instance_slots[index];
        if (slot->kind != ST_CLASS_GRAPH_INSTANCE_SLOT
                || !ast_string_valid(slot->name)
                || (slot->has_type && !ast_string_valid(slot->type_name))
                || !id_valid(slot->declaring_class,
                             graph->entity_count, false)
                || slot->origin.unit_index >= bundle->count
                || !ast_string_equal(slot->origin.source_name,
                    bundle->files[slot->origin.unit_index].ast.source_name)
                || slot->origin.span.begin.line > UINT32_MAX
                || slot->origin.span.begin.column > UINT32_MAX)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    }
    for (index = 0u; index < graph->class_variable_count; index++) {
        const st_class_graph_slot_t *slot = &graph->class_variables[index];
        if (slot->kind != ST_CLASS_GRAPH_CLASS_VARIABLE
                || !ast_string_valid(slot->name)
                || (slot->has_type && !ast_string_valid(slot->type_name))
                || !id_valid(slot->declaring_class,
                             graph->entity_count, false)
                || slot->origin.unit_index >= bundle->count
                || !ast_string_equal(slot->origin.source_name,
                    bundle->files[slot->origin.unit_index].ast.source_name)
                || slot->origin.span.begin.line > UINT32_MAX
                || slot->origin.span.begin.column > UINT32_MAX)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    }
    return ST_IMAGE_EMIT_OK;
}

static bool intern_ast_string(emit_builder_t *builder, st_ast_string_t string)
{
    size_t ignored;
    return ast_string_valid(string)
        && string_pool_intern(&builder->strings, string.data, string.length,
                              &ignored);
}

static bool collect_strings_and_selectors(emit_builder_t *builder)
{
    const st_class_graph_result_t *graph = builder->graph;
    size_t index;
    for (index = 0u; index < graph->entity_count; index++)
        if (!intern_ast_string(builder, graph->entities[index].name))
            return false;
    for (index = 0u; index < st_selector_count(builder->selectors); index++) {
        const st_selector_t *selector = st_selector_get(
            builder->selectors, (st_selector_id_t)(index + 1u));
        size_t ignored;
        if (selector == NULL
                || !string_pool_intern(&builder->strings, selector->bytes,
                                       selector->length, &ignored)) {
            return false;
        }
    }
    for (index = 0u; index < graph->method_count; index++) {
        const st_class_graph_method_t *method = &graph->methods[index];
        st_selector_id_t selector_id;
        if (!intern_ast_string(builder, method->selector)
                || !intern_ast_string(builder, method->origin.source_name)
                || !st_selector_lookup(builder->selectors,
                       method->selector.data, method->selector.length,
                       &selector_id))
            return false;
        builder->method_selector_ids[index] = selector_id;
    }
    for (index = 0u; index < graph->instance_slot_count; index++) {
        const st_class_graph_slot_t *slot = &graph->instance_slots[index];
        if (!intern_ast_string(builder, slot->name)
                || (slot->has_type && !intern_ast_string(builder,
                                                         slot->type_name)))
            return false;
    }
    for (index = 0u; index < graph->class_variable_count; index++) {
        const st_class_graph_slot_t *slot = &graph->class_variables[index];
        if (!intern_ast_string(builder, slot->name)
                || (slot->has_type && !intern_ast_string(builder,
                                                         slot->type_name)))
            return false;
    }
    for (index = 0u; index < builder->global_count; index++) {
        size_t ignored;
        const st_image_global_artifact_t *global = &builder->globals[index];
        if (!string_pool_intern(&builder->strings, global->name,
                                global->name_length, &ignored))
            return false;
    }
    if (builder->strings.byte_count > UINT32_MAX) {
        builder->strings.overflow = true;
        return false;
    }
    return true;
}

static bool build_method_order(emit_builder_t *builder)
{
    const st_class_graph_result_t *graph = builder->graph;
    size_t index;
    size_t cursor = 0u;
    for (index = 0u; index < graph->method_count; index++) {
        st_class_graph_id_t owner = graph->methods[index].owner;
        if (builder->method_counts[owner - 1u] == UINT32_MAX) return false;
        builder->method_counts[owner - 1u]++;
    }
    for (index = 0u; index < graph->entity_count; index++) {
        builder->method_offsets[index] = (uint32_t)cursor;
        cursor += builder->method_counts[index];
        builder->method_counts[index] = 0u;
    }
    for (index = 0u; index < graph->method_count; index++) {
        size_t owner_index = (size_t)graph->methods[index].owner - 1u;
        size_t destination = builder->method_offsets[owner_index]
            + builder->method_counts[owner_index]++;
        builder->method_order[destination] = index;
    }
    if (cursor != graph->method_count) return false;
    for (index = 0u; index < graph->entity_count; index++)
        if (builder->method_counts[index]
                != graph->entities[index].own_method_count) return false;
    for (index = 0u; index < graph->entity_count; index++) {
        size_t begin = builder->method_offsets[index];
        size_t count = builder->method_counts[index];
        for (size_t item = 1u; item < count; item++) {
            size_t method_index = builder->method_order[begin + item];
            st_selector_id_t selector =
                builder->method_selector_ids[method_index];
            size_t position = item;
            while (position != 0u
                    && builder->method_selector_ids[
                           builder->method_order[begin + position - 1u]]
                        > selector) {
                builder->method_order[begin + position] =
                    builder->method_order[begin + position - 1u];
                position--;
            }
            builder->method_order[begin + position] = method_index;
        }
    }
    return true;
}

static bool valid_symbol_prefix(const char *prefix)
{
    const unsigned char *cursor = (const unsigned char *)prefix;
    if (cursor == NULL || !((*cursor >= 'A' && *cursor <= 'Z')
            || (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_'))
        return false;
    for (cursor++; *cursor != '\0'; cursor++)
        if (!((*cursor >= 'A' && *cursor <= 'Z')
                || (*cursor >= 'a' && *cursor <= 'z')
                || (*cursor >= '0' && *cursor <= '9') || *cursor == '_'))
            return false;
    return true;
}

static bool valid_external_symbol(const char *symbol, size_t length)
{
    size_t index;
    unsigned char byte;
    if (!symbol || length == 0u) return false;
    byte = (unsigned char)symbol[0];
    if (!((byte >= 'A' && byte <= 'Z') ||
          (byte >= 'a' && byte <= 'z') || byte == '_'))
        return false;
    for (index = 1u; index < length; index++) {
        byte = (unsigned char)symbol[index];
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_'))
            return false;
    }
    return true;
}

static bool build_symbol_names(emit_builder_t *builder, const char *prefix)
{
    static const char *const suffixes[SYMBOL_COUNT] = {
        "strings", "selectors", "instance_slots", "class_variables",
        "root_bitmap_words", "root_maps", "methods", "entities",
        "descriptor", "runtime_methods", "block_captures",
        "block_descriptors", "globals", "literal_bytes", "string_literals",
        "runtime_bindings", "method_entries", "method_slots",
        "shape_pointer_bitmaps", "class_descriptors", "shape_descriptors",
        "class_descriptor_pointers", "shape_descriptor_pointers",
        "runtime_descriptors", "entity_runtime_class_ids", "runtime_layouts"
    };
    size_t prefix_length = strlen(prefix);
    size_t index;
    for (index = 0u; index < SYMBOL_COUNT; index++) {
        size_t suffix_length = strlen(suffixes[index]);
        size_t length;
        if (prefix_length > SIZE_MAX - suffix_length - 2u) return false;
        length = prefix_length + suffix_length + 2u;
        builder->symbol_names[index] = builder->allocator.allocate(
            builder->allocator.user, length);
        if (builder->symbol_names[index] == NULL) return false;
        memcpy(builder->symbol_names[index], prefix, prefix_length);
        builder->symbol_names[index][prefix_length] = '_';
        memcpy(builder->symbol_names[index] + prefix_length + 1u,
               suffixes[index], suffix_length + 1u);
    }
    return true;
}

static st_image_emit_status_t prepare_method_artifacts(
    emit_builder_t *builder, const st_image_emit_options_t *options)
{
    const st_class_graph_result_t *graph = builder->graph;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(builder->context);
    size_t artifact_index;
    if (!options || options->method_artifact_count == 0u) return ST_IMAGE_EMIT_OK;
    if (!arch || arch->ptr_size != 8)
        return ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET;
    for (artifact_index = 0u;
         artifact_index < options->method_artifact_count; artifact_index++) {
        const st_image_aot_method_artifact_t *artifact =
            &options->method_artifacts[artifact_index];
        const st_class_graph_method_t *method;
        size_t graph_index;
        size_t map_index;
        size_t map_total = builder->root_map_count;
        size_t bitmap_total = builder->root_bitmap_word_count;
        char *symbol_copy;
        if (artifact->method_id == ST_CLASS_GRAPH_INVALID_ID ||
            (size_t)artifact->method_id > graph->method_count)
            return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
        graph_index = (size_t)artifact->method_id - 1u;
        if (builder->method_artifacts[graph_index])
            return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
        method = &graph->methods[graph_index];
        if (artifact->owner != method->owner || !artifact->selector ||
            artifact->selector_length != method->selector.length ||
            memcmp(artifact->selector, method->selector.data,
                   method->selector.length) != 0 ||
            artifact->arity != method->node->as.method.arguments.count ||
            !valid_external_symbol(artifact->symbol,
                                   artifact->symbol_length) ||
            (artifact->flags & ~(uint32_t)ST_METHOD_FLAGS_MASK) != 0u ||
            ((artifact->flags & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u &&
             (artifact->flags & ST_METHOD_CAN_UNWIND) == 0u) ||
            ((!artifact->root_maps) != (artifact->root_map_count == 0u)) ||
            ((!artifact->blocks) != (artifact->block_count == 0u)) ||
            ((!artifact->string_literals)
                != (artifact->string_literal_count == 0u)) ||
            (artifact->frame_root_capacity == 0u &&
             artifact->root_map_count != 0u))
            return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
        if (artifact->symbol_length == SIZE_MAX ||
            artifact->root_map_count > SIZE_MAX / sizeof(*artifact->root_maps) ||
            (arch->ptr_size == 4 && artifact->root_map_count > UINT32_MAX) ||
            artifact->root_map_count > SIZE_MAX - map_total)
            return ST_IMAGE_EMIT_ERR_OVERFLOW;
        for (map_index = 0u; map_index < artifact->root_map_count; map_index++) {
            const st_image_root_map_metadata_t *map =
                &artifact->root_maps[map_index];
            size_t required_words = ((size_t)map->root_count + 63u) / 64u;
            unsigned tail = map->root_count & 63u;
            if (map->safepoint_id == 0u ||
                (map_index != 0u &&
                 artifact->root_maps[map_index - 1u].safepoint_id >=
                     map->safepoint_id) ||
                map->root_count > artifact->frame_root_capacity ||
                map->bitmap_word_count != required_words ||
                ((!map->live_root_bitmap) != (required_words == 0u)) ||
                (tail != 0u && required_words != 0u &&
                 (map->live_root_bitmap[required_words - 1u] &
                  ~(UINT64_MAX >> (64u - tail))) != 0u))
                return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
            if (required_words > SIZE_MAX - bitmap_total)
                return ST_IMAGE_EMIT_ERR_OVERFLOW;
            bitmap_total += required_words;
        }
        if (map_total + artifact->root_map_count >
                SIZE_MAX / sizeof(anvil_value_t *) ||
            bitmap_total > SIZE_MAX / sizeof(anvil_value_t *))
            return ST_IMAGE_EMIT_ERR_OVERFLOW;
        symbol_copy = builder->allocator.allocate(
            builder->allocator.user, artifact->symbol_length + 1u);
        if (!symbol_copy) return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
        memcpy(symbol_copy, artifact->symbol, artifact->symbol_length);
        symbol_copy[artifact->symbol_length] = '\0';
        for (size_t symbol_index = 0u; symbol_index < SYMBOL_COUNT;
             symbol_index++) {
            if (strcmp(symbol_copy, builder->symbol_names[symbol_index]) == 0) {
                release(builder->allocator, symbol_copy);
                return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
            }
        }
        builder->method_artifacts[graph_index] = artifact;
        builder->artifact_symbol_names[graph_index] = symbol_copy;
        builder->root_map_offsets[graph_index] = map_total;
        builder->root_map_count = map_total + artifact->root_map_count;
        builder->root_bitmap_word_count = bitmap_total;
    }
    for (artifact_index = 0u; artifact_index < graph->method_count;
         artifact_index++)
        if (!builder->method_artifacts[artifact_index])
            return ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT;
    builder->has_method_code = true;
    return ST_IMAGE_EMIT_OK;
}

static st_image_emit_status_t prepare_image_runtime_artifacts(
    emit_builder_t *builder, const st_image_emit_options_t *options)
{
    symbol_ref_t *names = NULL;
    unsigned char *runtime_used = NULL;
    uint64_t literal_cursor = 0u;
    size_t index, method_index;

    if (options != NULL) {
        builder->globals = options->globals;
        builder->global_count = options->global_count;
    }
    if (builder->global_count > UINT32_MAX)
        return ST_IMAGE_EMIT_ERR_OVERFLOW;
    names = allocate_array(builder->allocator, builder->global_count,
                           sizeof(*names));
    runtime_used = allocate_array(builder->allocator, builder->global_count,
                                  sizeof(*runtime_used));
    if (builder->global_count != 0u && (!names || !runtime_used)) {
        release(builder->allocator, names);
        release(builder->allocator, runtime_used);
        return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
    }
    for (index = 0u; index < builder->global_count; index++) {
        const st_image_global_artifact_t *global = &builder->globals[index];
        if (global->semantic_external_id == 0u
                || global->semantic_external_id == ST_SEMA_INVALID_ID
                || global->runtime_index >= builder->global_count
                || runtime_used[global->runtime_index]
                || !global->name || global->name_length == 0u
                || global->name_length == SIZE_MAX
                || global->name[global->name_length] != '\0'
                || memchr(global->name, '\0', global->name_length) != NULL
                || (index != 0u
                    && builder->globals[index - 1u].semantic_external_id
                        >= global->semantic_external_id)) {
            release(builder->allocator, names);
            release(builder->allocator, runtime_used);
            return ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT;
        }
        runtime_used[global->runtime_index] = 1u;
        names[index].bytes = global->name;
        names[index].length = global->name_length;
    }
    if (builder->global_count > 1u)
        qsort(names, builder->global_count, sizeof(*names),
              symbol_ref_compare);
    for (index = 0u; index < builder->global_count; index++) {
        if (!runtime_used[index]
                || (index != 0u && names[index - 1u].length == names[index].length
                    && memcmp(names[index - 1u].bytes, names[index].bytes,
                              names[index].length) == 0)) {
            release(builder->allocator, names);
            release(builder->allocator, runtime_used);
            return ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT;
        }
    }
    release(builder->allocator, names);
    release(builder->allocator, runtime_used);

    if (!builder->has_method_code) return ST_IMAGE_EMIT_OK;
    for (method_index = 0u; method_index < builder->graph->method_count;
         method_index++) {
        const st_image_aot_method_artifact_t *method =
            builder->method_artifacts[method_index];
        for (index = 0u; index < method->string_literal_count; index++) {
            const st_image_string_literal_artifact_t *literal =
                &method->string_literals[index];
            if (literal_cursor > UINT32_MAX
                    || literal->literal_id != (uint32_t)literal_cursor
                    || literal->method_id != method->method_id
                    || ((!literal->bytes) != (literal->length == 0u))
                    || literal->length
                        > SIZE_MAX - builder->string_literal_bytes)
                return ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT;
            if (literal->length
                    > UINT32_MAX - builder->string_literal_bytes)
                return ST_IMAGE_EMIT_ERR_OVERFLOW;
            builder->string_literal_bytes += literal->length;
            literal_cursor++;
        }
    }
    if (literal_cursor > UINT32_MAX)
        return ST_IMAGE_EMIT_ERR_OVERFLOW;
    builder->string_literal_count = (size_t)literal_cursor;
    return ST_IMAGE_EMIT_OK;
}

static st_image_emit_status_t prepare_layout(
    emit_builder_t *builder, const st_image_emit_options_t *options)
{
    const st_image_layout_result_t *layout = options != NULL
        ? options->layout : NULL;
    if (layout == NULL) {
        st_image_layout_options_t layout_options = {
            .allocator = {
                builder->allocator.allocate,
                builder->allocator.deallocate,
                builder->allocator.user
            }
        };
        st_image_layout_status_t status = st_image_layout_build(
            &builder->owned_layout, builder->graph, &layout_options);
        if (status == ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY)
            return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
        if (status == ST_IMAGE_LAYOUT_ERR_OVERFLOW)
            return ST_IMAGE_EMIT_ERR_OVERFLOW;
        if (status != ST_IMAGE_LAYOUT_OK)
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
        layout = &builder->owned_layout;
    }
    if (layout->status != ST_IMAGE_LAYOUT_OK
            || layout->entity_count != builder->graph->entity_count
            || layout->entity_runtime_class_ids == NULL
            || layout->class_count == 0u || layout->classes == NULL
            || layout->shape_count == 0u || layout->shapes == NULL
            || layout->class_count > ST_HEADER_CLASS_MAX
            || layout->shape_count > ST_HEADER_SHAPE_MAX)
        return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    for (size_t index = 0u; index < builder->graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &builder->graph->entities[index];
        uint32_t runtime_id = layout->entity_runtime_class_ids[index];
        if ((entity->kind == ST_CLASS_GRAPH_NAMESPACE) != (runtime_id == 0u))
            return ST_IMAGE_EMIT_ERR_INVALID_GRAPH;
    }
    builder->layout = layout;
    return ST_IMAGE_EMIT_OK;
}

static st_image_emit_status_t prepare_block_artifacts(emit_builder_t *builder)
{
    const st_class_graph_result_t *graph = builder->graph;
    symbol_ref_t *symbols;
    size_t method_index, block_total = 0u, block_cursor = 0u;
    size_t symbol_count, symbol_cursor = 0u;
    if (!builder->has_method_code) return ST_IMAGE_EMIT_OK;
    builder->method_root_map_count = builder->root_map_count;
    for (method_index = 0u; method_index < graph->method_count; method_index++) {
        size_t count = builder->method_artifacts[method_index]->block_count;
        if (count > SIZE_MAX - block_total) return ST_IMAGE_EMIT_ERR_OVERFLOW;
        block_total += count;
    }
    builder->blocks = allocate_array(builder->allocator, block_total,
                                     sizeof(*builder->blocks));
    if (block_total != 0u && builder->blocks == NULL)
        return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
    builder->block_count = block_total;
    if (block_total > (SIZE_MAX - SYMBOL_COUNT - graph->method_count) / 3u)
        return ST_IMAGE_EMIT_ERR_OVERFLOW;
    symbol_count = SYMBOL_COUNT + graph->method_count + block_total * 3u;
    symbols = allocate_array(builder->allocator, symbol_count,
                             sizeof(*symbols));
    if (symbols == NULL) return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
    for (method_index = 0u; method_index < SYMBOL_COUNT; method_index++)
        symbols[symbol_cursor++] = (symbol_ref_t) {
            builder->symbol_names[method_index],
            strlen(builder->symbol_names[method_index])
        };
    for (method_index = 0u; method_index < graph->method_count; method_index++)
        symbols[symbol_cursor++] = (symbol_ref_t) {
            builder->method_artifacts[method_index]->symbol,
            builder->method_artifacts[method_index]->symbol_length
        };

    for (method_index = 0u; method_index < graph->method_count; method_index++) {
        const st_image_aot_method_artifact_t *method =
            builder->method_artifacts[method_index];
        size_t block_index;
        for (block_index = 0u; block_index < method->block_count; block_index++) {
            const st_image_aot_block_artifact_t *block =
                &method->blocks[block_index];
            size_t capture_index, map_index;
            size_t bitmap_total = builder->root_bitmap_word_count;
            bool saw_cell = false;
            if ((block_index != 0u
                    && method->blocks[block_index - 1u].lexical_ordinal
                        >= block->lexical_ordinal)
                    || !valid_external_symbol(block->code_symbol,
                                              block->code_symbol_length)
                    || !valid_external_symbol(block->descriptor_symbol,
                                              block->descriptor_symbol_length)
                    || !valid_external_symbol(block->method_descriptor_symbol,
                        block->method_descriptor_symbol_length)
                    || (block->flags & ~(uint32_t)ST_AOT_BLOCK_FLAGS_MASK)
                        != 0u
                    || (block->method_flags
                        & ~(uint32_t)ST_METHOD_FLAGS_MASK) != 0u
                    || ((block->method_flags
                         & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u
                        && (block->method_flags & ST_METHOD_CAN_UNWIND) == 0u)
                    || (((block->flags & ST_AOT_BLOCK_HAS_HOME) != 0u)
                        != ((block->method_flags
                             & ST_METHOD_HAS_NON_LOCAL_RETURN) != 0u))
                    || ((!block->captures) != (block->capture_count == 0u))
                    || ((!block->root_maps) != (block->root_map_count == 0u))
                    || (block->frame_root_capacity == 0u
                        && block->root_map_count != 0u)
                    || block->capture_count > UINT32_MAX) {
                release(builder->allocator, symbols);
                return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
            }
            for (capture_index = 0u; capture_index < block->capture_count;
                 capture_index++) {
                const st_aot_capture_descriptor_t *capture =
                    &block->captures[capture_index];
                size_t prior;
                if (capture->kind > ST_AOT_CAPTURE_SELF) {
                    release(builder->allocator, symbols);
                    return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
                }
                if (capture->kind == ST_AOT_CAPTURE_CELL) saw_cell = true;
                for (prior = 0u; prior < capture_index; prior++)
                    if (block->captures[prior].binding_id
                            == capture->binding_id) {
                        release(builder->allocator, symbols);
                        return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
                    }
            }
            if (saw_cell != ((block->flags & ST_AOT_BLOCK_HAS_CELLS) != 0u)) {
                release(builder->allocator, symbols);
                return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
            }
            if (block->capture_count > SIZE_MAX - builder->block_capture_count) {
                release(builder->allocator, symbols);
                return ST_IMAGE_EMIT_ERR_OVERFLOW;
            }
            for (map_index = 0u; map_index < block->root_map_count; map_index++) {
                const st_image_root_map_metadata_t *map =
                    &block->root_maps[map_index];
                size_t words = ((size_t)map->root_count + 63u) / 64u;
                unsigned tail = map->root_count & 63u;
                if (map->safepoint_id == 0u
                        || (map_index != 0u
                            && block->root_maps[map_index - 1u].safepoint_id
                                >= map->safepoint_id)
                        || map->root_count > block->frame_root_capacity
                        || map->bitmap_word_count != words
                        || ((!map->live_root_bitmap) != (words == 0u))
                        || (tail != 0u && words != 0u
                            && (map->live_root_bitmap[words - 1u]
                                & ~(UINT64_MAX >> (64u - tail))) != 0u)) {
                    release(builder->allocator, symbols);
                    return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
                }
                if (words > SIZE_MAX - bitmap_total) {
                    release(builder->allocator, symbols);
                    return ST_IMAGE_EMIT_ERR_OVERFLOW;
                }
                bitmap_total += words;
            }
            if (block->root_map_count > SIZE_MAX - builder->root_map_count
                    || block->root_map_count
                        > SIZE_MAX - builder->block_root_map_count) {
                release(builder->allocator, symbols);
                return ST_IMAGE_EMIT_ERR_OVERFLOW;
            }
            builder->blocks[block_cursor] = (prepared_block_t) {
                block, method_index, builder->root_map_count,
                builder->block_capture_count
            };
            builder->root_map_count += block->root_map_count;
            builder->block_root_map_count += block->root_map_count;
            builder->root_bitmap_word_count = bitmap_total;
            builder->block_capture_count += block->capture_count;
            symbols[symbol_cursor++] = (symbol_ref_t) {
                block->code_symbol, block->code_symbol_length
            };
            symbols[symbol_cursor++] = (symbol_ref_t) {
                block->descriptor_symbol, block->descriptor_symbol_length
            };
            symbols[symbol_cursor++] = (symbol_ref_t) {
                block->method_descriptor_symbol,
                block->method_descriptor_symbol_length
            };
            block_cursor++;
        }
    }
    if (block_cursor != block_total || symbol_cursor != symbol_count) {
        release(builder->allocator, symbols);
        return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
    }
    qsort(symbols, symbol_count, sizeof(*symbols), symbol_ref_compare);
    for (method_index = 1u; method_index < symbol_count; method_index++)
        if (symbols[method_index - 1u].length == symbols[method_index].length
                && memcmp(symbols[method_index - 1u].bytes,
                          symbols[method_index].bytes,
                          symbols[method_index].length) == 0) {
            release(builder->allocator, symbols);
            return ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT;
        }
    release(builder->allocator, symbols);
    return ST_IMAGE_EMIT_OK;
}

static anvil_value_t *table_first_address(anvil_ctx_t *context,
                                          anvil_value_t *global,
                                          anvil_type_t *array_type)
{
    anvil_value_t *indices[2];
    anvil_value_t *base;
    base = anvil_const_symbol_addr(global);
    indices[0] = anvil_const_u32(context, 0u);
    indices[1] = anvil_const_u32(context, 0u);
    if (base == NULL || indices[0] == NULL || indices[1] == NULL) return NULL;
    return anvil_const_gep(base, array_type, indices, 2u);
}

static anvil_value_t *string_address(emit_builder_t *builder,
                                     anvil_value_t *strings_global,
                                     anvil_type_t *strings_type,
                                     st_ast_string_t string)
{
    anvil_value_t *indices[2];
    anvil_value_t *base;
    size_t offset;
    if (!string_pool_lookup(&builder->strings, string.data, string.length,
                            &offset)) return NULL;
    base = anvil_const_symbol_addr(strings_global);
    indices[0] = anvil_const_u32(builder->context, 0u);
    indices[1] = anvil_const_u64(builder->context, (uint64_t)offset);
    if (base == NULL || indices[0] == NULL || indices[1] == NULL) return NULL;
    return anvil_const_gep(base, strings_type, indices, 2u);
}

static anvil_value_t *add_initialized_global(anvil_module_t *module,
                                             const char *name,
                                             anvil_value_t *initializer,
                                             anvil_linkage_t linkage)
{
    anvil_value_t *global;
    if (initializer == NULL) return NULL;
    global = anvil_module_add_global(module, name,
                                     anvil_value_get_type(initializer), linkage);
    return global != NULL && anvil_global_set_initializer(global, initializer)
        ? global : NULL;
}

static anvil_value_t *emit_string_blob(emit_builder_t *builder,
                                       anvil_type_t **type_out)
{
    anvil_value_t **bytes;
    anvil_value_t *initializer = NULL;
    anvil_value_t *global = NULL;
    size_t record_index;
    anvil_type_t *i8 = anvil_type_u8(builder->context);
    if (i8 == NULL || builder->strings.byte_count == 0u) return NULL;
    bytes = allocate_array(builder->allocator, builder->strings.byte_count,
                           sizeof(*bytes));
    if (bytes == NULL) return NULL;
    for (record_index = 0u; record_index < builder->strings.count;
         record_index++) {
        const string_record_t *record = &builder->strings.records[record_index];
        size_t byte_index;
        for (byte_index = 0u; byte_index < record->length; byte_index++) {
            bytes[record->offset + byte_index] = anvil_const_u8(
                builder->context, (uint8_t)record->bytes[byte_index]);
            if (bytes[record->offset + byte_index] == NULL) goto done;
        }
        bytes[record->offset + record->length] = anvil_const_u8(
            builder->context, 0u);
        if (bytes[record->offset + record->length] == NULL) goto done;
    }
    initializer = anvil_const_array(builder->context, i8, bytes,
                                    builder->strings.byte_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_STRINGS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global != NULL) *type_out = anvil_value_get_type(initializer);
done:
    release(builder->allocator, bytes);
    return global;
}

static anvil_value_t *emit_selector_table(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    anvil_type_t *fields[5] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u64(ctx),
        anvil_type_u32(ctx), ptr_i8
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 5u, false);
    size_t count = st_selector_count(builder->selectors);
    anvil_value_t **elements;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t index;
    if (element == NULL) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (count == 0u) return NULL;
    elements = allocate_array(builder->allocator, count, sizeof(*elements));
    if (elements == NULL) return NULL;
    for (index = 0u; index < count; index++) {
        const st_selector_t *selector = st_selector_get(
            builder->selectors, (st_selector_id_t)(index + 1u));
        st_ast_string_t spelling;
        anvil_value_t *values[5];
        if (selector == NULL) goto fail;
        spelling.data = selector->bytes;
        spelling.length = selector->length;
        values[0] = anvil_const_u32(ctx, (uint32_t)(index + 1u));
        values[1] = anvil_const_u32(ctx, (uint32_t)selector->kind);
        values[2] = anvil_const_u64(ctx, selector->hash);
        values[3] = anvil_const_u32(ctx, selector->arity);
        values[4] = string_address(builder, strings_global, strings_type,
                                   spelling);
        elements[index] = anvil_const_struct(ctx, element, values, 5u);
        if (elements[index] == NULL) goto fail;
    }
    initializer = anvil_const_array(ctx, element, elements, count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_SELECTORS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global != NULL) {
        *element_type_out = element;
        *array_type_out = anvil_value_get_type(initializer);
    }
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_global_table(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    anvil_type_t *fields[3] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), ptr_i8
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 3u, false);
    anvil_value_t **elements;
    anvil_value_t *initializer, *global;
    size_t index;
    if (!element) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (builder->global_count == 0u) return NULL;
    elements = allocate_array(builder->allocator, builder->global_count,
                              sizeof(*elements));
    if (!elements) return NULL;
    for (index = 0u; index < builder->global_count; index++) {
        const st_image_global_artifact_t *artifact = &builder->globals[index];
        st_ast_string_t name = { artifact->name, artifact->name_length };
        anvil_value_t *values[3];
        values[0] = anvil_const_u32(ctx, artifact->semantic_external_id);
        values[1] = anvil_const_u32(ctx, artifact->runtime_index);
        values[2] = string_address(builder, strings_global, strings_type,
                                   name);
        elements[index] = anvil_const_struct(ctx, element, values, 3u);
        if (!elements[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, elements,
                                    builder->global_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_GLOBALS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_literal_blob(emit_builder_t *builder,
                                        anvil_type_t **array_type_out)
{
    anvil_type_t *i8 = anvil_type_u8(builder->context);
    size_t emitted_bytes = builder->string_literal_bytes == 0u
        ? 1u : builder->string_literal_bytes;
    anvil_value_t **bytes = allocate_array(builder->allocator, emitted_bytes,
                                           sizeof(*bytes));
    anvil_value_t *initializer, *global;
    size_t cursor = 0u, method_index, literal_index;
    if (!i8 || !bytes) return NULL;
    for (method_index = 0u; method_index < builder->graph->method_count;
         method_index++) {
        const st_image_aot_method_artifact_t *method =
            builder->method_artifacts[method_index];
        if (!method) continue;
        for (literal_index = 0u;
             literal_index < method->string_literal_count; literal_index++) {
            const st_image_string_literal_artifact_t *literal =
                &method->string_literals[literal_index];
            for (size_t byte_index = 0u; byte_index < literal->length;
                 byte_index++) {
                if (cursor >= emitted_bytes) goto fail;
                bytes[cursor++] = anvil_const_u8(builder->context,
                                                 literal->bytes[byte_index]);
                if (!bytes[cursor - 1u]) goto fail;
            }
        }
    }
    if (cursor != builder->string_literal_bytes) goto fail;
    if (cursor == 0u) {
        bytes[0] = anvil_const_u8(builder->context, 0u);
        if (!bytes[0]) goto fail;
    }
    initializer = anvil_const_array(builder->context, i8, bytes,
                                    emitted_bytes);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_LITERAL_BYTES], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, bytes);
    return global;
fail:
    release(builder->allocator, bytes);
    return NULL;
}

static anvil_value_t *emit_string_literal_table(
    emit_builder_t *builder, anvil_value_t *blob_global,
    anvil_type_t *blob_type, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[4] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_ptr(ctx, anvil_type_u8(ctx)), target_size_type(builder)
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 4u, false);
    anvil_value_t **elements;
    anvil_value_t *initializer, *global;
    size_t cursor = 0u, byte_cursor = 0u, method_index, literal_index;
    if (!element) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (builder->string_literal_count == 0u) return NULL;
    elements = allocate_array(builder->allocator,
        builder->string_literal_count, sizeof(*elements));
    if (!elements) return NULL;
    for (method_index = 0u; method_index < builder->graph->method_count;
         method_index++) {
        const st_image_aot_method_artifact_t *method =
            builder->method_artifacts[method_index];
        for (literal_index = 0u;
             literal_index < method->string_literal_count; literal_index++) {
            const st_image_string_literal_artifact_t *literal =
                &method->string_literals[literal_index];
            anvil_value_t *values[4];
            values[0] = anvil_const_u32(ctx, literal->literal_id);
            values[1] = anvil_const_u32(ctx, literal->method_id);
            values[2] = table_index_address(ctx, blob_global, blob_type,
                                            byte_cursor);
            values[3] = target_size_constant(builder, literal->length);
            elements[cursor++] = anvil_const_struct(ctx, element, values, 4u);
            if (!elements[cursor - 1u]) goto fail;
            byte_cursor += literal->length;
        }
    }
    if (cursor != builder->string_literal_count
            || byte_cursor != builder->string_literal_bytes) goto fail;
    initializer = anvil_const_array(ctx, element, elements, cursor);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_STRING_LITERALS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_slot_table(
    emit_builder_t *builder, const st_class_graph_slot_t *slots, size_t count,
    int symbol_index, anvil_value_t *strings_global, anvil_type_t *strings_type,
    anvil_type_t **element_type_out, anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    anvil_type_t *fields[9] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_u32(ctx), ptr_i8, ptr_i8
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 9u, false);
    anvil_value_t **elements;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t index;
    if (element == NULL) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (count == 0u) return NULL;
    elements = allocate_array(builder->allocator, count, sizeof(*elements));
    if (elements == NULL) return NULL;
    for (index = 0u; index < count; index++) {
        anvil_value_t *values[9];
        values[0] = anvil_const_u32(ctx, slots[index].declaring_class);
        values[1] = anvil_const_u32(ctx, slots[index].slot);
        values[2] = anvil_const_u32(ctx, (uint32_t)slots[index].kind);
        values[3] = anvil_const_u32(ctx, slots[index].has_type
                                    ? ST_IMAGE_SLOT_FLAG_HAS_TYPE : 0u);
        values[4] = anvil_const_u32(ctx,
                                    (uint32_t)slots[index].origin.unit_index);
        values[5] = anvil_const_u32(
            ctx, (uint32_t)slots[index].origin.span.begin.line);
        values[6] = anvil_const_u32(
            ctx, (uint32_t)slots[index].origin.span.begin.column);
        values[7] = string_address(builder, strings_global, strings_type,
                                   slots[index].name);
        values[8] = slots[index].has_type
            ? string_address(builder, strings_global, strings_type,
                             slots[index].type_name)
            : anvil_const_null(ctx, ptr_i8);
        elements[index] = anvil_const_struct(ctx, element, values, 9u);
        if (elements[index] == NULL) goto fail;
    }
    initializer = anvil_const_array(ctx, element, elements, count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[symbol_index], initializer, ANVIL_LINK_INTERNAL);
    if (global != NULL) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *table_index_address(anvil_ctx_t *context,
                                          anvil_value_t *global,
                                          anvil_type_t *array_type,
                                          size_t index)
{
    anvil_value_t *indices[2];
    anvil_value_t *base;
    if (!global || !array_type) return NULL;
    base = anvil_const_symbol_addr(global);
    indices[0] = anvil_const_u32(context, 0u);
    indices[1] = anvil_const_u64(context, (uint64_t)index);
    if (!base || !indices[0] || !indices[1]) return NULL;
    return anvil_const_gep(base, array_type, indices, 2u);
}

static anvil_value_t *target_size_constant(emit_builder_t *builder,
                                           size_t value)
{
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(builder->context);
    if (!arch) return NULL;
    if (arch->ptr_size == 4) {
        if (value > UINT32_MAX) return NULL;
        return anvil_const_u32(builder->context, (uint32_t)value);
    }
    return arch->ptr_size == 8
        ? anvil_const_u64(builder->context, (uint64_t)value) : NULL;
}

static anvil_type_t *target_size_type(emit_builder_t *builder)
{
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(builder->context);
    if (!arch) return NULL;
    if (arch->ptr_size == 4) return anvil_type_u32(builder->context);
    return arch->ptr_size == 8 ? anvil_type_u64(builder->context) : NULL;
}

static anvil_value_t *emit_root_bitmap_table(
    emit_builder_t *builder, anvil_type_t **array_type_out)
{
    anvil_type_t *u64 = anvil_type_u64(builder->context);
    anvil_value_t **words;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t method_index;
    size_t cursor = 0u;
    *array_type_out = NULL;
    if (!u64 || builder->root_bitmap_word_count == 0u) return NULL;
    words = allocate_array(builder->allocator,
                           builder->root_bitmap_word_count, sizeof(*words));
    if (!words) return NULL;
    for (method_index = 0u; method_index < builder->graph->method_count;
         method_index++) {
        const st_image_aot_method_artifact_t *artifact =
            builder->method_artifacts[method_index];
        size_t map_index;
        if (!artifact) goto fail;
        for (map_index = 0u; map_index < artifact->root_map_count; map_index++) {
            const st_image_root_map_metadata_t *map =
                &artifact->root_maps[map_index];
            size_t word_index;
            for (word_index = 0u; word_index < map->bitmap_word_count;
                 word_index++) {
                if (cursor >= builder->root_bitmap_word_count) goto fail;
                words[cursor++] = anvil_const_u64(
                    builder->context, map->live_root_bitmap[word_index]);
                if (!words[cursor - 1u]) goto fail;
            }
        }
    }
    for (method_index = 0u; method_index < builder->block_count;
         method_index++) {
        const st_image_aot_block_artifact_t *artifact =
            builder->blocks[method_index].artifact;
        size_t map_index;
        for (map_index = 0u; map_index < artifact->root_map_count; map_index++) {
            const st_image_root_map_metadata_t *map =
                &artifact->root_maps[map_index];
            size_t word_index;
            for (word_index = 0u; word_index < map->bitmap_word_count;
                 word_index++) {
                if (cursor >= builder->root_bitmap_word_count) goto fail;
                words[cursor++] = anvil_const_u64(
                    builder->context, map->live_root_bitmap[word_index]);
                if (!words[cursor - 1u]) goto fail;
            }
        }
    }
    if (cursor != builder->root_bitmap_word_count) goto fail;
    initializer = anvil_const_array(builder->context, u64, words, cursor);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_ROOT_BITMAPS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, words);
    return global;
fail:
    release(builder->allocator, words);
    return NULL;
}

static anvil_value_t *emit_root_map_table(
    emit_builder_t *builder, anvil_value_t *bitmap_global,
    anvil_type_t *bitmap_array, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *fields[4] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), target_size_type(builder),
        anvil_type_ptr(ctx, u64)
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 4u, false);
    anvil_value_t **maps;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t method_index;
    size_t map_cursor = 0u;
    size_t bitmap_cursor = 0u;
    if (!element) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (builder->root_map_count == 0u) return NULL;
    maps = allocate_array(builder->allocator, builder->root_map_count,
                          sizeof(*maps));
    if (!maps) return NULL;
    for (method_index = 0u; method_index < builder->graph->method_count;
         method_index++) {
        const st_image_aot_method_artifact_t *artifact =
            builder->method_artifacts[method_index];
        size_t map_index;
        if (!artifact) goto fail;
        for (map_index = 0u; map_index < artifact->root_map_count; map_index++) {
            const st_image_root_map_metadata_t *map =
                &artifact->root_maps[map_index];
            anvil_value_t *values[4];
            if (map_cursor >= builder->root_map_count) goto fail;
            values[0] = anvil_const_u32(ctx, map->safepoint_id);
            values[1] = anvil_const_u32(ctx, map->root_count);
            values[2] = target_size_constant(builder,
                                             map->bitmap_word_count);
            values[3] = map->bitmap_word_count == 0u
                ? anvil_const_null(ctx, fields[3])
                : table_index_address(ctx, bitmap_global, bitmap_array,
                                      bitmap_cursor);
            maps[map_cursor++] = anvil_const_struct(ctx, element, values, 4u);
            if (!maps[map_cursor - 1u]) goto fail;
            if (map->bitmap_word_count > SIZE_MAX - bitmap_cursor) goto fail;
            bitmap_cursor += map->bitmap_word_count;
        }
    }
    for (method_index = 0u; method_index < builder->block_count;
         method_index++) {
        const st_image_aot_block_artifact_t *artifact =
            builder->blocks[method_index].artifact;
        size_t map_index;
        for (map_index = 0u; map_index < artifact->root_map_count; map_index++) {
            const st_image_root_map_metadata_t *map =
                &artifact->root_maps[map_index];
            anvil_value_t *values[4];
            if (map_cursor >= builder->root_map_count) goto fail;
            values[0] = anvil_const_u32(ctx, map->safepoint_id);
            values[1] = anvil_const_u32(ctx, map->root_count);
            values[2] = target_size_constant(builder,
                                             map->bitmap_word_count);
            values[3] = map->bitmap_word_count == 0u
                ? anvil_const_null(ctx, fields[3])
                : table_index_address(ctx, bitmap_global, bitmap_array,
                                      bitmap_cursor);
            maps[map_cursor++] = anvil_const_struct(ctx, element, values, 4u);
            if (!maps[map_cursor - 1u]) goto fail;
            if (map->bitmap_word_count > SIZE_MAX - bitmap_cursor) goto fail;
            bitmap_cursor += map->bitmap_word_count;
        }
    }
    if (map_cursor != builder->root_map_count ||
        bitmap_cursor != builder->root_bitmap_word_count)
        goto fail;
    initializer = anvil_const_array(ctx, element, maps, map_cursor);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_ROOT_MAPS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, maps);
    return global;
fail:
    release(builder->allocator, maps);
    return NULL;
}

static anvil_type_t *method_code_function_type(emit_builder_t *builder)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *frame = anvil_type_named_struct(ctx, "StFrame");
    anvil_type_t *parameters[1];
    anvil_type_t *function;
    if (!frame) return NULL;
    parameters[0] = anvil_type_ptr(ctx, frame);
    function = anvil_type_func(ctx, anvil_type_u64(ctx), parameters, 1u,
                               false);
    return function;
}

static anvil_value_t *method_code_extern(emit_builder_t *builder,
                                         size_t method_index)
{
    anvil_type_t *function_type;
    if (!builder->has_method_code
            || method_index >= builder->graph->method_count)
        return NULL;
    if (builder->method_code_externs[method_index] != NULL)
        return builder->method_code_externs[method_index];
    function_type = method_code_function_type(builder);
    if (function_type == NULL) return NULL;
    builder->method_code_externs[method_index] = anvil_module_add_extern(
        builder->module, builder->artifact_symbol_names[method_index],
        function_type);
    return builder->method_code_externs[method_index];
}

static anvil_type_t *runtime_method_type(emit_builder_t *builder,
                                         anvil_type_t *root_map_element)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *unwind_fields[5] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_u32(ctx), anvil_type_u32(ctx)
    };
    anvil_type_t *unwind = anvil_type_literal_struct(ctx, unwind_fields, 5u,
                                                     false);
    anvil_type_t *size = target_size_type(builder);
    anvil_type_t *fields[15];
    anvil_type_t *type;
    size_t index;
    if (!unwind || !size || !root_map_element) return NULL;
    for (index = 0u; index < 7u; index++) fields[index] = anvil_type_u32(ctx);
    fields[7] = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    fields[8] = size;
    fields[9] = size;
    fields[10] = size;
    fields[11] = anvil_type_ptr(ctx, root_map_element);
    fields[12] = size;
    fields[13] = anvil_type_ptr(ctx, unwind);
    fields[14] = size;
    type = anvil_type_literal_struct(ctx, fields, 15u, false);
    if (!type || anvil_type_size(type) != sizeof(StMethodDescriptor)
            || anvil_type_align(type) != _Alignof(StMethodDescriptor)
            || anvil_type_struct_field_offset(type, 7u)
                != offsetof(StMethodDescriptor, source_name)
            || anvil_type_struct_field_offset(type, 11u)
                != offsetof(StMethodDescriptor, root_maps)
            || anvil_type_struct_field_offset(type, 13u)
                != offsetof(StMethodDescriptor, unwind_regions))
        return NULL;
    return type;
}

static anvil_value_t *runtime_method_initializer(
    emit_builder_t *builder, anvil_type_t *type,
    const st_class_graph_method_t *method, uint32_t arity,
    uint32_t root_capacity, uint32_t flags, size_t root_map_offset,
    size_t root_map_count, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_value_t *root_maps_global,
    anvil_type_t *root_map_element, anvil_type_t *root_maps_array)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_value_t *values[15];
    anvil_type_t *root_pointer = anvil_type_ptr(ctx, root_map_element);
    anvil_type_t *unwind_pointer = anvil_type_struct_field_type(type, 13u);
    values[0] = anvil_const_u32(ctx, ST_METHOD_ABI_VERSION);
    values[1] = anvil_const_u32(ctx,
                                builder->method_selector_ids[method->id - 1u]);
    values[2] = anvil_const_u32(
        ctx, st_image_layout_runtime_class_id(builder->layout,
                                              method->owner));
    values[3] = anvil_const_u32(ctx, arity);
    values[4] = anvil_const_u32(ctx, root_capacity);
    values[5] = anvil_const_u32(ctx, flags);
    values[6] = anvil_const_u32(ctx, 0u);
    values[7] = string_address(builder, strings_global, strings_type,
                               method->origin.source_name);
    values[8] = target_size_constant(builder,
                                     method->origin.source_name.length);
    values[9] = target_size_constant(builder, method->origin.span.begin.offset);
    values[10] = target_size_constant(builder, method->origin.span.end.offset);
    values[11] = root_map_count != 0u
        ? table_index_address(ctx, root_maps_global, root_maps_array,
                              root_map_offset)
        : anvil_const_null(ctx, root_pointer);
    values[12] = target_size_constant(builder, root_map_count);
    values[13] = anvil_const_null(ctx, unwind_pointer);
    values[14] = target_size_constant(builder, 0u);
    for (size_t index = 0u; index < 15u; index++)
        if (!values[index]) return NULL;
    return anvil_const_struct(ctx, type, values, 15u);
}

static anvil_value_t *emit_runtime_method_table(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_value_t *root_maps_global,
    anvil_type_t *root_map_element, anvil_type_t *root_maps_array,
    anvil_type_t **element_type_out, anvil_type_t **array_type_out)
{
    anvil_type_t *element = runtime_method_type(builder, root_map_element);
    anvil_value_t **elements;
    anvil_value_t *initializer, *global;
    size_t index;
    *element_type_out = element;
    *array_type_out = NULL;
    if (!element) return NULL;
    if (builder->graph->method_count == 0u) return NULL;
    elements = allocate_array(builder->allocator, builder->graph->method_count,
                              sizeof(*elements));
    if (!elements) return NULL;
    for (index = 0u; index < builder->graph->method_count; index++) {
        const st_image_aot_method_artifact_t *artifact =
            builder->method_artifacts[index];
        elements[index] = runtime_method_initializer(
            builder, element, &builder->graph->methods[index],
            artifact->arity, artifact->frame_root_capacity, artifact->flags,
            builder->root_map_offsets[index], artifact->root_map_count,
            strings_global, strings_type, root_maps_global, root_map_element,
            root_maps_array);
        if (!elements[index]) goto fail;
    }
    initializer = anvil_const_array(builder->context, element, elements,
                                    builder->graph->method_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_RUNTIME_METHODS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_block_capture_table(
    emit_builder_t *builder, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[2] = { anvil_type_u32(ctx), anvil_type_u32(ctx) };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 2u, false);
    anvil_value_t **elements;
    anvil_value_t *initializer, *global;
    size_t block_index, cursor = 0u;
    *element_type_out = element;
    *array_type_out = NULL;
    if (!element || anvil_type_size(element)
            != sizeof(st_aot_capture_descriptor_t)) return NULL;
    if (builder->block_capture_count == 0u) return NULL;
    elements = allocate_array(builder->allocator, builder->block_capture_count,
                              sizeof(*elements));
    if (!elements) return NULL;
    for (block_index = 0u; block_index < builder->block_count; block_index++) {
        const st_image_aot_block_artifact_t *block =
            builder->blocks[block_index].artifact;
        size_t capture_index;
        for (capture_index = 0u; capture_index < block->capture_count;
             capture_index++) {
            anvil_value_t *values[2] = {
                anvil_const_u32(ctx, block->captures[capture_index].binding_id),
                anvil_const_u32(ctx, block->captures[capture_index].kind)
            };
            if (cursor >= builder->block_capture_count) goto fail;
            elements[cursor++] = anvil_const_struct(ctx, element, values, 2u);
            if (!elements[cursor - 1u]) goto fail;
        }
    }
    if (cursor != builder->block_capture_count) goto fail;
    initializer = anvil_const_array(ctx, element, elements, cursor);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_BLOCK_CAPTURES], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_block_descriptors(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_value_t *root_maps_global,
    anvil_type_t *root_map_element, anvil_type_t *root_maps_array,
    anvil_value_t *captures_global, anvil_type_t *capture_element,
    anvil_type_t *captures_array, anvil_type_t *runtime_method_element,
    anvil_type_t **registry_element_out, anvil_type_t **registry_array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *method_function = method_code_function_type(builder);
    anvil_type_t *method_code = method_function
        ? anvil_type_ptr(ctx, method_function) : NULL;
    anvil_type_t *fields[8];
    anvil_type_t *descriptor_type;
    anvil_type_t *descriptor_pointer;
    anvil_value_t **registry;
    anvil_value_t *initializer, *global;
    size_t index;
    *registry_element_out = NULL;
    *registry_array_out = NULL;
    if (!method_code || !runtime_method_element || !capture_element)
        return NULL;
    fields[0] = anvil_type_u32(ctx);
    fields[1] = anvil_type_u32(ctx);
    fields[2] = anvil_type_u32(ctx);
    fields[3] = anvil_type_u32(ctx);
    fields[4] = method_code;
    fields[5] = anvil_type_ptr(ctx, runtime_method_element);
    fields[6] = anvil_type_ptr(ctx, capture_element);
    fields[7] = target_size_type(builder);
    descriptor_type = anvil_type_literal_struct(ctx, fields, 8u, false);
    if (!descriptor_type || anvil_type_size(descriptor_type)
            != sizeof(st_aot_block_descriptor_t)) return NULL;
    descriptor_pointer = anvil_type_ptr(ctx, descriptor_type);
    *registry_element_out = descriptor_pointer;
    if (builder->block_count == 0u) return NULL;
    registry = allocate_array(builder->allocator, builder->block_count,
                              sizeof(*registry));
    if (!registry) return NULL;
    for (index = 0u; index < builder->block_count; index++) {
        const prepared_block_t *prepared = &builder->blocks[index];
        const st_image_aot_block_artifact_t *block = prepared->artifact;
        const st_class_graph_method_t *method =
            &builder->graph->methods[prepared->method_index];
        anvil_value_t *method_value = runtime_method_initializer(
            builder, runtime_method_element, method, block->arity,
            block->frame_root_capacity, block->method_flags,
            prepared->root_map_offset, block->root_map_count, strings_global,
            strings_type, root_maps_global, root_map_element, root_maps_array);
        anvil_value_t *method_global;
        anvil_value_t *code;
        anvil_value_t *values[8];
        anvil_value_t *descriptor_value;
        anvil_value_t *descriptor_global;
        if (!method_value) goto fail;
        method_global = add_initialized_global(builder->module,
            block->method_descriptor_symbol, method_value, ANVIL_LINK_EXTERNAL);
        code = anvil_module_add_extern(builder->module, block->code_symbol,
                                       method_function);
        values[0] = anvil_const_u32(ctx, ST_AOT_BLOCK_ABI_VERSION);
        values[1] = anvil_const_u32(ctx, block->arity);
        values[2] = anvil_const_u32(ctx, (uint32_t)block->capture_count);
        values[3] = anvil_const_u32(ctx, block->flags);
        values[4] = code ? anvil_const_symbol_addr(code) : NULL;
        values[5] = method_global ? anvil_const_symbol_addr(method_global)
                                  : NULL;
        values[6] = block->capture_count != 0u
            ? table_index_address(ctx, captures_global, captures_array,
                                  prepared->capture_offset)
            : anvil_const_null(ctx, fields[6]);
        values[7] = target_size_constant(builder, block->capture_count);
        descriptor_value = anvil_const_struct(ctx, descriptor_type, values,
                                              8u);
        descriptor_global = add_initialized_global(builder->module,
            block->descriptor_symbol, descriptor_value, ANVIL_LINK_EXTERNAL);
        registry[index] = descriptor_global
            ? anvil_const_symbol_addr(descriptor_global) : NULL;
        if (!registry[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, descriptor_pointer, registry,
                                    builder->block_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_BLOCK_DESCRIPTORS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *registry_array_out = anvil_value_get_type(initializer);
    release(builder->allocator, registry);
    return global;
fail:
    release(builder->allocator, registry);
    return NULL;
}

static anvil_value_t *emit_method_table(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_value_t *root_maps_global,
    anvil_type_t *root_map_element, anvil_type_t *root_maps_array,
    anvil_value_t *runtime_methods_global,
    anvil_type_t *runtime_method_element,
    anvil_type_t *runtime_methods_array,
    anvil_type_t **element_type_out, anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    anvil_type_t *frame_type = anvil_type_named_struct(ctx, "StFrame");
    anvil_type_t *frame_pointer = frame_type
        ? anvil_type_ptr(ctx, frame_type) : NULL;
    anvil_type_t *method_params[1] = { frame_pointer };
    anvil_type_t *method_function = anvil_type_func(
        ctx, anvil_type_u64(ctx), method_params, 1u, false);
    anvil_type_t *method_pointer = method_function
        ? anvil_type_ptr(ctx, method_function) : NULL;
    anvil_type_t *fields[18];
    anvil_type_t *element;
    anvil_value_t **elements;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t output_index;
    for (output_index = 0u; output_index < 12u; output_index++)
        fields[output_index] = anvil_type_u32(ctx);
    fields[12] = method_pointer;
    fields[13] = anvil_type_ptr(ctx, root_map_element);
    fields[14] = target_size_type(builder);
    fields[15] = ptr_i8;
    fields[16] = ptr_i8;
    fields[17] = anvil_type_ptr(ctx, runtime_method_element);
    element = method_pointer && runtime_method_element
        ? anvil_type_literal_struct(ctx, fields, 18u, false) : NULL;
    if (element == NULL) return NULL;
    *element_type_out = element;
    *array_type_out = NULL;
    if (builder->graph->method_count == 0u) return NULL;
    elements = allocate_array(builder->allocator, builder->graph->method_count,
                              sizeof(*elements));
    if (elements == NULL) return NULL;
    for (output_index = 0u; output_index < builder->graph->method_count;
         output_index++) {
        size_t method_index = builder->method_order[output_index];
        const st_class_graph_method_t *method =
            &builder->graph->methods[method_index];
        const st_image_aot_method_artifact_t *artifact =
            builder->has_method_code
                ? builder->method_artifacts[method_index] : NULL;
        anvil_value_t *values[18];
        values[0] = anvil_const_u32(ctx, method->id);
        values[1] = anvil_const_u32(ctx, method->owner);
        values[2] = anvil_const_u32(ctx, method->instance_class);
        values[3] = anvil_const_u32(ctx, method->lexical_super);
        values[4] = anvil_const_u32(ctx,
                                    builder->method_selector_ids[method_index]);
        values[5] = anvil_const_u32(ctx,
            (uint32_t)method->node->as.method.arguments.count);
        values[6] = anvil_const_u32(ctx, method->class_side
                                    ? ST_IMAGE_METHOD_FLAG_CLASS_SIDE : 0u);
        values[7] = anvil_const_u32(ctx, (uint32_t)method->origin.unit_index);
        values[8] = anvil_const_u32(
            ctx, (uint32_t)method->origin.span.begin.line);
        values[9] = anvil_const_u32(
            ctx, (uint32_t)method->origin.span.begin.column);
        values[10] = anvil_const_u32(
            ctx, artifact ? artifact->frame_root_capacity : 0u);
        values[11] = anvil_const_u32(ctx, artifact ? artifact->flags : 0u);
        if (artifact) {
            anvil_value_t *function = method_code_extern(builder, method_index);
            values[12] = function ? anvil_const_symbol_addr(function) : NULL;
        } else {
            values[12] = anvil_const_null(ctx, method_pointer);
        }
        values[13] = artifact && artifact->root_map_count != 0u
            ? table_index_address(ctx, root_maps_global, root_maps_array,
                                  builder->root_map_offsets[method_index])
            : anvil_const_null(ctx, fields[13]);
        values[14] = target_size_constant(
            builder, artifact ? artifact->root_map_count : 0u);
        values[15] = string_address(builder, strings_global, strings_type,
                                    method->selector);
        values[16] = string_address(builder, strings_global, strings_type,
                                    method->origin.source_name);
        values[17] = builder->has_method_code
            ? table_index_address(ctx, runtime_methods_global,
                                  runtime_methods_array, method_index)
            : anvil_const_null(ctx, fields[17]);
        elements[output_index] = anvil_const_struct(
            ctx, element, values, 18u);
        if (elements[output_index] == NULL) goto fail;
    }
    initializer = anvil_const_array(ctx, element, elements,
                                    builder->graph->method_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_METHODS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global != NULL) *array_type_out = anvil_value_get_type(initializer);
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *emit_entity_table(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_type, anvil_type_t **element_type_out,
    anvil_type_t **array_type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[16];
    anvil_type_t *element;
    anvil_value_t **elements;
    anvil_value_t *initializer;
    anvil_value_t *global;
    size_t index;
    for (index = 0u; index < 15u; index++) fields[index] = anvil_type_u32(ctx);
    fields[15] = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    element = anvil_type_literal_struct(ctx, fields, 16u, false);
    if (element == NULL) return NULL;
    elements = allocate_array(builder->allocator, builder->graph->entity_count,
                              sizeof(*elements));
    if (elements == NULL) return NULL;
    for (index = 0u; index < builder->graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &builder->graph->entities[index];
        anvil_value_t *values[16];
        values[0] = anvil_const_u32(ctx, entity->id);
        values[1] = anvil_const_u32(ctx, (uint32_t)entity->kind);
        values[2] = anvil_const_u32(ctx, entity->namespace_id);
        values[3] = anvil_const_u32(ctx, entity->superclass_id);
        values[4] = anvil_const_u32(ctx, entity->metaclass_id);
        values[5] = anvil_const_u32(ctx, entity->instance_class_id);
        values[6] = anvil_const_u32(ctx, (uint32_t)entity->instance_slot_offset);
        values[7] = anvil_const_u32(ctx, (uint32_t)entity->instance_slot_count);
        values[8] = anvil_const_u32(ctx, (uint32_t)entity->class_variable_offset);
        values[9] = anvil_const_u32(ctx, (uint32_t)entity->class_variable_count);
        values[10] = anvil_const_u32(ctx, builder->method_offsets[index]);
        values[11] = anvil_const_u32(ctx, builder->method_counts[index]);
        values[12] = anvil_const_u32(ctx,
                                     (uint32_t)entity->origin.unit_index);
        values[13] = anvil_const_u32(
            ctx, (uint32_t)entity->origin.span.begin.line);
        values[14] = anvil_const_u32(
            ctx, (uint32_t)entity->origin.span.begin.column);
        values[15] = string_address(builder, strings_global, strings_type,
                                    entity->name);
        elements[index] = anvil_const_struct(ctx, element, values, 16u);
        if (elements[index] == NULL) goto fail;
    }
    initializer = anvil_const_array(ctx, element, elements,
                                    builder->graph->entity_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_ENTITIES], initializer,
        ANVIL_LINK_INTERNAL);
    if (global != NULL) {
        *element_type_out = element;
        *array_type_out = anvil_value_get_type(initializer);
    }
    release(builder->allocator, elements);
    return global;
fail:
    release(builder->allocator, elements);
    return NULL;
}

static anvil_value_t *table_or_null(emit_builder_t *builder,
                                    anvil_value_t *global,
                                    anvil_type_t *array_type,
                                    anvil_type_t *element_type)
{
    if (global != NULL && array_type != NULL)
        return table_first_address(builder->context, global, array_type);
    return anvil_const_null(builder->context,
                            anvil_type_ptr(builder->context, element_type));
}

static anvil_value_t *emit_runtime_bindings(
    emit_builder_t *builder, anvil_value_t *runtime_methods_global,
    anvil_type_t *runtime_method_element, anvil_type_t *runtime_methods_array,
    anvil_type_t **element_out, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *function = method_code_function_type(builder);
    anvil_type_t *fields[3] = {
        anvil_type_ptr(ctx, runtime_method_element),
        function != NULL ? anvil_type_ptr(ctx, function) : NULL,
        anvil_type_u64(ctx)
    };
    anvil_type_t *element = fields[1] != NULL
        ? anvil_type_literal_struct(ctx, fields, 3u, false) : NULL;
    anvil_value_t **values = NULL;
    anvil_value_t *initializer, *global;
    *element_out = element;
    *array_out = NULL;
    if (!element || !builder->has_method_code
            || builder->graph->method_count == 0u)
        return NULL;
    values = allocate_array(builder->allocator, builder->graph->method_count,
                            sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < builder->graph->method_count; index++) {
        anvil_value_t *code = method_code_extern(builder, index);
        anvil_value_t *fields_values[3] = {
            table_index_address(ctx, runtime_methods_global,
                                runtime_methods_array, index),
            code != NULL ? anvil_const_symbol_addr(code) : NULL,
            anvil_const_u64(ctx, 1u)
        };
        values[index] = anvil_const_struct(ctx, element, fields_values, 3u);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->graph->method_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_RUNTIME_BINDINGS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_method_entries(
    emit_builder_t *builder, anvil_value_t *bindings_global,
    anvil_type_t *binding_element, anvil_type_t *bindings_array,
    anvil_type_t **element_out, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[3] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_ptr(ctx, binding_element)
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 3u, false);
    anvil_value_t **values = NULL;
    anvil_value_t *initializer, *global;
    *element_out = element;
    *array_out = NULL;
    if (!element || !builder->has_method_code
            || builder->graph->method_count == 0u)
        return NULL;
    values = allocate_array(builder->allocator, builder->graph->method_count,
                            sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < builder->graph->method_count; index++) {
        const st_class_graph_method_t *method = &builder->graph->methods[index];
        anvil_value_t *fields_values[3] = {
            anvil_const_u32(ctx, builder->method_selector_ids[index]),
            anvil_const_u32(
                ctx, st_image_layout_runtime_class_id(builder->layout,
                                                      method->owner)),
            table_index_address(ctx, bindings_global, bindings_array, index)
        };
        values[index] = anvil_const_struct(ctx, element, fields_values, 3u);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->graph->method_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_METHOD_ENTRIES], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_method_slots(
    emit_builder_t *builder, anvil_value_t *entries_global,
    anvil_type_t *entry_element, anvil_type_t *entries_array,
    anvil_type_t **element_out, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[2] = {
        anvil_type_u32(ctx), anvil_type_ptr(ctx, entry_element)
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 2u, false);
    anvil_value_t **values = NULL;
    anvil_value_t *initializer, *global;
    *element_out = element;
    *array_out = NULL;
    if (!element || !builder->has_method_code
            || builder->graph->method_count == 0u)
        return NULL;
    values = allocate_array(builder->allocator, builder->graph->method_count,
                            sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t output = 0u; output < builder->graph->method_count; output++) {
        size_t method_index = builder->method_order[output];
        anvil_value_t *fields_values[2] = {
            anvil_const_u32(ctx, builder->method_selector_ids[method_index]),
            table_index_address(ctx, entries_global, entries_array,
                                method_index)
        };
        values[output] = anvil_const_struct(ctx, element, fields_values, 2u);
        if (!values[output]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->graph->method_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_METHOD_SLOTS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_shape_pointer_bitmaps(
    emit_builder_t *builder, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    size_t count = builder->layout->pointer_bitmap_word_count;
    anvil_value_t **values;
    anvil_value_t *initializer, *global;
    *array_out = NULL;
    if (count == 0u) return NULL;
    values = allocate_array(builder->allocator, count, sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < count; index++) {
        values[index] = anvil_const_u64(
            ctx, builder->layout->pointer_bitmaps[index]);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, anvil_type_u64(ctx), values, count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_SHAPE_BITMAPS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static bool round_up_size(size_t value, size_t alignment, size_t *result_out)
{
    size_t mask;
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
        return false;
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) return false;
    *result_out = (value + mask) & ~mask;
    return true;
}

static anvil_value_t *emit_shape_descriptors(
    emit_builder_t *builder, anvil_value_t *bitmaps_global,
    anvil_type_t *bitmaps_array, anvil_type_t **element_out,
    anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *size_type = target_size_type(builder);
    anvil_type_t *fields[8] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), size_type, size_type,
        size_type, anvil_type_u32(ctx),
        anvil_type_ptr(ctx, anvil_type_u64(ctx)), size_type
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 8u, false);
    anvil_type_t *heap_fields[3] = {
        anvil_type_u64(ctx), size_type, size_type
    };
    anvil_type_t *heap_prefix = anvil_type_literal_struct(
        ctx, heap_fields, 3u, false);
    anvil_value_t **values = NULL;
    anvil_value_t *initializer, *global;
    size_t alignment, payload_offset;
    *element_out = element;
    *array_out = NULL;
    if (!element || !heap_prefix || builder->layout->shape_count == 0u)
        return NULL;
    alignment = anvil_type_align(heap_prefix);
    payload_offset = anvil_type_size(heap_prefix);
    if (alignment == 0u || payload_offset == 0u) return NULL;
    values = allocate_array(builder->allocator, builder->layout->shape_count,
                            sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < builder->layout->shape_count; index++) {
        const st_image_runtime_shape_layout_t *shape =
            &builder->layout->shapes[index];
        size_t fixed_bytes, minimum;
        if (shape->fixed_word_count > SIZE_MAX / sizeof(uint64_t)) goto fail;
        fixed_bytes = shape->fixed_word_count * sizeof(uint64_t);
        if (payload_offset > SIZE_MAX - fixed_bytes
                || !round_up_size(payload_offset + fixed_bytes,
                                  alignment, &minimum))
            goto fail;
        anvil_value_t *fields_values[8] = {
            anvil_const_u32(ctx, shape->runtime_shape_id),
            anvil_const_u32(ctx, shape->runtime_class_id),
            target_size_constant(builder, alignment),
            target_size_constant(builder, minimum),
            target_size_constant(builder, shape->fixed_word_count),
            anvil_const_u32(ctx, (uint32_t)shape->indexed_format),
            shape->bitmap_word_count != 0u
                ? table_index_address(ctx, bitmaps_global, bitmaps_array,
                                      shape->bitmap_offset)
                : anvil_const_null(ctx, fields[6]),
            target_size_constant(builder, shape->bitmap_word_count)
        };
        values[index] = anvil_const_struct(ctx, element, fields_values, 8u);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->layout->shape_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_SHAPE_DESCRIPTORS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_class_descriptors(
    emit_builder_t *builder, anvil_value_t *strings_global,
    anvil_type_t *strings_array, anvil_value_t *slots_global,
    anvil_type_t *slot_element, anvil_type_t *slots_array,
    anvil_type_t **element_out, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *size_type = target_size_type(builder);
    anvil_type_t *fields[9] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_ptr(ctx, anvil_type_u8(ctx)), size_type,
        anvil_type_ptr(ctx, slot_element), size_type
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 9u, false);
    anvil_value_t **values = NULL;
    anvil_value_t *initializer, *global;
    *element_out = element;
    *array_out = NULL;
    if (!element || builder->layout->class_count == 0u) return NULL;
    values = allocate_array(builder->allocator, builder->layout->class_count,
                            sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < builder->layout->class_count; index++) {
        const st_image_runtime_class_layout_t *layout_class =
            &builder->layout->classes[index];
        const st_class_graph_entity_t *entity = st_class_graph_entity(
            builder->graph, layout_class->graph_entity_id);
        size_t graph_index = layout_class->graph_entity_id - 1u;
        size_t method_count = builder->has_method_code
            ? builder->method_counts[graph_index] : 0u;
        anvil_value_t *fields_values[9] = {
            anvil_const_u32(ctx, layout_class->runtime_class_id),
            anvil_const_u32(ctx, layout_class->superclass_id),
            anvil_const_u32(ctx, layout_class->metaclass_id),
            anvil_const_u32(ctx, layout_class->default_shape_id),
            anvil_const_u32(ctx, layout_class->flags),
            string_address(builder, strings_global, strings_array,
                           entity->name),
            target_size_constant(builder, entity->name.length),
            method_count != 0u
                ? table_index_address(ctx, slots_global, slots_array,
                                      builder->method_offsets[graph_index])
                : anvil_const_null(ctx, fields[7]),
            target_size_constant(builder, method_count)
        };
        values[index] = anvil_const_struct(ctx, element, fields_values, 9u);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->layout->class_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_CLASS_DESCRIPTORS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_descriptor_pointer_table(
    emit_builder_t *builder, int symbol_index, anvil_value_t *descriptors,
    anvil_type_t *descriptor_element, anvil_type_t *descriptors_array,
    size_t count, anvil_type_t **pointer_element_out,
    anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *pointer = anvil_type_ptr(ctx, descriptor_element);
    anvil_value_t **values;
    anvil_value_t *initializer, *global;
    *pointer_element_out = pointer;
    *array_out = NULL;
    if (!pointer || count == 0u) return NULL;
    values = allocate_array(builder->allocator, count, sizeof(*values));
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < count; index++) {
        values[index] = table_index_address(ctx, descriptors,
                                            descriptors_array, index);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, pointer, values, count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[symbol_index], initializer, ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_runtime_descriptors(
    emit_builder_t *builder, anvil_value_t *class_pointers,
    anvil_type_t *class_pointer_element, anvil_type_t *class_pointers_array,
    anvil_value_t *shape_pointers, anvil_type_t *shape_pointer_element,
    anvil_type_t *shape_pointers_array, anvil_type_t **type_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[4] = {
        anvil_type_ptr(ctx, class_pointer_element), target_size_type(builder),
        anvil_type_ptr(ctx, shape_pointer_element), target_size_type(builder)
    };
    anvil_type_t *type = anvil_type_literal_struct(ctx, fields, 4u, false);
    anvil_value_t *values[4] = {
        table_first_address(ctx, class_pointers, class_pointers_array),
        target_size_constant(builder, builder->layout->class_count),
        table_first_address(ctx, shape_pointers, shape_pointers_array),
        target_size_constant(builder, builder->layout->shape_count)
    };
    anvil_value_t *initializer = type != NULL
        ? anvil_const_struct(ctx, type, values, 4u) : NULL;
    anvil_value_t *global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_RUNTIME_DESCRIPTORS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *type_out = type;
    return global;
}

static anvil_value_t *emit_entity_runtime_ids(
    emit_builder_t *builder, anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_value_t **values = allocate_array(
        builder->allocator, builder->layout->entity_count, sizeof(*values));
    anvil_value_t *initializer, *global;
    *array_out = NULL;
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0u; index < builder->layout->entity_count; index++) {
        values[index] = anvil_const_u32(
            ctx, builder->layout->entity_runtime_class_ids[index]);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, anvil_type_u32(ctx), values,
                                    builder->layout->entity_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_ENTITY_RUNTIME_IDS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static anvil_value_t *emit_runtime_layouts(
    emit_builder_t *builder, anvil_type_t **element_out,
    anvil_type_t **array_out)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[5] = {
        anvil_type_u32(ctx), anvil_type_u32(ctx), anvil_type_u32(ctx),
        anvil_type_u32(ctx), anvil_type_u32(ctx)
    };
    anvil_type_t *element = anvil_type_literal_struct(ctx, fields, 5u, false);
    anvil_value_t **values = allocate_array(
        builder->allocator, builder->layout->shape_count, sizeof(*values));
    anvil_value_t *initializer, *global;
    *element_out = element;
    *array_out = NULL;
    if (!values) {
        builder->allocation_failed = true;
        return NULL;
    }
    if (!element) return NULL;
    for (size_t index = 0u; index < builder->layout->shape_count; index++) {
        const st_image_runtime_shape_layout_t *shape =
            &builder->layout->shapes[index];
        anvil_value_t *fields_values[5] = {
            anvil_const_u32(ctx, shape->graph_entity_id),
            anvil_const_u32(ctx, shape->runtime_class_id),
            anvil_const_u32(ctx, shape->runtime_shape_id),
            anvil_const_u32(ctx, (uint32_t)shape->recipe),
            anvil_const_u32(ctx, shape->is_default
                ? ST_IMAGE_RUNTIME_LAYOUT_DEFAULT : 0u)
        };
        values[index] = anvil_const_struct(ctx, element, fields_values, 5u);
        if (!values[index]) goto fail;
    }
    initializer = anvil_const_array(ctx, element, values,
                                    builder->layout->shape_count);
    global = add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_RUNTIME_LAYOUTS], initializer,
        ANVIL_LINK_INTERNAL);
    if (global) *array_out = anvil_value_get_type(initializer);
    release(builder->allocator, values);
    return global;
fail:
    release(builder->allocator, values);
    return NULL;
}

static bool emit_descriptor(
    emit_builder_t *builder,
    anvil_value_t *strings_global, anvil_type_t *strings_array,
    anvil_value_t *selectors_global, anvil_type_t *selector_element,
    anvil_type_t *selectors_array,
    anvil_value_t *instance_global, anvil_type_t *instance_element,
    anvil_type_t *instance_array,
    anvil_value_t *class_global, anvil_type_t *class_element,
    anvil_type_t *class_array,
    anvil_value_t *methods_global, anvil_type_t *method_element,
    anvil_type_t *methods_array,
    anvil_value_t *entities_global, anvil_type_t *entity_element,
    anvil_type_t *entities_array,
    anvil_value_t *runtime_methods_global,
    anvil_type_t *runtime_method_element,
    anvil_type_t *runtime_methods_array,
    anvil_value_t *block_descriptors_global,
    anvil_type_t *block_descriptor_pointer,
    anvil_type_t *block_descriptors_array,
    anvil_value_t *globals_global, anvil_type_t *global_element,
    anvil_type_t *globals_array,
    anvil_value_t *literals_global, anvil_type_t *literal_element,
    anvil_type_t *literals_array,
    anvil_value_t *entity_runtime_ids_global,
    anvil_type_t *entity_runtime_ids_array,
    anvil_value_t *runtime_layouts_global,
    anvil_type_t *runtime_layout_element,
    anvil_type_t *runtime_layouts_array,
    anvil_value_t *runtime_descriptors_global,
    anvil_type_t *runtime_descriptors_type)
{
    anvil_ctx_t *ctx = builder->context;
    anvil_type_t *fields[37];
    anvil_value_t *values[37];
    anvil_type_t *descriptor;
    anvil_value_t *initializer;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    size_t class_count = 0u, metaclass_count = 0u, namespace_count = 0u;
    size_t index;
    if (arch == NULL || arch->ptr_size <= 0) return false;
    for (index = 0u; index < builder->graph->entity_count; index++) {
        switch (builder->graph->entities[index].kind) {
            case ST_CLASS_GRAPH_CLASS: class_count++; break;
            case ST_CLASS_GRAPH_METACLASS: metaclass_count++; break;
            case ST_CLASS_GRAPH_NAMESPACE: namespace_count++; break;
        }
    }
    fields[0] = anvil_type_u64(ctx);
    for (index = 1u; index < 24u; index++) fields[index] = anvil_type_u32(ctx);
    fields[24] = anvil_type_ptr(ctx, entity_element);
    fields[25] = anvil_type_ptr(ctx, method_element);
    fields[26] = anvil_type_ptr(ctx, selector_element);
    fields[27] = anvil_type_ptr(ctx, instance_element);
    fields[28] = anvil_type_ptr(ctx, class_element);
    fields[29] = anvil_type_ptr(ctx, anvil_type_u8(ctx));
    fields[30] = anvil_type_ptr(ctx, runtime_method_element);
    fields[31] = anvil_type_ptr(ctx, block_descriptor_pointer);
    fields[32] = anvil_type_ptr(ctx, global_element);
    fields[33] = anvil_type_ptr(ctx, literal_element);
    fields[34] = anvil_type_ptr(ctx, anvil_type_u32(ctx));
    fields[35] = anvil_type_ptr(ctx, runtime_layout_element);
    fields[36] = anvil_type_ptr(ctx, runtime_descriptors_type);
    descriptor = anvil_type_literal_struct(ctx, fields, 37u, false);
    values[0] = anvil_const_u64(ctx, ST_IMAGE_METADATA_MAGIC);
    values[1] = anvil_const_u32(ctx, ST_IMAGE_METADATA_ABI_VERSION);
    values[2] = anvil_const_u32(
        ctx, ST_IMAGE_METADATA_FLAG_TYPED_RELOCATIONS |
             (builder->has_method_code
                  ? ST_IMAGE_METADATA_FLAG_METHOD_CODE |
                        ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS |
                        (builder->root_map_count != 0u
                             ? ST_IMAGE_METADATA_FLAG_ROOT_MAPS : 0u) |
                        (builder->block_count != 0u
                             ? ST_IMAGE_METADATA_FLAG_BLOCK_CODE : 0u)
                  : ST_IMAGE_METADATA_FLAG_METADATA_ONLY) |
             ((builder->global_count != 0u
                    || builder->string_literal_count != 0u)
                  ? ST_IMAGE_METADATA_FLAG_IMAGE_RUNTIME_TABLES : 0u) |
             ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS);
    values[3] = anvil_const_u32(ctx, (uint32_t)arch->ptr_size);
    values[4] = anvil_const_u32(ctx, (uint32_t)arch->endian);
    values[5] = anvil_const_u32(ctx, (uint32_t)builder->bundle->image_count);
    values[6] = anvil_const_u32(ctx,
        (uint32_t)(builder->bundle->count - builder->bundle->image_count));
    values[7] = anvil_const_u32(ctx, (uint32_t)builder->graph->entity_count);
    values[8] = anvil_const_u32(ctx, (uint32_t)class_count);
    values[9] = anvil_const_u32(ctx, (uint32_t)metaclass_count);
    values[10] = anvil_const_u32(ctx, (uint32_t)namespace_count);
    values[11] = anvil_const_u32(ctx, (uint32_t)builder->graph->method_count);
    values[12] = anvil_const_u32(ctx,
                                 (uint32_t)st_selector_count(builder->selectors));
    values[13] = anvil_const_u32(ctx,
        (uint32_t)builder->graph->instance_slot_count);
    values[14] = anvil_const_u32(ctx,
        (uint32_t)builder->graph->class_variable_count);
    values[15] = anvil_const_u32(ctx, (uint32_t)builder->strings.byte_count);
    values[16] = anvil_const_u32(ctx, (uint32_t)builder->block_count);
    values[17] = anvil_const_u32(ctx,
                                (uint32_t)builder->block_capture_count);
    values[18] = anvil_const_u32(ctx, (uint32_t)builder->global_count);
    values[19] = anvil_const_u32(ctx,
                                (uint32_t)builder->string_literal_count);
    values[20] = anvil_const_u32(ctx,
                                (uint32_t)builder->string_literal_bytes);
    values[21] = anvil_const_u32(ctx, (uint32_t)builder->layout->class_count);
    values[22] = anvil_const_u32(ctx, (uint32_t)builder->layout->shape_count);
    values[23] = anvil_const_u32(ctx, (uint32_t)builder->layout->shape_count);
    values[24] = table_or_null(builder, entities_global, entities_array,
                               entity_element);
    values[25] = table_or_null(builder, methods_global, methods_array,
                               method_element);
    values[26] = table_or_null(builder, selectors_global, selectors_array,
                               selector_element);
    values[27] = table_or_null(builder, instance_global, instance_array,
                               instance_element);
    values[28] = table_or_null(builder, class_global, class_array,
                               class_element);
    values[29] = table_first_address(ctx, strings_global, strings_array);
    values[30] = table_or_null(builder, runtime_methods_global,
                               runtime_methods_array,
                               runtime_method_element);
    values[31] = table_or_null(builder, block_descriptors_global,
                               block_descriptors_array,
                               block_descriptor_pointer);
    values[32] = table_or_null(builder, globals_global, globals_array,
                               global_element);
    values[33] = table_or_null(builder, literals_global, literals_array,
                               literal_element);
    values[34] = table_first_address(ctx, entity_runtime_ids_global,
                                     entity_runtime_ids_array);
    values[35] = table_first_address(ctx, runtime_layouts_global,
                                     runtime_layouts_array);
    values[36] = anvil_const_symbol_addr(runtime_descriptors_global);
    initializer = descriptor != NULL
        ? anvil_const_struct(ctx, descriptor, values, 37u) : NULL;
    return add_initialized_global(builder->module,
        builder->symbol_names[SYMBOL_DESCRIPTOR], initializer,
        ANVIL_LINK_EXTERNAL) != NULL;
}

static st_image_emit_status_t map_build_failure(emit_builder_t *builder)
{
    anvil_error_t error;
    if (builder->strings.overflow) return ST_IMAGE_EMIT_ERR_OVERFLOW;
    if (builder->allocation_failed) return ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
    error = anvil_ctx_get_last_error(builder->context);
    return error == ANVIL_OK || error == ANVIL_ERR_NOMEM
        ? ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY : ST_IMAGE_EMIT_ERR_ANVIL;
}

static void builder_destroy(emit_builder_t *builder, bool destroy_module)
{
    size_t index;
    if (builder == NULL) return;
    if (destroy_module && builder->module != NULL)
        anvil_module_destroy(builder->module);
    string_pool_destroy(&builder->strings);
    release(builder->allocator, builder->method_selector_ids);
    release(builder->allocator, builder->method_order);
    release(builder->allocator, builder->method_offsets);
    release(builder->allocator, builder->method_counts);
    release(builder->allocator, builder->method_artifacts);
    if (builder->artifact_symbol_names) {
        for (index = 0u; index < builder->graph->method_count; index++)
            release(builder->allocator,
                    builder->artifact_symbol_names[index]);
    }
    release(builder->allocator, builder->artifact_symbol_names);
    release(builder->allocator, builder->method_code_externs);
    release(builder->allocator, builder->root_map_offsets);
    release(builder->allocator, builder->blocks);
    st_image_layout_result_destroy(&builder->owned_layout);
    for (index = 0u; index < SYMBOL_COUNT; index++)
        release(builder->allocator, builder->symbol_names[index]);
    memset(builder, 0, sizeof(*builder));
}

void st_image_emit_result_init(st_image_emit_result_t *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void st_image_emit_result_destroy(st_image_emit_result_t *result)
{
    if (result == NULL) return;
    if (result->module != NULL) anvil_module_destroy(result->module);
    memset(result, 0, sizeof(*result));
}

static bool emit_result_is_empty(const st_image_emit_result_t *result)
{
    return result != NULL && result->status == ST_IMAGE_EMIT_OK
        && result->module == NULL && result->source_count == 0u
        && result->image_source_count == 0u && result->entity_count == 0u
        && result->method_count == 0u && result->selector_count == 0u
        && result->instance_slot_count == 0u
        && result->class_variable_count == 0u && result->root_map_count == 0u
        && result->root_bitmap_word_count == 0u && result->string_bytes == 0u
        && result->block_count == 0u && result->block_capture_count == 0u
        && result->block_root_map_count == 0u
        && result->global_count == 0u
        && result->string_literal_count == 0u
        && result->string_literal_bytes == 0u
        && result->runtime_class_count == 0u
        && result->runtime_shape_count == 0u
        && !result->has_method_code;
}

st_image_emit_status_t st_image_emit_metadata(
    st_image_emit_result_t *result, anvil_ctx_t *context,
    const st_source_bundle_t *bundle, const st_class_graph_result_t *graph,
    const st_image_emit_options_t *options)
{
    emit_builder_t builder;
    st_image_emit_status_t status;
    st_image_emit_allocator_t allocator = {0};
    const char *module_name = "smalltalk.image";
    const char *symbol_prefix = "st_image";
    anvil_value_t *strings_global, *selectors_global, *instance_global;
    anvil_value_t *class_global, *root_bitmaps_global, *root_maps_global;
    anvil_value_t *runtime_methods_global, *block_captures_global;
    anvil_value_t *block_descriptors_global, *methods_global, *entities_global;
    anvil_value_t *globals_global, *literal_blob_global, *literals_global;
    anvil_value_t *runtime_bindings_global, *method_entries_global;
    anvil_value_t *method_slots_global, *shape_bitmaps_global;
    anvil_value_t *class_descriptors_global, *shape_descriptors_global;
    anvil_value_t *class_pointers_global, *shape_pointers_global;
    anvil_value_t *runtime_descriptors_global, *entity_runtime_ids_global;
    anvil_value_t *runtime_layouts_global;
    anvil_type_t *strings_array = NULL;
    anvil_type_t *selector_element = NULL, *selectors_array = NULL;
    anvil_type_t *instance_element = NULL, *instance_array = NULL;
    anvil_type_t *class_element = NULL, *class_array = NULL;
    anvil_type_t *root_bitmaps_array = NULL;
    anvil_type_t *root_map_element = NULL, *root_maps_array = NULL;
    anvil_type_t *runtime_method_element = NULL;
    anvil_type_t *runtime_methods_array = NULL;
    anvil_type_t *block_capture_element = NULL;
    anvil_type_t *block_captures_array = NULL;
    anvil_type_t *block_descriptor_pointer = NULL;
    anvil_type_t *block_descriptors_array = NULL;
    anvil_type_t *method_element = NULL, *methods_array = NULL;
    anvil_type_t *entity_element = NULL, *entities_array = NULL;
    anvil_type_t *global_element = NULL, *globals_array = NULL;
    anvil_type_t *literal_blob_array = NULL;
    anvil_type_t *literal_element = NULL, *literals_array = NULL;
    anvil_type_t *runtime_binding_element = NULL;
    anvil_type_t *runtime_bindings_array = NULL;
    anvil_type_t *method_entry_element = NULL, *method_entries_array = NULL;
    anvil_type_t *method_slot_element = NULL, *method_slots_array = NULL;
    anvil_type_t *shape_bitmaps_array = NULL;
    anvil_type_t *class_descriptor_element = NULL;
    anvil_type_t *class_descriptors_array = NULL;
    anvil_type_t *shape_descriptor_element = NULL;
    anvil_type_t *shape_descriptors_array = NULL;
    anvil_type_t *class_pointer_element = NULL, *class_pointers_array = NULL;
    anvil_type_t *shape_pointer_element = NULL, *shape_pointers_array = NULL;
    anvil_type_t *runtime_descriptors_type = NULL;
    anvil_type_t *entity_runtime_ids_array = NULL;
    anvil_type_t *runtime_layout_element = NULL;
    anvil_type_t *runtime_layouts_array = NULL;
    if (!emit_result_is_empty(result)) {
        return ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT;
    }
    status = validate_inputs(context, bundle, graph, options);
    if (status != ST_IMAGE_EMIT_OK) {
        result->status = status;
        return status;
    }
    if (options != NULL) {
        allocator = options->allocator;
        if (options->module_name != NULL) module_name = options->module_name;
        if (options->symbol_prefix != NULL) symbol_prefix = options->symbol_prefix;
    }
    if (allocator.allocate == NULL) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
    }
    if (module_name[0] == '\0' || !valid_symbol_prefix(symbol_prefix)) {
        result->status = ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    memset(&builder, 0, sizeof(builder));
    st_image_layout_result_init(&builder.owned_layout);
    builder.allocator = allocator;
    builder.context = context;
    builder.bundle = bundle;
    builder.graph = graph;
    builder.selectors = options->selectors;
    builder.strings.allocator = allocator;
    builder.method_selector_ids = allocate_array(
        allocator, graph->method_count, sizeof(*builder.method_selector_ids));
    builder.method_order = allocate_array(
        allocator, graph->method_count, sizeof(*builder.method_order));
    builder.method_offsets = allocate_array(
        allocator, graph->entity_count, sizeof(*builder.method_offsets));
    builder.method_counts = allocate_array(
        allocator, graph->entity_count, sizeof(*builder.method_counts));
    builder.method_artifacts = allocate_array(
        allocator, graph->method_count, sizeof(*builder.method_artifacts));
    builder.artifact_symbol_names = allocate_array(
        allocator, graph->method_count,
        sizeof(*builder.artifact_symbol_names));
    builder.method_code_externs = allocate_array(
        allocator, graph->method_count,
        sizeof(*builder.method_code_externs));
    builder.root_map_offsets = allocate_array(
        allocator, graph->method_count, sizeof(*builder.root_map_offsets));
    if ((graph->method_count != 0u
            && (builder.method_selector_ids == NULL
                || builder.method_order == NULL
                || builder.method_artifacts == NULL
                || builder.artifact_symbol_names == NULL
                || builder.method_code_externs == NULL
                || builder.root_map_offsets == NULL))
            || builder.method_offsets == NULL || builder.method_counts == NULL
            || !build_symbol_names(&builder, symbol_prefix)) {
        builder_destroy(&builder, true);
        result->status = ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    status = prepare_layout(&builder, options);
    if (status != ST_IMAGE_EMIT_OK) {
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    status = prepare_method_artifacts(&builder, options);
    if (status != ST_IMAGE_EMIT_OK) {
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    status = prepare_image_runtime_artifacts(&builder, options);
    if (status != ST_IMAGE_EMIT_OK) {
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    status = prepare_block_artifacts(&builder);
    if (status != ST_IMAGE_EMIT_OK) {
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    if (!collect_strings_and_selectors(&builder)
            || !build_method_order(&builder)) {
        status = map_build_failure(&builder);
        if (anvil_ctx_get_last_error(context) != ANVIL_ERR_NOMEM)
            status = ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY;
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    anvil_ctx_clear_error(context);
    builder.module = anvil_module_create(context, module_name);
    strings_global = builder.module != NULL
        ? emit_string_blob(&builder, &strings_array) : NULL;
    selectors_global = strings_global != NULL
        ? emit_selector_table(&builder, strings_global, strings_array,
                              &selector_element, &selectors_array) : NULL;
    globals_global = strings_global != NULL
        ? emit_global_table(&builder, strings_global, strings_array,
                            &global_element, &globals_array) : NULL;
    if (builder.global_count == 0u)
        global_element = anvil_type_u8(builder.context);
    literal_blob_global = (builder.string_literal_count != 0u)
        ? emit_literal_blob(&builder, &literal_blob_array) : NULL;
    literals_global = (builder.string_literal_count != 0u
            && literal_blob_global != NULL)
        ? emit_string_literal_table(&builder, literal_blob_global,
                                    literal_blob_array, &literal_element,
                                    &literals_array) : NULL;
    if (builder.string_literal_count == 0u)
        literal_element = anvil_type_u8(builder.context);
    instance_global = (selectors_global != NULL
                       || st_selector_count(builder.selectors) == 0u)
        ? emit_slot_table(&builder, graph->instance_slots,
                          graph->instance_slot_count, SYMBOL_INSTANCE_SLOTS,
                          strings_global, strings_array, &instance_element,
                          &instance_array) : NULL;
    class_global = ((selectors_global != NULL
                    || st_selector_count(builder.selectors) == 0u)
                    && (graph->instance_slot_count == 0u
                        || instance_global != NULL))
        ? emit_slot_table(&builder, graph->class_variables,
                          graph->class_variable_count, SYMBOL_CLASS_VARIABLES,
                          strings_global, strings_array, &class_element,
                          &class_array) : NULL;
    root_bitmaps_global = ((graph->class_variable_count == 0u ||
                            class_global != NULL) && builder.has_method_code)
        ? emit_root_bitmap_table(&builder, &root_bitmaps_array) : NULL;
    root_maps_global = ((graph->class_variable_count == 0u ||
                         class_global != NULL) &&
                        (!builder.has_method_code ||
                         builder.root_bitmap_word_count == 0u ||
                         root_bitmaps_global != NULL))
        ? emit_root_map_table(&builder, root_bitmaps_global,
                              root_bitmaps_array, &root_map_element,
                              &root_maps_array) : NULL;
    runtime_methods_global = ((builder.root_map_count == 0u
                               || root_maps_global != NULL)
                              && builder.has_method_code)
        ? emit_runtime_method_table(
              &builder, strings_global, strings_array, root_maps_global,
              root_map_element, root_maps_array, &runtime_method_element,
              &runtime_methods_array)
        : NULL;
    if (!builder.has_method_code)
        runtime_method_element = anvil_type_u8(builder.context);
    block_captures_global = builder.has_method_code
        ? emit_block_capture_table(
              &builder, &block_capture_element, &block_captures_array)
        : NULL;
    if (!builder.has_method_code)
        block_descriptor_pointer = anvil_type_ptr(
            builder.context, anvil_type_u8(builder.context));
    block_descriptors_global = (builder.has_method_code
            && runtime_method_element != NULL
            && (builder.block_capture_count == 0u
                || block_captures_global != NULL))
        ? emit_block_descriptors(
              &builder, strings_global, strings_array, root_maps_global,
              root_map_element, root_maps_array, block_captures_global,
              block_capture_element, block_captures_array,
              runtime_method_element, &block_descriptor_pointer,
              &block_descriptors_array)
        : NULL;
    methods_global = ((selectors_global != NULL
                    || st_selector_count(builder.selectors) == 0u)
                    && (graph->instance_slot_count == 0u
                        || instance_global != NULL)
                    && (graph->class_variable_count == 0u
                        || class_global != NULL)
                    && (builder.root_bitmap_word_count == 0u
                        || root_bitmaps_global != NULL)
                    && (builder.root_map_count == 0u
                        || root_maps_global != NULL)
                    && (!builder.has_method_code
                        || runtime_methods_global != NULL)
                    && (builder.block_count == 0u
                        || block_descriptors_global != NULL))
        ? emit_method_table(&builder, strings_global, strings_array,
                            root_maps_global, root_map_element, root_maps_array,
                            runtime_methods_global, runtime_method_element,
                            runtime_methods_array,
                            &method_element, &methods_array) : NULL;
    entities_global = ((selectors_global != NULL
                    || st_selector_count(builder.selectors) == 0u)
                    && (graph->instance_slot_count == 0u
                        || instance_global != NULL)
                    && (graph->class_variable_count == 0u
                        || class_global != NULL)
                    && (graph->method_count == 0u || methods_global != NULL))
        ? emit_entity_table(&builder, strings_global, strings_array,
                            &entity_element, &entities_array) : NULL;
    runtime_bindings_global = builder.has_method_code
        ? emit_runtime_bindings(
            &builder, runtime_methods_global, runtime_method_element,
            runtime_methods_array, &runtime_binding_element,
            &runtime_bindings_array)
        : NULL;
    method_entries_global = builder.has_method_code
            && (builder.graph->method_count == 0u
                || runtime_bindings_global != NULL)
        ? emit_method_entries(
            &builder, runtime_bindings_global, runtime_binding_element,
            runtime_bindings_array, &method_entry_element,
            &method_entries_array)
        : NULL;
    method_slots_global = builder.has_method_code
            && (builder.graph->method_count == 0u
                || method_entries_global != NULL)
        ? emit_method_slots(
            &builder, method_entries_global, method_entry_element,
            method_entries_array, &method_slot_element, &method_slots_array)
        : NULL;
    if (!builder.has_method_code)
        method_slot_element = anvil_type_u8(builder.context);
    shape_bitmaps_global = emit_shape_pointer_bitmaps(
        &builder, &shape_bitmaps_array);
    shape_descriptors_global =
        (builder.layout->pointer_bitmap_word_count == 0u
            || shape_bitmaps_global != NULL)
        ? emit_shape_descriptors(
            &builder, shape_bitmaps_global, shape_bitmaps_array,
            &shape_descriptor_element, &shape_descriptors_array)
        : NULL;
    class_descriptors_global =
        (!builder.has_method_code || builder.graph->method_count == 0u
            || method_slots_global != NULL)
        ? emit_class_descriptors(
            &builder, strings_global, strings_array, method_slots_global,
            method_slot_element, method_slots_array,
            &class_descriptor_element, &class_descriptors_array)
        : NULL;
    class_pointers_global = class_descriptors_global != NULL
        ? emit_descriptor_pointer_table(
            &builder, SYMBOL_CLASS_POINTERS, class_descriptors_global,
            class_descriptor_element, class_descriptors_array,
            builder.layout->class_count, &class_pointer_element,
            &class_pointers_array)
        : NULL;
    shape_pointers_global = shape_descriptors_global != NULL
        ? emit_descriptor_pointer_table(
            &builder, SYMBOL_SHAPE_POINTERS, shape_descriptors_global,
            shape_descriptor_element, shape_descriptors_array,
            builder.layout->shape_count, &shape_pointer_element,
            &shape_pointers_array)
        : NULL;
    runtime_descriptors_global = class_pointers_global != NULL
            && shape_pointers_global != NULL
        ? emit_runtime_descriptors(
            &builder, class_pointers_global, class_pointer_element,
            class_pointers_array, shape_pointers_global,
            shape_pointer_element, shape_pointers_array,
            &runtime_descriptors_type)
        : NULL;
    entity_runtime_ids_global = emit_entity_runtime_ids(
        &builder, &entity_runtime_ids_array);
    runtime_layouts_global = emit_runtime_layouts(
        &builder, &runtime_layout_element, &runtime_layouts_array);
    if (entities_global == NULL
            || (builder.global_count != 0u && globals_global == NULL)
            || (builder.string_literal_count != 0u
                && literals_global == NULL)
            || (builder.has_method_code && builder.graph->method_count != 0u
                && (runtime_bindings_global == NULL
                    || method_entries_global == NULL
                    || method_slots_global == NULL))
            || shape_descriptors_global == NULL
            || class_descriptors_global == NULL
            || runtime_descriptors_global == NULL
            || entity_runtime_ids_global == NULL
            || runtime_layouts_global == NULL
            || !emit_descriptor(&builder, strings_global, strings_array,
                 selectors_global, selector_element, selectors_array,
                 instance_global, instance_element, instance_array,
                 class_global, class_element, class_array,
                 methods_global, method_element, methods_array,
                 entities_global, entity_element, entities_array,
                 runtime_methods_global, runtime_method_element,
                 runtime_methods_array, block_descriptors_global,
                 block_descriptor_pointer, block_descriptors_array,
                 globals_global, global_element, globals_array,
                 literals_global, literal_element, literals_array,
                 entity_runtime_ids_global, entity_runtime_ids_array,
                 runtime_layouts_global, runtime_layout_element,
                 runtime_layouts_array, runtime_descriptors_global,
                 runtime_descriptors_type)) {
        status = map_build_failure(&builder);
        builder_destroy(&builder, true);
        result->status = status;
        return status;
    }
    {
        char verify_error[256];
        if (!anvil_module_verify(builder.module, verify_error,
                                 sizeof(verify_error))) {
            builder_destroy(&builder, true);
            result->status = ST_IMAGE_EMIT_ERR_ANVIL;
            return result->status;
        }
    }
    result->module = builder.module;
    result->source_count = bundle->count;
    result->image_source_count = bundle->image_count;
    result->entity_count = graph->entity_count;
    result->method_count = graph->method_count;
    result->selector_count = st_selector_count(builder.selectors);
    result->instance_slot_count = graph->instance_slot_count;
    result->class_variable_count = graph->class_variable_count;
    result->root_map_count = builder.root_map_count;
    result->root_bitmap_word_count = builder.root_bitmap_word_count;
    result->block_count = builder.block_count;
    result->block_capture_count = builder.block_capture_count;
    result->block_root_map_count = builder.block_root_map_count;
    result->global_count = builder.global_count;
    result->string_literal_count = builder.string_literal_count;
    result->string_literal_bytes = builder.string_literal_bytes;
    result->runtime_class_count = builder.layout->class_count;
    result->runtime_shape_count = builder.layout->shape_count;
    result->string_bytes = builder.strings.byte_count;
    result->has_method_code = builder.has_method_code;
    result->status = ST_IMAGE_EMIT_OK;
    builder.module = NULL;
    builder_destroy(&builder, false);
    return ST_IMAGE_EMIT_OK;
}

const char *st_image_emit_status_string(st_image_emit_status_t status)
{
    switch (status) {
        case ST_IMAGE_EMIT_OK: return "ok";
        case ST_IMAGE_EMIT_ERR_INVALID_ARGUMENT: return "invalid argument";
        case ST_IMAGE_EMIT_ERR_INVALID_BUNDLE: return "invalid source bundle";
        case ST_IMAGE_EMIT_ERR_INVALID_GRAPH: return "invalid class graph";
        case ST_IMAGE_EMIT_ERR_OUT_OF_MEMORY: return "out of memory";
        case ST_IMAGE_EMIT_ERR_OVERFLOW: return "overflow";
        case ST_IMAGE_EMIT_ERR_UNSUPPORTED_TARGET: return "unsupported target";
        case ST_IMAGE_EMIT_ERR_INVALID_METHOD_ARTIFACT:
            return "invalid AOT method artifact";
        case ST_IMAGE_EMIT_ERR_INVALID_BLOCK_ARTIFACT:
            return "invalid AOT block artifact";
        case ST_IMAGE_EMIT_ERR_INVALID_GLOBAL_ARTIFACT:
            return "invalid image-runtime global artifact";
        case ST_IMAGE_EMIT_ERR_INVALID_LITERAL_ARTIFACT:
            return "invalid image-runtime String literal artifact";
        case ST_IMAGE_EMIT_ERR_UNSUPPORTED_BLOCK_FEATURE:
            return "unsupported AOT block feature";
        case ST_IMAGE_EMIT_ERR_METHOD_CODE_UNAVAILABLE:
            return "method code is unavailable in metadata-only emission";
        case ST_IMAGE_EMIT_ERR_ANVIL: return "Anvil IR construction failed";
    }
    return "unknown image emission status";
}
