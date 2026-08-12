#include "st_class_graph.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    GRAPH_EVENT_DEFINITION,
    GRAPH_EVENT_EXTENSION
} graph_event_kind_t;

typedef struct {
    graph_event_kind_t kind;
    const st_ast_node_t *node;
    st_class_graph_id_t namespace_id;
    st_class_graph_id_t target_id;
    size_t unit_index;
    size_t order;
} graph_event_t;

typedef struct {
    const st_ast_node_t *node;
    size_t unit_index;
    size_t order;
    bool layout_done;
    bool cycle_reported;
    unsigned visit_state;
} graph_entity_work_t;

typedef struct {
    uint64_t hash;
    st_class_graph_method_id_t id;
} graph_method_bucket_t;

typedef struct {
    st_class_graph_allocator_t allocator;
    size_t entity_capacity;
    size_t method_capacity;
    size_t instance_slot_capacity;
    size_t class_variable_capacity;
    size_t catalog_capacity;
    size_t diagnostic_capacity;
    graph_entity_work_t *entity_work;
    size_t entity_work_capacity;
    st_class_graph_id_t *entity_buckets;
    size_t entity_bucket_capacity;
    graph_event_t *events;
    size_t event_count;
    size_t event_capacity;
    st_class_graph_id_t *path;
    size_t path_capacity;
    graph_method_bucket_t *method_buckets;
    size_t method_bucket_capacity;
    size_t method_index_probe_count;
    size_t *scope_global_heads;
    size_t *scope_global_tails;
    size_t *scope_visible_counts;
    size_t scope_capacity;
    size_t *global_next;
    size_t global_next_capacity;
    st_sema_external_t **catalog_views;
    size_t *catalog_view_counts;
    size_t catalog_view_capacity;
    size_t compatibility_view_count;
    size_t compatibility_entry_count;
    atomic_flag catalog_lock;
} graph_impl_t;

typedef struct {
    st_class_graph_result_t *result;
    graph_impl_t *impl;
    const st_ast_unit_t *const *units;
    size_t unit_count;
    size_t next_order;
} graph_builder_t;

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

static void set_status(graph_builder_t *builder, st_class_graph_status_t status)
{
    if (builder->result->status == ST_CLASS_GRAPH_OK)
        builder->result->status = status;
}

static void release_memory(graph_impl_t *impl, void *pointer)
{
    if (pointer != NULL)
        impl->allocator.deallocate(impl->allocator.user, pointer);
}

static void *allocate_zeroed(graph_builder_t *builder, size_t count,
                             size_t element_size)
{
    size_t bytes;
    void *memory;
    if (count == 0u || element_size == 0u) {
        set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    if (count > SIZE_MAX / element_size) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return NULL;
    }
    bytes = count * element_size;
    memory = builder->impl->allocator.allocate(
        builder->impl->allocator.user, bytes);
    if (memory == NULL) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY);
        return NULL;
    }
    memset(memory, 0, bytes);
    return memory;
}

static bool reserve_array(graph_builder_t *builder, void **array,
                          size_t *capacity, size_t count, size_t required,
                          size_t element_size)
{
    size_t new_capacity;
    void *replacement;
    if (required <= *capacity) {
        if (required != 0u && *array == NULL) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
        return true;
    }
    new_capacity = *capacity == 0u ? 8u : *capacity;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2u) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        new_capacity *= 2u;
    }
    replacement = allocate_zeroed(builder, new_capacity, element_size);
    if (replacement == NULL) return false;
    if (count != 0u) memcpy(replacement, *array, count * element_size);
    release_memory(builder->impl, *array);
    *array = replacement;
    *capacity = new_capacity;
    return true;
}

static bool string_valid(st_ast_string_t value)
{
    return value.data != NULL && value.length != 0u;
}

static bool string_equal(st_ast_string_t left, st_ast_string_t right)
{
    return left.length == right.length
        && (left.length == 0u
            || memcmp(left.data, right.data, left.length) == 0);
}

static bool string_is(st_ast_string_t value, const char *literal)
{
    size_t length = strlen(literal);
    return value.length == length
        && (length == 0u || memcmp(value.data, literal, length) == 0);
}

static bool list_valid(const st_ast_list_t *list)
{
    return list != NULL && (list->count == 0u || list->items != NULL)
        && list->count <= list->capacity;
}

static st_ast_string_t node_name(const st_ast_node_t *node)
{
    if (node != NULL && node->kind == ST_AST_VARIABLE)
        return node->as.variable.name;
    return (st_ast_string_t){0};
}

static st_class_graph_origin_t origin_for(graph_builder_t *builder,
                                          size_t unit_index,
                                          const st_ast_node_t *node)
{
    st_class_graph_origin_t origin;
    memset(&origin, 0, sizeof(origin));
    origin.unit_index = unit_index;
    if (unit_index < builder->unit_count && builder->units[unit_index] != NULL)
        origin.source_name = builder->units[unit_index]->source_name;
    if (node != NULL) origin.span = node->span;
    return origin;
}

static bool append_diagnostic(graph_builder_t *builder,
                              st_class_graph_diagnostic_code_t code,
                              st_ast_string_t name, size_t unit_index,
                              const st_ast_node_t *node,
                              const st_class_graph_origin_t *related)
{
    st_class_graph_result_t *result = builder->result;
    st_class_graph_diagnostic_t *diagnostic;
    if (result->diagnostic_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!reserve_array(builder, (void **)&result->diagnostics,
                       &builder->impl->diagnostic_capacity,
                       result->diagnostic_count,
                       result->diagnostic_count + 1u,
                       sizeof(*result->diagnostics))) return false;
    if (result->diagnostics == NULL) {
        set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
        return false;
    }
    diagnostic = &result->diagnostics[result->diagnostic_count++];
    diagnostic->code = code;
    diagnostic->name = name;
    diagnostic->origin = origin_for(builder, unit_index, node);
    if (related != NULL) {
        diagnostic->related_origin = *related;
        diagnostic->has_related_origin = true;
    }
    return true;
}

static st_class_graph_entity_t *entity_mut(graph_builder_t *builder,
                                           st_class_graph_id_t id)
{
    if (id == ST_CLASS_GRAPH_INVALID_ID
            || (size_t)id > builder->result->entity_count) return NULL;
    return &builder->result->entities[id - 1u];
}

static graph_entity_work_t *work_mut(graph_builder_t *builder,
                                     st_class_graph_id_t id)
{
    if (id == ST_CLASS_GRAPH_INVALID_ID
            || (size_t)id > builder->result->entity_count) return NULL;
    return &builder->impl->entity_work[id - 1u];
}

static uint64_t entity_name_hash(st_class_graph_id_t namespace_id,
                                 st_ast_string_t name)
{
    uint64_t hash = UINT64_C(14695981039346656037)
        ^ (uint64_t)namespace_id;
    size_t index;
    hash *= UINT64_C(1099511628211);
    for (index = 0u; index < name.length; index++) {
        hash ^= (unsigned char)name.data[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    hash *= UINT64_C(0xd6e8feb86659fd93);
    hash ^= hash >> 32;
    return hash;
}

static size_t probe_distance(size_t index, uint64_t hash, size_t mask)
{
    return (index - ((size_t)hash & mask)) & mask;
}

static void entity_index_insert_unchecked(graph_builder_t *builder,
                                          st_class_graph_id_t id)
{
    const st_class_graph_entity_t *entity =
        &builder->result->entities[id - 1u];
    size_t mask = builder->impl->entity_bucket_capacity - 1u;
    uint64_t hash = entity_name_hash(entity->namespace_id, entity->name);
    size_t index = (size_t)hash & mask;
    size_t distance = 0u;
    for (;;) {
        st_class_graph_id_t existing_id =
            builder->impl->entity_buckets[index];
        if (existing_id == ST_CLASS_GRAPH_INVALID_ID) {
            builder->impl->entity_buckets[index] = id;
            return;
        }
        {
            const st_class_graph_entity_t *existing =
                &builder->result->entities[existing_id - 1u];
            uint64_t existing_hash = entity_name_hash(
                existing->namespace_id, existing->name);
            size_t existing_distance = probe_distance(
                index, existing_hash, mask);
            if (existing_distance < distance) {
                builder->impl->entity_buckets[index] = id;
                id = existing_id;
                distance = existing_distance;
            }
        }
        index = (index + 1u) & mask;
        distance++;
    }
}

static bool entity_index_rehash(graph_builder_t *builder,
                                size_t new_capacity)
{
    st_class_graph_id_t *old = builder->impl->entity_buckets;
    size_t index;
    if (new_capacity < 16u
            || (new_capacity & (new_capacity - 1u)) != 0u
            || new_capacity > SIZE_MAX / sizeof(*old)) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    builder->impl->entity_buckets = allocate_zeroed(
        builder, new_capacity, sizeof(*old));
    if (builder->impl->entity_buckets == NULL) {
        builder->impl->entity_buckets = old;
        return false;
    }
    builder->impl->entity_bucket_capacity = new_capacity;
    for (index = 0u; index < builder->result->entity_count; index++) {
        if (builder->result->entities[index].kind != ST_CLASS_GRAPH_METACLASS)
            entity_index_insert_unchecked(builder,
                builder->result->entities[index].id);
    }
    release_memory(builder->impl, old);
    return true;
}

static bool entity_index_reserve(graph_builder_t *builder, size_t extra)
{
    size_t required;
    size_t capacity = builder->impl->entity_bucket_capacity;
    if (extra > SIZE_MAX - builder->result->entity_count) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    required = builder->result->entity_count + extra;
    if (capacity == 0u) capacity = 16u;
    while (required > capacity - capacity / 4u) {
        if (capacity > SIZE_MAX / 2u) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        capacity *= 2u;
    }
    return capacity == builder->impl->entity_bucket_capacity
        || entity_index_rehash(builder, capacity);
}

static const st_class_graph_entity_t *find_exact_index(
    const st_class_graph_result_t *result, const graph_impl_t *impl,
    st_class_graph_id_t namespace_id, st_ast_string_t name)
{
    size_t capacity = impl->entity_bucket_capacity;
    size_t mask;
    uint64_t hash;
    size_t index;
    size_t distance = 0u;
    if (capacity == 0u || impl->entity_buckets == NULL) return NULL;
    mask = capacity - 1u;
    hash = entity_name_hash(namespace_id, name);
    index = (size_t)hash & mask;
    for (;;) {
        st_class_graph_id_t id = impl->entity_buckets[index];
        const st_class_graph_entity_t *entity;
        uint64_t existing_hash;
        if (id == ST_CLASS_GRAPH_INVALID_ID) return NULL;
        if ((size_t)id > result->entity_count) return NULL;
        entity = &result->entities[id - 1u];
        existing_hash = entity_name_hash(entity->namespace_id, entity->name);
        if (probe_distance(index, existing_hash, mask) < distance) return NULL;
        if (entity->namespace_id == namespace_id
                && string_equal(entity->name, name)) return entity;
        index = (index + 1u) & mask;
        if (++distance == capacity) return NULL;
    }
}

static const st_class_graph_entity_t *find_exact(const graph_builder_t *builder,
                                                  st_class_graph_id_t namespace_id,
                                                  st_ast_string_t name)
{
    return find_exact_index(builder->result, builder->impl, namespace_id,
                            name);
}

static const st_class_graph_entity_t *find_visible_index(
    const st_class_graph_result_t *result, const graph_impl_t *impl,
    st_class_graph_id_t namespace_id, st_ast_string_t name)
{
    st_class_graph_id_t scope = namespace_id;
    for (;;) {
        const st_class_graph_entity_t *found = find_exact_index(
            result, impl, scope, name);
        if (found != NULL) return found;
        if (scope == ST_CLASS_GRAPH_INVALID_ID ||
            (size_t)scope > result->entity_count)
            break;
        scope = result->entities[scope - 1u].namespace_id;
    }
    return NULL;
}

static const st_class_graph_entity_t *find_visible(
    const graph_builder_t *builder, st_class_graph_id_t namespace_id,
    st_ast_string_t name)
{
    return find_visible_index(builder->result, builder->impl, namespace_id,
                              name);
}

static bool append_event(graph_builder_t *builder, graph_event_t event)
{
    graph_impl_t *impl = builder->impl;
    if (impl->event_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!reserve_array(builder, (void **)&impl->events,
                       &impl->event_capacity, impl->event_count,
                       impl->event_count + 1u, sizeof(*impl->events))) {
        return false;
    }
    impl->events[impl->event_count++] = event;
    return true;
}

static bool reserve_entities(graph_builder_t *builder, size_t extra)
{
    size_t required;
    if (extra > SIZE_MAX - builder->result->entity_count) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    required = builder->result->entity_count + extra;
    if (required > UINT32_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    return reserve_array(builder, (void **)&builder->result->entities,
                         &builder->impl->entity_capacity,
                         builder->result->entity_count, required,
                         sizeof(*builder->result->entities))
        && reserve_array(builder, (void **)&builder->impl->entity_work,
                         &builder->impl->entity_work_capacity,
                         builder->result->entity_count, required,
                         sizeof(*builder->impl->entity_work));
}

static st_class_graph_id_t append_namespace(graph_builder_t *builder,
                                             const st_ast_node_t *node,
                                             st_class_graph_id_t namespace_id,
                                             size_t unit_index, size_t order)
{
    st_class_graph_result_t *result = builder->result;
    st_class_graph_entity_t *entity;
    graph_entity_work_t *work;
    if (!entity_index_reserve(builder, 1u)
            || !reserve_entities(builder, 1u))
        return ST_CLASS_GRAPH_INVALID_ID;
    entity = &result->entities[result->entity_count];
    work = &builder->impl->entity_work[result->entity_count];
    memset(entity, 0, sizeof(*entity));
    memset(work, 0, sizeof(*work));
    entity->id = (st_class_graph_id_t)(result->entity_count + 1u);
    entity->kind = ST_CLASS_GRAPH_NAMESPACE;
    entity->name = node_name(node->as.class_decl.name);
    entity->declaration = node;
    entity->origin = origin_for(builder, unit_index, node);
    entity->namespace_id = namespace_id;
    entity->inheritance_valid = true;
    work->node = node;
    work->unit_index = unit_index;
    work->order = order;
    result->entity_count++;
    entity_index_insert_unchecked(builder, entity->id);
    return entity->id;
}

static st_class_graph_id_t append_class_pair(graph_builder_t *builder,
                                              const st_ast_node_t *node,
                                              st_class_graph_id_t namespace_id,
                                              size_t unit_index, size_t order)
{
    st_class_graph_result_t *result = builder->result;
    st_class_graph_entity_t *class_entity;
    st_class_graph_entity_t *meta_entity;
    graph_entity_work_t *class_work;
    graph_entity_work_t *meta_work;
    st_class_graph_id_t class_id;
    st_class_graph_id_t meta_id;
    if (!entity_index_reserve(builder, 2u)
            || !reserve_entities(builder, 2u))
        return ST_CLASS_GRAPH_INVALID_ID;
    class_id = (st_class_graph_id_t)(result->entity_count + 1u);
    meta_id = class_id + 1u;
    class_entity = &result->entities[result->entity_count];
    meta_entity = class_entity + 1;
    class_work = &builder->impl->entity_work[result->entity_count];
    meta_work = class_work + 1;
    memset(class_entity, 0, sizeof(*class_entity));
    memset(meta_entity, 0, sizeof(*meta_entity));
    memset(class_work, 0, sizeof(*class_work));
    memset(meta_work, 0, sizeof(*meta_work));
    class_entity->id = class_id;
    class_entity->kind = ST_CLASS_GRAPH_CLASS;
    class_entity->name = node_name(node->as.class_decl.name);
    class_entity->declaration = node;
    class_entity->origin = origin_for(builder, unit_index, node);
    class_entity->namespace_id = namespace_id;
    class_entity->metaclass_id = meta_id;
    class_entity->inheritance_valid = true;
    meta_entity->id = meta_id;
    meta_entity->kind = ST_CLASS_GRAPH_METACLASS;
    meta_entity->name = class_entity->name;
    meta_entity->declaration = node;
    meta_entity->origin = class_entity->origin;
    meta_entity->namespace_id = namespace_id;
    meta_entity->instance_class_id = class_id;
    meta_entity->inheritance_valid = true;
    class_work->node = node;
    class_work->unit_index = unit_index;
    class_work->order = order;
    meta_work->node = node;
    meta_work->unit_index = unit_index;
    meta_work->order = order;
    result->entity_count += 2u;
    entity_index_insert_unchecked(builder, class_id);
    return class_id;
}

static bool collect_declaration(graph_builder_t *builder,
                                const st_ast_node_t *node,
                                st_class_graph_id_t namespace_id,
                                size_t unit_index)
{
    st_ast_string_t name;
    const st_class_graph_entity_t *duplicate;
    graph_event_t event;
    size_t order;
    size_t member;
    st_class_graph_id_t id;
    if (builder->next_order == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    order = builder->next_order++;
    if (node == NULL || node->kind != ST_AST_CLASS) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 (st_ast_string_t){0}, unit_index, node, NULL);
    }
    name = node_name(node->as.class_decl.name);
    if (!string_valid(name)) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 name, unit_index, node, NULL);
    }
    if (node->as.class_decl.is_extension) {
        memset(&event, 0, sizeof(event));
        event.kind = GRAPH_EVENT_EXTENSION;
        event.node = node;
        event.namespace_id = namespace_id;
        event.unit_index = unit_index;
        event.order = order;
        return append_event(builder, event);
    }
    duplicate = find_exact(builder, namespace_id, name);
    if (duplicate != NULL) {
        if (!append_diagnostic(builder,
                ST_CLASS_GRAPH_DIAG_DUPLICATE_DEFINITION, name, unit_index,
                node, &duplicate->origin)) return false;
        if (!node->as.class_decl.is_namespace) return true;
        id = duplicate->kind == ST_CLASS_GRAPH_NAMESPACE
            ? duplicate->id : ST_CLASS_GRAPH_INVALID_ID;
    } else if (node->as.class_decl.is_namespace) {
        id = append_namespace(builder, node, namespace_id, unit_index, order);
        if (id == ST_CLASS_GRAPH_INVALID_ID) return false;
    } else {
        id = append_class_pair(builder, node, namespace_id, unit_index, order);
        if (id == ST_CLASS_GRAPH_INVALID_ID) return false;
        memset(&event, 0, sizeof(event));
        event.kind = GRAPH_EVENT_DEFINITION;
        event.node = node;
        event.namespace_id = namespace_id;
        event.target_id = id;
        event.unit_index = unit_index;
        event.order = order;
        if (!append_event(builder, event)) return false;
    }
    if (!node->as.class_decl.is_namespace) return true;
    if (!list_valid(&node->as.class_decl.members)) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 name, unit_index, node, NULL);
    }
    if (id == ST_CLASS_GRAPH_INVALID_ID) return true;
    for (member = 0u; member < node->as.class_decl.members.count; member++) {
        if (!collect_declaration(builder,
                node->as.class_decl.members.items[member], id, unit_index)) {
            return false;
        }
    }
    return true;
}

static bool collect_units(graph_builder_t *builder)
{
    size_t unit_index;
    for (unit_index = 0u; unit_index < builder->unit_count; unit_index++) {
        const st_ast_unit_t *unit = builder->units[unit_index];
        size_t declaration;
        if (unit == NULL || st_ast_unit_status(unit) != ST_AST_OK
                || !list_valid(&unit->declarations)) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
        for (declaration = 0u; declaration < unit->declarations.count;
             declaration++) {
            if (!collect_declaration(builder,
                    unit->declarations.items[declaration],
                    ST_CLASS_GRAPH_INVALID_ID, unit_index)) return false;
        }
    }
    return true;
}

static bool resolve_superclasses(graph_builder_t *builder)
{
    size_t index;
    st_class_graph_result_t *result = builder->result;
    for (index = 0u; index < result->entity_count; index++) {
        st_class_graph_entity_t *entity = &result->entities[index];
        graph_entity_work_t *work = &builder->impl->entity_work[index];
        const st_ast_node_t *super_node;
        const st_class_graph_entity_t *super_entity;
        st_ast_string_t super_name;
        if (entity->kind != ST_CLASS_GRAPH_CLASS) continue;
        super_node = work->node == NULL
            ? NULL : work->node->as.class_decl.super_name;
        if (super_node != NULL && super_node->kind == ST_AST_NIL) continue;
        super_name = node_name(super_node);
        if (!string_valid(super_name)) {
            entity->inheritance_valid = false;
            if (!append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                    super_name, work->unit_index, super_node, NULL)) return false;
            continue;
        }
        super_entity = find_visible(builder, entity->namespace_id, super_name);
        if (super_entity == NULL) {
            entity->inheritance_valid = false;
            if (!append_diagnostic(builder,
                    ST_CLASS_GRAPH_DIAG_SUPERCLASS_MISSING, super_name,
                    work->unit_index, super_node, NULL)) return false;
        } else if (super_entity->kind != ST_CLASS_GRAPH_CLASS) {
            entity->inheritance_valid = false;
            if (!append_diagnostic(builder,
                    ST_CLASS_GRAPH_DIAG_SUPERCLASS_NOT_CLASS, super_name,
                    work->unit_index, super_node,
                    &super_entity->origin)) return false;
        } else {
            entity->superclass_id = super_entity->id;
        }
    }
    return true;
}

static bool reserve_path(graph_builder_t *builder)
{
    size_t required = builder->result->entity_count == 0u
        ? 1u : builder->result->entity_count;
    return reserve_array(builder, (void **)&builder->impl->path,
                         &builder->impl->path_capacity, 0u, required,
                         sizeof(*builder->impl->path));
}

static bool detect_cycles(graph_builder_t *builder)
{
    st_class_graph_result_t *result = builder->result;
    size_t start;
    if (!reserve_path(builder)) return false;
    for (start = 0u; start < result->entity_count; start++) {
        st_class_graph_entity_t *start_entity = &result->entities[start];
        st_class_graph_id_t current;
        size_t path_count = 0u;
        size_t path_index;
        if (start_entity->kind != ST_CLASS_GRAPH_CLASS
                || builder->impl->entity_work[start].visit_state != 0u) {
            continue;
        }
        current = start_entity->id;
        while (current != ST_CLASS_GRAPH_INVALID_ID) {
            st_class_graph_entity_t *entity = entity_mut(builder, current);
            graph_entity_work_t *work = work_mut(builder, current);
            if (entity == NULL || work == NULL
                    || entity->kind != ST_CLASS_GRAPH_CLASS) break;
            if (work->visit_state == 2u) break;
            if (work->visit_state == 1u) {
                size_t cycle_begin = 0u;
                while (cycle_begin < path_count
                        && builder->impl->path[cycle_begin] != current) {
                    cycle_begin++;
                }
                for (path_index = cycle_begin; path_index < path_count;
                     path_index++) {
                    st_class_graph_id_t cycle_id = builder->impl->path[path_index];
                    st_class_graph_entity_t *cycle_entity = entity_mut(
                        builder, cycle_id);
                    graph_entity_work_t *cycle_work = work_mut(builder, cycle_id);
                    cycle_entity->inheritance_valid = false;
                    if (!cycle_work->cycle_reported) {
                        const st_class_graph_entity_t *related = entity_mut(
                            builder, cycle_entity->superclass_id);
                        if (!append_diagnostic(builder,
                                ST_CLASS_GRAPH_DIAG_INHERITANCE_CYCLE,
                                cycle_entity->name, cycle_work->unit_index,
                                cycle_work->node,
                                related == NULL ? NULL : &related->origin)) {
                            return false;
                        }
                        cycle_work->cycle_reported = true;
                    }
                }
                break;
            }
            work->visit_state = 1u;
            builder->impl->path[path_count++] = current;
            current = entity->superclass_id;
        }
        for (path_index = 0u; path_index < path_count; path_index++)
            work_mut(builder, builder->impl->path[path_index])->visit_state = 2u;
    }
    /* A descendant of a missing or cyclic superclass is not layout-safe. */
    for (;;) {
        bool changed = false;
        size_t index;
        for (index = 0u; index < result->entity_count; index++) {
            st_class_graph_entity_t *entity = &result->entities[index];
            const st_class_graph_entity_t *super_entity;
            if (entity->kind != ST_CLASS_GRAPH_CLASS
                    || !entity->inheritance_valid
                    || entity->superclass_id == ST_CLASS_GRAPH_INVALID_ID) {
                continue;
            }
            super_entity = entity_mut(builder, entity->superclass_id);
            if (super_entity == NULL || !super_entity->inheritance_valid) {
                entity->inheritance_valid = false;
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (start = 0u; start < result->entity_count; start++) {
        st_class_graph_entity_t *entity = &result->entities[start];
        st_class_graph_entity_t *meta;
        const st_class_graph_entity_t *super_entity;
        if (entity->kind != ST_CLASS_GRAPH_CLASS) continue;
        meta = entity_mut(builder, entity->metaclass_id);
        if (meta == NULL) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
        meta->inheritance_valid = entity->inheritance_valid;
        if (entity->superclass_id != ST_CLASS_GRAPH_INVALID_ID) {
            super_entity = entity_mut(builder, entity->superclass_id);
            if (super_entity != NULL)
                meta->superclass_id = super_entity->metaclass_id;
        }
    }
    return true;
}

static bool append_instance_slot(graph_builder_t *builder,
                                 st_class_graph_slot_t slot)
{
    st_class_graph_result_t *result = builder->result;
    if (result->instance_slot_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!reserve_array(builder, (void **)&result->instance_slots,
                       &builder->impl->instance_slot_capacity,
                       result->instance_slot_count,
                       result->instance_slot_count + 1u,
                       sizeof(*result->instance_slots))) return false;
    result->instance_slots[result->instance_slot_count++] = slot;
    return true;
}

static bool append_class_variable(graph_builder_t *builder,
                                  st_class_graph_slot_t slot)
{
    st_class_graph_result_t *result = builder->result;
    if (result->class_variable_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!reserve_array(builder, (void **)&result->class_variables,
                       &builder->impl->class_variable_capacity,
                       result->class_variable_count,
                       result->class_variable_count + 1u,
                       sizeof(*result->class_variables))) return false;
    result->class_variables[result->class_variable_count++] = slot;
    return true;
}

static bool is_class_variable(st_ast_string_t name)
{
    unsigned char first;
    if (!string_valid(name)) return false;
    first = (unsigned char)name.data[0];
    return first >= (unsigned char)'A' && first <= (unsigned char)'Z';
}

static const st_class_graph_slot_t *find_slot(
    const st_class_graph_slot_t *slots, size_t offset, size_t count,
    st_ast_string_t name)
{
    size_t index;
    for (index = 0u; index < count; index++) {
        if (string_equal(slots[offset + index].name, name))
            return &slots[offset + index];
    }
    return NULL;
}

static bool append_own_variables(graph_builder_t *builder,
                                 st_class_graph_entity_t *entity)
{
    graph_entity_work_t *work = work_mut(builder, entity->id);
    const st_ast_node_t *node = work == NULL ? NULL : work->node;
    size_t variable;
    if (node == NULL || !list_valid(&node->as.class_decl.variables)) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 entity->name,
                                 work == NULL ? 0u : work->unit_index,
                                 node, NULL);
    }
    for (variable = 0u; variable < node->as.class_decl.variables.count;
         variable++) {
        const st_ast_node_t *declaration =
            node->as.class_decl.variables.items[variable];
        st_ast_string_t name = node_name(declaration);
        bool class_variable;
        const st_class_graph_slot_t *duplicate;
        st_class_graph_slot_t slot;
        if (!string_valid(name)) {
            if (!append_diagnostic(builder,
                    ST_CLASS_GRAPH_DIAG_MALFORMED_AST, name,
                    work->unit_index, declaration, NULL)) return false;
            continue;
        }
        class_variable = is_class_variable(name);
        duplicate = class_variable
            ? find_slot(builder->result->class_variables,
                        entity->class_variable_offset,
                        entity->class_variable_count, name)
            : find_slot(builder->result->instance_slots,
                        entity->instance_slot_offset,
                        entity->instance_slot_count, name);
        if (duplicate != NULL) {
            if (!append_diagnostic(builder,
                    ST_CLASS_GRAPH_DIAG_DUPLICATE_VARIABLE, name,
                    work->unit_index, declaration,
                    &duplicate->origin)) return false;
            continue;
        }
        if ((class_variable ? entity->class_variable_count
                            : entity->instance_slot_count) >= UINT32_MAX) {
            if (!append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_SLOT_OVERFLOW,
                    name, work->unit_index, declaration, NULL)) return false;
            continue;
        }
        memset(&slot, 0, sizeof(slot));
        slot.kind = class_variable ? ST_CLASS_GRAPH_CLASS_VARIABLE
                                   : ST_CLASS_GRAPH_INSTANCE_SLOT;
        slot.name = name;
        slot.declaration = declaration;
        slot.origin = origin_for(builder, work->unit_index, declaration);
        slot.declaring_class = entity->id;
        slot.slot = (uint32_t)(class_variable ? entity->class_variable_count
                                              : entity->instance_slot_count);
        if (declaration->kind == ST_AST_VARIABLE) {
            slot.has_type = declaration->as.variable.has_type;
            slot.type_name = declaration->as.variable.type_name;
        }
        if (class_variable) {
            if (!append_class_variable(builder, slot)) return false;
            entity->class_variable_count++;
        } else {
            if (!append_instance_slot(builder, slot)) return false;
            entity->instance_slot_count++;
        }
    }
    return true;
}

static bool build_one_layout(graph_builder_t *builder,
                             st_class_graph_entity_t *entity)
{
    st_class_graph_result_t *result = builder->result;
    graph_entity_work_t *work = work_mut(builder, entity->id);
    const st_class_graph_entity_t *super_entity = entity_mut(
        builder, entity->superclass_id);
    size_t index;
    entity->instance_slot_offset = result->instance_slot_count;
    entity->class_variable_offset = result->class_variable_count;
    if (super_entity != NULL && entity->inheritance_valid) {
        for (index = 0u; index < super_entity->instance_slot_count; index++) {
            st_class_graph_slot_t inherited = result->instance_slots[
                super_entity->instance_slot_offset + index];
            if (!append_instance_slot(builder, inherited)) return false;
            entity->instance_slot_count++;
        }
        for (index = 0u; index < super_entity->class_variable_count; index++) {
            st_class_graph_slot_t inherited = result->class_variables[
                super_entity->class_variable_offset + index];
            if (!append_class_variable(builder, inherited)) return false;
            entity->class_variable_count++;
        }
    }
    if (!append_own_variables(builder, entity)) return false;
    work->layout_done = true;
    {
        st_class_graph_entity_t *meta = entity_mut(builder, entity->metaclass_id);
        meta->class_variable_offset = entity->class_variable_offset;
        meta->class_variable_count = entity->class_variable_count;
        work_mut(builder, meta->id)->layout_done = true;
    }
    return true;
}

static bool build_layouts(graph_builder_t *builder)
{
    st_class_graph_result_t *result = builder->result;
    size_t remaining = 0u;
    size_t index;
    for (index = 0u; index < result->entity_count; index++)
        if (result->entities[index].kind == ST_CLASS_GRAPH_CLASS) remaining++;
    while (remaining != 0u) {
        bool progress = false;
        for (index = 0u; index < result->entity_count; index++) {
            st_class_graph_entity_t *entity = &result->entities[index];
            graph_entity_work_t *work = &builder->impl->entity_work[index];
            graph_entity_work_t *super_work;
            if (entity->kind != ST_CLASS_GRAPH_CLASS || work->layout_done)
                continue;
            super_work = work_mut(builder, entity->superclass_id);
            if (entity->inheritance_valid && super_work != NULL
                    && !super_work->layout_done) continue;
            if (!build_one_layout(builder, entity)) return false;
            remaining--;
            progress = true;
        }
        if (!progress) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
    }
    return true;
}

static uint64_t method_key_hash(st_class_graph_id_t owner,
                                st_ast_string_t selector)
{
    uint64_t hash = UINT64_C(14695981039346656037) ^ owner;
    size_t index;
    hash *= UINT64_C(1099511628211);
    for (index = 0u; index < selector.length; index++) {
        hash ^= (unsigned char)selector.data[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return hash == 0u ? UINT64_C(1) : hash;
}

static void method_index_insert_unchecked(graph_builder_t *builder,
                                          st_class_graph_method_id_t id)
{
    const st_class_graph_method_t *method =
        &builder->result->methods[id - 1u];
    graph_method_bucket_t incoming;
    size_t mask = builder->impl->method_bucket_capacity - 1u;
    size_t index;
    size_t distance = 0u;
    incoming.hash = method_key_hash(method->owner, method->selector);
    incoming.id = id;
    index = (size_t)incoming.hash & mask;
    for (;;) {
        graph_method_bucket_t *slot = &builder->impl->method_buckets[index];
        size_t resident_distance;
        if (builder->impl->method_index_probe_count != SIZE_MAX)
            builder->impl->method_index_probe_count++;
        if (slot->id == ST_CLASS_GRAPH_INVALID_ID) {
            *slot = incoming;
            return;
        }
        resident_distance = probe_distance(index, slot->hash, mask);
        if (resident_distance < distance) {
            graph_method_bucket_t displaced = *slot;
            *slot = incoming;
            incoming = displaced;
            distance = resident_distance;
        }
        index = (index + 1u) & mask;
        distance++;
    }
}

static bool method_index_rehash(graph_builder_t *builder,
                                size_t new_capacity)
{
    graph_method_bucket_t *old = builder->impl->method_buckets;
    size_t index;
    if (new_capacity < 16u ||
        (new_capacity & (new_capacity - 1u)) != 0u ||
        new_capacity > SIZE_MAX / sizeof(*old)) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    builder->impl->method_buckets = allocate_zeroed(
        builder, new_capacity, sizeof(*old));
    if (!builder->impl->method_buckets) {
        builder->impl->method_buckets = old;
        return false;
    }
    builder->impl->method_bucket_capacity = new_capacity;
    for (index = 0u; index < builder->result->method_count; index++)
        method_index_insert_unchecked(builder,
                                      (st_class_graph_method_id_t)index + 1u);
    release_memory(builder->impl, old);
    return true;
}

static bool method_index_reserve(graph_builder_t *builder, size_t extra)
{
    size_t required;
    size_t capacity = builder->impl->method_bucket_capacity;
    if (extra > SIZE_MAX - builder->result->method_count) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    required = builder->result->method_count + extra;
    if (capacity == 0u) capacity = 16u;
    while (required > capacity - capacity / 4u) {
        if (capacity > SIZE_MAX / 2u) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        capacity *= 2u;
    }
    return capacity == builder->impl->method_bucket_capacity ||
           method_index_rehash(builder, capacity);
}

static const st_class_graph_method_t *find_owned_method(
    graph_builder_t *builder, st_class_graph_id_t owner,
    st_ast_string_t selector)
{
    graph_impl_t *impl = builder->impl;
    uint64_t hash;
    size_t mask;
    size_t index;
    size_t distance = 0u;
    if (impl->method_bucket_capacity == 0u || !impl->method_buckets)
        return NULL;
    hash = method_key_hash(owner, selector);
    mask = impl->method_bucket_capacity - 1u;
    index = (size_t)hash & mask;
    for (;;) {
        const graph_method_bucket_t *slot = &impl->method_buckets[index];
        const st_class_graph_method_t *method;
        if (impl->method_index_probe_count != SIZE_MAX)
            impl->method_index_probe_count++;
        if (slot->id == ST_CLASS_GRAPH_INVALID_ID) return NULL;
        if (probe_distance(index, slot->hash, mask) < distance) return NULL;
        method = &builder->result->methods[slot->id - 1u];
        if (slot->hash == hash && method->owner == owner &&
            string_equal(method->selector, selector))
            return method;
        index = (index + 1u) & mask;
        if (++distance == impl->method_bucket_capacity) return NULL;
    }
}

static bool append_method(graph_builder_t *builder,
                          const st_ast_node_t *node,
                          st_class_graph_id_t instance_class,
                          size_t unit_index)
{
    st_class_graph_result_t *result = builder->result;
    st_class_graph_entity_t *class_entity = entity_mut(builder, instance_class);
    st_class_graph_id_t owner;
    const st_class_graph_method_t *duplicate;
    st_class_graph_method_t *method;
    st_ast_string_t selector;
    bool class_side;
    if (class_entity == NULL || class_entity->kind != ST_CLASS_GRAPH_CLASS
            || node == NULL || node->kind != ST_AST_METHOD) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 (st_ast_string_t){0}, unit_index, node, NULL);
    }
    selector = node->as.method.selector;
    class_side = node->as.method.class_side;
    if (!string_valid(selector)
            || (class_side && !string_is(node->as.method.class_name, "class"))
            || (!class_side && node->as.method.class_name.length != 0u)) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
                                 selector, unit_index, node, NULL);
    }
    owner = class_side ? class_entity->metaclass_id : class_entity->id;
    duplicate = find_owned_method(builder, owner, selector);
    if (duplicate != NULL) {
        return append_diagnostic(builder, ST_CLASS_GRAPH_DIAG_DUPLICATE_METHOD,
                                 selector, unit_index, node,
                                 &duplicate->origin);
    }
    if (result->method_count >= UINT32_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!method_index_reserve(builder, 1u) ||
        !reserve_array(builder, (void **)&result->methods,
                       &builder->impl->method_capacity, result->method_count,
                       result->method_count + 1u,
                       sizeof(*result->methods))) return false;
    method = &result->methods[result->method_count];
    memset(method, 0, sizeof(*method));
    method->id = (st_class_graph_method_id_t)(result->method_count + 1u);
    method->node = node;
    method->selector = selector;
    method->origin = origin_for(builder, unit_index, node);
    method->owner = owner;
    method->instance_class = instance_class;
    method->lexical_super = entity_mut(builder, owner)->superclass_id;
    method->class_side = class_side;
    result->method_count++;
    entity_mut(builder, owner)->own_method_count++;
    method_index_insert_unchecked(builder, method->id);
    return true;
}

static bool process_methods(graph_builder_t *builder)
{
    size_t event_index;
    for (event_index = 0u; event_index < builder->impl->event_count;
         event_index++) {
        graph_event_t *event = &builder->impl->events[event_index];
        st_class_graph_id_t target_id = event->target_id;
        st_ast_string_t name = node_name(event->node->as.class_decl.name);
        const st_class_graph_entity_t *target;
        size_t method_index;
        if (event->kind == GRAPH_EVENT_EXTENSION) {
            target = find_visible(builder, event->namespace_id, name);
            if (target == NULL) {
                if (!append_diagnostic(builder,
                        ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_MISSING, name,
                        event->unit_index, event->node, NULL)) return false;
                continue;
            }
            if (target->kind != ST_CLASS_GRAPH_CLASS) {
                if (!append_diagnostic(builder,
                        ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_NOT_CLASS, name,
                        event->unit_index, event->node,
                        &target->origin)) return false;
                continue;
            }
            if (work_mut(builder, target->id)->order >= event->order) {
                if (!append_diagnostic(builder,
                        ST_CLASS_GRAPH_DIAG_EXTENSION_BEFORE_TARGET, name,
                        event->unit_index, event->node,
                        &target->origin)) return false;
                continue;
            }
            target_id = target->id;
        }
        if (!list_valid(&event->node->as.class_decl.methods)) {
            if (!append_diagnostic(builder,
                    ST_CLASS_GRAPH_DIAG_MALFORMED_AST, name,
                    event->unit_index, event->node, NULL)) return false;
            continue;
        }
        for (method_index = 0u;
             method_index < event->node->as.class_decl.methods.count;
             method_index++) {
            if (!append_method(builder,
                    event->node->as.class_decl.methods.items[method_index],
                    target_id, event->unit_index)) return false;
        }
    }
    return true;
}

static bool append_catalog_entry(graph_builder_t *builder,
                                 st_sema_external_t entry)
{
    st_class_graph_result_t *result = builder->result;
    if (result->catalog_entry_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    if (!reserve_array(builder, (void **)&result->catalog_entries,
                       &builder->impl->catalog_capacity,
                       result->catalog_entry_count,
                       result->catalog_entry_count + 1u,
                       sizeof(*result->catalog_entries))) return false;
    result->catalog_entries[result->catalog_entry_count++] = entry;
    return true;
}

static bool allocate_catalog_layers(graph_builder_t *builder)
{
    graph_impl_t *impl = builder->impl;
    size_t scope_count;
    size_t entity_count = builder->result->entity_count;
    size_t index;
    if (entity_count == SIZE_MAX) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    scope_count = entity_count + 1u;
    impl->scope_global_heads = allocate_zeroed(
        builder, scope_count, sizeof(*impl->scope_global_heads));
    impl->scope_global_tails = allocate_zeroed(
        builder, scope_count, sizeof(*impl->scope_global_tails));
    impl->scope_visible_counts = allocate_zeroed(
        builder, scope_count, sizeof(*impl->scope_visible_counts));
    impl->global_next = allocate_zeroed(
        builder, entity_count == 0u ? 1u : entity_count,
        sizeof(*impl->global_next));
    impl->catalog_views = allocate_zeroed(
        builder, entity_count == 0u ? 1u : entity_count,
        sizeof(*impl->catalog_views));
    impl->catalog_view_counts = allocate_zeroed(
        builder, entity_count == 0u ? 1u : entity_count,
        sizeof(*impl->catalog_view_counts));
    if (!impl->scope_global_heads || !impl->scope_global_tails ||
        !impl->scope_visible_counts || !impl->global_next ||
        !impl->catalog_views || !impl->catalog_view_counts)
        return false;
    impl->scope_capacity = scope_count;
    impl->global_next_capacity = entity_count == 0u ? 1u : entity_count;
    impl->catalog_view_capacity = entity_count == 0u ? 1u : entity_count;
    for (index = 0u; index < scope_count; index++) {
        impl->scope_global_heads[index] = SIZE_MAX;
        impl->scope_global_tails[index] = SIZE_MAX;
    }
    for (index = 0u; index < impl->global_next_capacity; index++)
        impl->global_next[index] = SIZE_MAX;
    atomic_flag_clear(&impl->catalog_lock);
    return true;
}

static bool link_shared_globals(graph_builder_t *builder)
{
    st_class_graph_result_t *result = builder->result;
    graph_impl_t *impl = builder->impl;
    size_t entity_index;
    for (entity_index = 0u; entity_index < result->entity_count;
         entity_index++) {
        const st_class_graph_entity_t *entity = &result->entities[entity_index];
        st_sema_external_t entry;
        size_t global_index;
        size_t scope_index;
        if (entity->kind == ST_CLASS_GRAPH_METACLASS) continue;
        if ((size_t)entity->namespace_id >= impl->scope_capacity) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
        memset(&entry, 0, sizeof(entry));
        entry.name = entity->name;
        entry.kind = ST_SEMA_EXTERNAL_GLOBAL;
        entry.external_id = entity->id;
        global_index = result->catalog_entry_count;
        if (!append_catalog_entry(builder, entry)) return false;
        scope_index = entity->namespace_id;
        if (impl->scope_global_heads[scope_index] == SIZE_MAX) {
            impl->scope_global_heads[scope_index] = global_index;
        } else {
            impl->global_next[impl->scope_global_tails[scope_index]] =
                global_index;
        }
        impl->scope_global_tails[scope_index] = global_index;
    }
    return true;
}

typedef struct {
    uint64_t hash;
    st_ast_string_t name;
    size_t top;
} graph_active_global_t;

static graph_active_global_t *active_global_bucket(
    graph_active_global_t *buckets, size_t capacity, st_ast_string_t name)
{
    uint64_t hash = entity_name_hash(0u, name);
    size_t index;
    if (hash == 0u) hash = UINT64_C(1);
    index = (size_t)hash & (capacity - 1u);
    while (buckets[index].hash != 0u) {
        if (buckets[index].hash == hash &&
            string_equal(buckets[index].name, name))
            return &buckets[index];
        index = (index + 1u) & (capacity - 1u);
    }
    buckets[index].hash = hash;
    buckets[index].name = name;
    buckets[index].top = SIZE_MAX;
    return &buckets[index];
}

static bool compute_one_scope_visible_count(
    graph_builder_t *builder, st_class_graph_id_t scope_id,
    size_t parent_visible_count, graph_active_global_t *active,
    size_t active_capacity, size_t *previous)
{
    st_class_graph_result_t *result = builder->result;
    graph_impl_t *impl = builder->impl;
    size_t visible = parent_visible_count;
    size_t global = impl->scope_global_heads[scope_id];
    while (global != SIZE_MAX) {
        graph_active_global_t *bucket = active_global_bucket(
            active, active_capacity, result->catalog_entries[global].name);
        previous[global] = bucket->top;
        if (bucket->top == SIZE_MAX) {
            if (visible == SIZE_MAX) {
                set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
                return false;
            }
            visible++;
        }
        bucket->top = global;
        global = impl->global_next[global];
    }
    impl->scope_visible_counts[scope_id] = visible;
    global = impl->scope_global_heads[scope_id];
    while (global != SIZE_MAX) {
        const st_sema_external_t *entry = &result->catalog_entries[global];
        const st_class_graph_entity_t *entity = st_class_graph_entity(
            result, entry->external_id);
        if (entity && entity->kind == ST_CLASS_GRAPH_NAMESPACE &&
            !compute_one_scope_visible_count(builder, entity->id, visible,
                                             active, active_capacity,
                                             previous))
            return false;
        global = impl->global_next[global];
    }
    global = impl->scope_global_heads[scope_id];
    while (global != SIZE_MAX) {
        graph_active_global_t *bucket = active_global_bucket(
            active, active_capacity, result->catalog_entries[global].name);
        bucket->top = previous[global];
        global = impl->global_next[global];
    }
    return true;
}

static bool compute_scope_visible_counts(graph_builder_t *builder)
{
    st_class_graph_result_t *result = builder->result;
    graph_impl_t *impl = builder->impl;
    graph_active_global_t *active;
    size_t *previous;
    size_t active_capacity = 16u;
    size_t global_count = result->catalog_entry_count;
    bool succeeded;
    if (global_count > SIZE_MAX / 2u) {
        set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
        return false;
    }
    while (global_count * 2u > active_capacity - active_capacity / 4u) {
        if (active_capacity > SIZE_MAX / 2u) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        active_capacity *= 2u;
    }
    active = allocate_zeroed(builder, active_capacity, sizeof(*active));
    previous = allocate_zeroed(builder, global_count == 0u ? 1u : global_count,
                               sizeof(*previous));
    if (!active || !previous) {
        release_memory(impl, active);
        release_memory(impl, previous);
        return false;
    }
    succeeded = compute_one_scope_visible_count(
        builder, ST_CLASS_GRAPH_INVALID_ID, 0u, active, active_capacity,
        previous);
    release_memory(impl, active);
    release_memory(impl, previous);
    return succeeded;
}

static bool build_catalogs(graph_builder_t *builder)
{
    st_class_graph_result_t *result = builder->result;
    graph_impl_t *impl = builder->impl;
    size_t index;
    if (!allocate_catalog_layers(builder) || !link_shared_globals(builder) ||
        !compute_scope_visible_counts(builder))
        return false;
    for (index = 0u; index < result->entity_count; index++) {
        st_class_graph_entity_t *entity = &result->entities[index];
        st_class_graph_entity_t *meta;
        size_t globals;
        if (entity->kind != ST_CLASS_GRAPH_CLASS) continue;
        if ((size_t)entity->namespace_id >= impl->scope_capacity) {
            set_status(builder, ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT);
            return false;
        }
        globals = impl->scope_visible_counts[entity->namespace_id];
        if (entity->instance_slot_count > SIZE_MAX -
                                            entity->class_variable_count ||
            entity->instance_slot_count + entity->class_variable_count >
                                            SIZE_MAX - globals) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        entity->catalog_offset = entity->namespace_id;
        entity->catalog_count = entity->instance_slot_count +
                                entity->class_variable_count + globals;
        meta = entity_mut(builder, entity->metaclass_id);
        meta->catalog_offset = entity->namespace_id;
        if (entity->class_variable_count > SIZE_MAX - globals) {
            set_status(builder, ST_CLASS_GRAPH_ERR_OVERFLOW);
            return false;
        }
        meta->catalog_count = entity->class_variable_count + globals;
    }
    for (index = 0u; index < result->method_count; index++) {
        st_class_graph_method_t *method = &result->methods[index];
        const st_class_graph_entity_t *owner = entity_mut(builder, method->owner);
        method->catalog_offset = owner->catalog_offset;
        method->catalog_count = owner->catalog_count;
    }
    return true;
}

void st_class_graph_result_init(st_class_graph_result_t *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void st_class_graph_result_destroy(st_class_graph_result_t *result)
{
    graph_impl_t *impl;
    if (result == NULL) return;
    impl = result->implementation;
    if (impl != NULL) {
        size_t index;
        for (index = 0u; index < impl->catalog_view_capacity; index++)
            release_memory(impl, impl->catalog_views == NULL
                ? NULL : impl->catalog_views[index]);
        release_memory(impl, result->entities);
        release_memory(impl, result->methods);
        release_memory(impl, result->instance_slots);
        release_memory(impl, result->class_variables);
        release_memory(impl, result->catalog_entries);
        release_memory(impl, result->diagnostics);
        release_memory(impl, impl->entity_work);
        release_memory(impl, impl->entity_buckets);
        release_memory(impl, impl->events);
        release_memory(impl, impl->path);
        release_memory(impl, impl->method_buckets);
        release_memory(impl, impl->scope_global_heads);
        release_memory(impl, impl->scope_global_tails);
        release_memory(impl, impl->scope_visible_counts);
        release_memory(impl, impl->global_next);
        release_memory(impl, impl->catalog_views);
        release_memory(impl, impl->catalog_view_counts);
        impl->allocator.deallocate(impl->allocator.user, impl);
    }
    memset(result, 0, sizeof(*result));
}

static bool result_is_empty(const st_class_graph_result_t *result)
{
    return result != NULL && result->implementation == NULL
        && result->entities == NULL && result->entity_count == 0u
        && result->methods == NULL && result->method_count == 0u
        && result->instance_slots == NULL && result->instance_slot_count == 0u
        && result->class_variables == NULL
        && result->class_variable_count == 0u
        && result->catalog_entries == NULL
        && result->catalog_entry_count == 0u
        && result->diagnostics == NULL && result->diagnostic_count == 0u;
}

st_class_graph_status_t st_class_graph_build(
    st_class_graph_result_t *result,
    const st_ast_unit_t *const *units, size_t unit_count,
    const st_class_graph_options_t *options)
{
    st_class_graph_allocator_t allocator;
    st_class_graph_result_t temporary;
    graph_impl_t *impl;
    graph_builder_t builder;
    st_class_graph_status_t failure;
    if (result == NULL) return ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
    if (!result_is_empty(result) || (unit_count != 0u && units == NULL)) {
        if (result_is_empty(result))
            result->status = ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
        return ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
    }
    memset(&allocator, 0, sizeof(allocator));
    if (options != NULL) allocator = options->allocator;
    if ((allocator.allocate == NULL) != (allocator.deallocate == NULL)) {
        result->status = ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    if (allocator.allocate == NULL) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
    }
    st_class_graph_result_init(&temporary);
    impl = allocator.allocate(allocator.user, sizeof(*impl));
    if (impl == NULL) {
        result->status = ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    memset(impl, 0, sizeof(*impl));
    impl->allocator = allocator;
    temporary.implementation = impl;
    memset(&builder, 0, sizeof(builder));
    builder.result = &temporary;
    builder.impl = impl;
    builder.units = units;
    builder.unit_count = unit_count;
    if (!collect_units(&builder)
            || !resolve_superclasses(&builder)
            || !detect_cycles(&builder)
            || !build_layouts(&builder)
            || !process_methods(&builder)
            || !build_catalogs(&builder)) {
        failure = temporary.status == ST_CLASS_GRAPH_OK
            ? ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT : temporary.status;
        st_class_graph_result_destroy(&temporary);
        result->status = failure;
        return failure;
    }
    *result = temporary;
    return result->status;
}

bool st_class_graph_succeeded(const st_class_graph_result_t *result)
{
    return result != NULL && result->status == ST_CLASS_GRAPH_OK
        && result->diagnostic_count == 0u;
}

const st_class_graph_entity_t *st_class_graph_entity(
    const st_class_graph_result_t *result, st_class_graph_id_t id)
{
    if (result == NULL || id == ST_CLASS_GRAPH_INVALID_ID
            || (size_t)id > result->entity_count) return NULL;
    return &result->entities[id - 1u];
}

const st_class_graph_method_t *st_class_graph_method(
    const st_class_graph_result_t *result, st_class_graph_method_id_t id)
{
    if (result == NULL || id == 0u || (size_t)id > result->method_count)
        return NULL;
    return &result->methods[id - 1u];
}

const st_class_graph_method_t *st_class_graph_method_for_node(
    const st_class_graph_result_t *result, const st_ast_node_t *node)
{
    size_t index;
    if (result == NULL || node == NULL) return NULL;
    for (index = 0u; index < result->method_count; index++)
        if (result->methods[index].node == node) return &result->methods[index];
    return NULL;
}

static void catalog_lock(graph_impl_t *impl)
{
    while (atomic_flag_test_and_set_explicit(&impl->catalog_lock,
                                              memory_order_acquire)) {
    }
}

static void catalog_unlock(graph_impl_t *impl)
{
    atomic_flag_clear_explicit(&impl->catalog_lock, memory_order_release);
}

static void external_from_slot(st_sema_external_t *entry,
                               const st_class_graph_slot_t *slot,
                               st_sema_external_kind_t kind)
{
    memset(entry, 0, sizeof(*entry));
    entry->name = slot->name;
    entry->kind = kind;
    entry->slot = slot->slot;
    entry->external_id = slot->declaring_class;
}

static bool append_flat_visible_globals(
    const st_class_graph_result_t *result, const graph_impl_t *impl,
    st_class_graph_id_t namespace_id, st_sema_external_t *entries,
    size_t capacity, size_t *count_in_out)
{
    st_class_graph_id_t scope = namespace_id;
    for (;;) {
        size_t global;
        if ((size_t)scope >= impl->scope_capacity) return false;
        global = impl->scope_global_heads[scope];
        while (global != SIZE_MAX) {
            const st_sema_external_t *candidate;
            const st_class_graph_entity_t *visible;
            if (global >= result->catalog_entry_count) return false;
            candidate = &result->catalog_entries[global];
            visible = find_visible_index(result, impl, namespace_id,
                                         candidate->name);
            if (visible && visible->id == candidate->external_id) {
                if (*count_in_out >= capacity) return false;
                entries[(*count_in_out)++] = *candidate;
            }
            global = impl->global_next[global];
        }
        if (scope == ST_CLASS_GRAPH_INVALID_ID) break;
        if ((size_t)scope > result->entity_count) return false;
        scope = result->entities[scope - 1u].namespace_id;
    }
    return true;
}

static bool build_flat_catalog_for_owner(const st_class_graph_result_t *result,
                                         graph_impl_t *impl,
                                         st_class_graph_id_t owner_id,
                                         st_sema_external_t **entries_out,
                                         size_t *count_out)
{
    const st_class_graph_entity_t *owner = st_class_graph_entity(result,
                                                                 owner_id);
    const st_class_graph_entity_t *instance;
    st_sema_external_t *entries = NULL;
    size_t count = 0u;
    size_t index;
    if (!owner || (owner->kind != ST_CLASS_GRAPH_CLASS &&
                   owner->kind != ST_CLASS_GRAPH_METACLASS))
        return false;
    instance = owner->kind == ST_CLASS_GRAPH_CLASS
        ? owner : st_class_graph_entity(result, owner->instance_class_id);
    if (!instance) return false;
    if (owner->catalog_count != 0u) {
        if (owner->catalog_count > SIZE_MAX / sizeof(*entries)) return false;
        entries = impl->allocator.allocate(
            impl->allocator.user, owner->catalog_count * sizeof(*entries));
        if (!entries) return false;
    }
    if (owner->kind == ST_CLASS_GRAPH_CLASS) {
        for (index = 0u; index < instance->instance_slot_count; index++) {
            const st_class_graph_slot_t *slot = &result->instance_slots[
                instance->instance_slot_offset + index];
            external_from_slot(&entries[count++], slot,
                               ST_SEMA_EXTERNAL_INSTANCE_VARIABLE);
        }
    }
    for (index = 0u; index < instance->class_variable_count; index++) {
        const st_class_graph_slot_t *slot = &result->class_variables[
            instance->class_variable_offset + index];
        external_from_slot(&entries[count++], slot,
                           ST_SEMA_EXTERNAL_CLASS_VARIABLE);
    }
    if (!append_flat_visible_globals(result, impl, instance->namespace_id,
                                     entries, owner->catalog_count, &count) ||
        count != owner->catalog_count) {
        if (entries) impl->allocator.deallocate(impl->allocator.user, entries);
        return false;
    }
    *entries_out = entries;
    *count_out = count;
    return true;
}

bool st_class_graph_sema_catalog_for_method(
    const st_class_graph_result_t *result,
    st_class_graph_method_id_t method_id, st_sema_catalog_t *catalog_out)
{
    const st_class_graph_method_t *method = st_class_graph_method(
        result, method_id);
    graph_impl_t *impl;
    st_sema_external_t *entries;
    size_t count;
    if (!catalog_out) return false;
    memset(catalog_out, 0, sizeof(*catalog_out));
    if (!method || !result->implementation || method->owner == 0u ||
        (size_t)method->owner > result->entity_count)
        return false;
    impl = result->implementation;
    if ((size_t)method->owner > impl->catalog_view_capacity) return false;
    catalog_lock(impl);
    entries = impl->catalog_views[method->owner - 1u];
    count = impl->catalog_view_counts[method->owner - 1u];
    if (!entries && count == 0u) {
        const st_class_graph_entity_t *owner = st_class_graph_entity(
            result, method->owner);
        if (!owner || owner->catalog_count != 0u) {
            if (!build_flat_catalog_for_owner(result, impl, method->owner,
                                              &entries, &count)) {
                catalog_unlock(impl);
                return false;
            }
            if (impl->compatibility_view_count == SIZE_MAX ||
                count > SIZE_MAX - impl->compatibility_entry_count) {
                if (entries)
                    impl->allocator.deallocate(impl->allocator.user, entries);
                catalog_unlock(impl);
                return false;
            }
            impl->catalog_views[method->owner - 1u] = entries;
            impl->catalog_view_counts[method->owner - 1u] = count;
            impl->compatibility_view_count++;
            impl->compatibility_entry_count += count;
        }
    }
    catalog_out->entries = entries;
    catalog_out->count = count;
    catalog_out->has_lexical_super =
        method->lexical_super != ST_CLASS_GRAPH_INVALID_ID;
    catalog_unlock(impl);
    return true;
}

typedef struct {
    uint64_t hash;
    st_ast_string_t name;
} graph_name_bucket_t;

static uint64_t graph_name_hash(st_ast_string_t name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;
    for (index = 0u; index < name.length; index++) {
        hash ^= (unsigned char)name.data[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return hash == 0u ? UINT64_C(1) : hash;
}

static size_t reference_name_count(const st_ast_node_t *node)
{
    size_t count = 0u;
    size_t index;
    if (!node) return 0u;
    switch (node->kind) {
    case ST_AST_VARIABLE:
        return string_valid(node->as.variable.name) ? 1u : 0u;
    case ST_AST_BLOCK:
        if (!list_valid(&node->as.block.expressions)) return 0u;
        for (index = 0u; index < node->as.block.expressions.count; index++) {
            size_t child = reference_name_count(
                node->as.block.expressions.items[index]);
            if (count > SIZE_MAX - child) return SIZE_MAX;
            count += child;
        }
        return count;
    case ST_AST_EXPRESSION:
        if (!list_valid(&node->as.expression.assignments) ||
            !list_valid(&node->as.expression.messages))
            return 0u;
        count = reference_name_count(node->as.expression.receiver);
        for (index = 0u; index < node->as.expression.assignments.count;
             index++) {
            size_t child = reference_name_count(
                node->as.expression.assignments.items[index]);
            if (count == SIZE_MAX || count > SIZE_MAX - child) return SIZE_MAX;
            count += child;
        }
        for (index = 0u; index < node->as.expression.messages.count; index++) {
            const st_ast_node_t *message =
                node->as.expression.messages.items[index];
            size_t argument;
            if (!message || message->kind != ST_AST_MESSAGE ||
                !list_valid(&message->as.message.arguments))
                continue;
            for (argument = 0u; argument < message->as.message.arguments.count;
                 argument++) {
                size_t child = reference_name_count(
                    message->as.message.arguments.items[argument]);
                if (count == SIZE_MAX || count > SIZE_MAX - child)
                    return SIZE_MAX;
                count += child;
            }
        }
        return count;
    default:
        return 0u;
    }
}

static void insert_reference_names(const st_ast_node_t *node,
                                   graph_name_bucket_t *buckets,
                                   size_t capacity, size_t *unique_count)
{
    size_t index;
    if (!node) return;
    if (node->kind == ST_AST_VARIABLE && string_valid(node->as.variable.name)) {
        st_ast_string_t name = node->as.variable.name;
        uint64_t hash = graph_name_hash(name);
        size_t slot = (size_t)hash & (capacity - 1u);
        while (buckets[slot].hash != 0u) {
            if (buckets[slot].hash == hash &&
                string_equal(buckets[slot].name, name))
                return;
            slot = (slot + 1u) & (capacity - 1u);
        }
        buckets[slot].hash = hash;
        buckets[slot].name = name;
        (*unique_count)++;
        return;
    }
    if (node->kind == ST_AST_BLOCK && list_valid(&node->as.block.expressions)) {
        for (index = 0u; index < node->as.block.expressions.count; index++)
            insert_reference_names(node->as.block.expressions.items[index],
                                   buckets, capacity, unique_count);
    } else if (node->kind == ST_AST_EXPRESSION &&
               list_valid(&node->as.expression.assignments) &&
               list_valid(&node->as.expression.messages)) {
        insert_reference_names(node->as.expression.receiver, buckets, capacity,
                               unique_count);
        for (index = 0u; index < node->as.expression.assignments.count; index++)
            insert_reference_names(node->as.expression.assignments.items[index],
                                   buckets, capacity, unique_count);
        for (index = 0u; index < node->as.expression.messages.count; index++) {
            const st_ast_node_t *message =
                node->as.expression.messages.items[index];
            size_t argument;
            if (!message || message->kind != ST_AST_MESSAGE ||
                !list_valid(&message->as.message.arguments))
                continue;
            for (argument = 0u; argument < message->as.message.arguments.count;
                 argument++)
                insert_reference_names(
                    message->as.message.arguments.items[argument], buckets,
                    capacity, unique_count);
        }
    }
}

static bool name_is_pseudo_variable(st_ast_string_t name)
{
    return string_is(name, "self") || string_is(name, "super") ||
           string_is(name, "thisContext");
}

void st_class_graph_sema_view_init(st_class_graph_sema_view_t *view)
{
    if (view) memset(view, 0, sizeof(*view));
}

st_class_graph_status_t st_class_graph_sema_view_build_minimal(
    st_class_graph_sema_view_t *view,
    const st_class_graph_result_t *result,
    st_class_graph_method_id_t method_id)
{
    const st_class_graph_method_t *method = st_class_graph_method(result,
                                                                  method_id);
    const st_class_graph_entity_t *owner;
    const st_class_graph_entity_t *instance;
    graph_impl_t *impl;
    graph_name_bucket_t *names = NULL;
    st_sema_external_t *entries = NULL;
    size_t reference_count;
    size_t name_capacity = 16u;
    size_t unique_count = 0u;
    size_t entry_capacity;
    size_t entry_count = 0u;
    size_t index;
    if (!view || view->initialized || view->catalog.entries ||
        view->catalog.count != 0u || view->catalog.has_lexical_super ||
        view->catalog.allocator.allocate || view->catalog.allocator.deallocate ||
        view->catalog.allocator.user || view->allocator.allocate ||
        view->allocator.deallocate || view->allocator.user ||
        !result || !result->implementation || !method)
        return ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
    owner = st_class_graph_entity(result, method->owner);
    instance = !owner ? NULL : owner->kind == ST_CLASS_GRAPH_CLASS
        ? owner : st_class_graph_entity(result, owner->instance_class_id);
    if (!owner || !instance || !method->node ||
        method->node->kind != ST_AST_METHOD)
        return ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT;
    impl = result->implementation;
    reference_count = reference_name_count(method->node->as.method.body);
    if (reference_count == SIZE_MAX)
        return ST_CLASS_GRAPH_ERR_OVERFLOW;
    if (reference_count == 0u) {
        view->catalog.has_lexical_super =
            method->lexical_super != ST_CLASS_GRAPH_INVALID_ID;
        view->allocator = impl->allocator;
        view->initialized = true;
        return ST_CLASS_GRAPH_OK;
    }
    while (reference_count > name_capacity / 2u) {
        if (name_capacity > SIZE_MAX / 2u)
            return ST_CLASS_GRAPH_ERR_OVERFLOW;
        name_capacity *= 2u;
    }
    if (name_capacity > SIZE_MAX / sizeof(*names))
        return ST_CLASS_GRAPH_ERR_OVERFLOW;
    names = impl->allocator.allocate(impl->allocator.user,
                                     name_capacity * sizeof(*names));
    if (!names) return ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY;
    memset(names, 0, name_capacity * sizeof(*names));
    insert_reference_names(method->node->as.method.body, names, name_capacity,
                           &unique_count);
    if (unique_count > SIZE_MAX / 3u) {
        impl->allocator.deallocate(impl->allocator.user, names);
        return ST_CLASS_GRAPH_ERR_OVERFLOW;
    }
    entry_capacity = unique_count * 3u;
    if (entry_capacity != 0u) {
        if (entry_capacity > SIZE_MAX / sizeof(*entries)) {
            impl->allocator.deallocate(impl->allocator.user, names);
            return ST_CLASS_GRAPH_ERR_OVERFLOW;
        }
        entries = impl->allocator.allocate(
            impl->allocator.user, entry_capacity * sizeof(*entries));
        if (!entries) {
            impl->allocator.deallocate(impl->allocator.user, names);
            return ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY;
        }
    }
    for (index = 0u; index < name_capacity; index++) {
        const graph_name_bucket_t *name = &names[index];
        const st_class_graph_slot_t *slot;
        const st_class_graph_entity_t *global;
        if (name->hash == 0u || name_is_pseudo_variable(name->name)) continue;
        if (owner->kind == ST_CLASS_GRAPH_CLASS) {
            slot = find_slot(result->instance_slots,
                             instance->instance_slot_offset,
                             instance->instance_slot_count, name->name);
            if (slot)
                external_from_slot(&entries[entry_count++], slot,
                                   ST_SEMA_EXTERNAL_INSTANCE_VARIABLE);
        }
        slot = find_slot(result->class_variables,
                         instance->class_variable_offset,
                         instance->class_variable_count, name->name);
        if (slot)
            external_from_slot(&entries[entry_count++], slot,
                               ST_SEMA_EXTERNAL_CLASS_VARIABLE);
        global = find_visible_index(result, impl, instance->namespace_id,
                                    name->name);
        if (global && global->kind != ST_CLASS_GRAPH_METACLASS) {
            st_sema_external_t *entry = &entries[entry_count++];
            memset(entry, 0, sizeof(*entry));
            entry->name = global->name;
            entry->kind = ST_SEMA_EXTERNAL_GLOBAL;
            entry->external_id = global->id;
        }
    }
    impl->allocator.deallocate(impl->allocator.user, names);
    if (entry_count == 0u && entries) {
        impl->allocator.deallocate(impl->allocator.user, entries);
        entries = NULL;
    }
    view->catalog.entries = entries;
    view->catalog.count = entry_count;
    view->catalog.has_lexical_super =
        method->lexical_super != ST_CLASS_GRAPH_INVALID_ID;
    view->allocator = impl->allocator;
    view->initialized = true;
    return ST_CLASS_GRAPH_OK;
}

void st_class_graph_sema_view_destroy(st_class_graph_sema_view_t *view)
{
    if (!view) return;
    if (view->initialized && view->catalog.entries &&
        view->allocator.deallocate)
        view->allocator.deallocate(view->allocator.user,
                                   (void *)view->catalog.entries);
    memset(view, 0, sizeof(*view));
}

bool st_class_graph_stats(const st_class_graph_result_t *result,
                          st_class_graph_stats_t *stats_out)
{
    graph_impl_t *impl;
    if (stats_out) memset(stats_out, 0, sizeof(*stats_out));
    if (!result || !result->implementation || !stats_out) return false;
    impl = result->implementation;
    catalog_lock(impl);
    stats_out->shared_global_count = result->catalog_entry_count;
    stats_out->compatibility_view_count = impl->compatibility_view_count;
    stats_out->compatibility_entry_count = impl->compatibility_entry_count;
    stats_out->method_index_capacity = impl->method_bucket_capacity;
    stats_out->method_index_probe_count = impl->method_index_probe_count;
    catalog_unlock(impl);
    return true;
}

const char *st_class_graph_status_string(st_class_graph_status_t status)
{
    switch (status) {
    case ST_CLASS_GRAPH_OK: return "ok";
    case ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_CLASS_GRAPH_ERR_OVERFLOW: return "size overflow";
    default: return "unknown class graph status";
    }
}

const char *st_class_graph_diagnostic_string(
    st_class_graph_diagnostic_code_t code)
{
    switch (code) {
    case ST_CLASS_GRAPH_DIAG_MALFORMED_AST: return "malformed AST";
    case ST_CLASS_GRAPH_DIAG_DUPLICATE_DEFINITION: return "duplicate definition";
    case ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_MISSING: return "extension target missing";
    case ST_CLASS_GRAPH_DIAG_EXTENSION_BEFORE_TARGET: return "extension precedes target";
    case ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_NOT_CLASS: return "extension target is not a class";
    case ST_CLASS_GRAPH_DIAG_SUPERCLASS_MISSING: return "superclass missing";
    case ST_CLASS_GRAPH_DIAG_SUPERCLASS_NOT_CLASS: return "superclass is not a class";
    case ST_CLASS_GRAPH_DIAG_INHERITANCE_CYCLE: return "inheritance cycle";
    case ST_CLASS_GRAPH_DIAG_DUPLICATE_METHOD: return "duplicate method";
    case ST_CLASS_GRAPH_DIAG_DUPLICATE_VARIABLE: return "duplicate variable";
    case ST_CLASS_GRAPH_DIAG_SLOT_OVERFLOW: return "slot overflow";
    default: return "unknown class graph diagnostic";
    }
}
