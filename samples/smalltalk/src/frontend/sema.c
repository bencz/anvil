#include "st_sema.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t hash;
    st_sema_scope_id_t scope;
    st_sema_binding_id_t binding_plus_one;
} binding_slot_t;

typedef struct {
    const st_ast_node_t *node;
    size_t index_plus_one;
} node_slot_t;

typedef struct {
    st_sema_binding_id_t *items;
    size_t count;
    size_t capacity;
} capture_work_t;

typedef struct {
    st_sema_allocator_t allocator;
    size_t binding_capacity;
    size_t reference_capacity;
    size_t scope_capacity;
    size_t block_capacity;
    size_t capture_capacity;
    size_t return_capacity;
    size_t diagnostic_capacity;
    binding_slot_t *binding_slots;
    size_t binding_slot_count;
    size_t binding_slot_capacity;
    node_slot_t *reference_slots;
    size_t reference_slot_count;
    size_t reference_slot_capacity;
    capture_work_t *block_captures;
    size_t block_capture_capacity;
    st_sema_binding_id_t *external_bindings;
    size_t external_binding_count;
    st_sema_binding_id_t self_binding;
    st_sema_binding_id_t super_binding;
    st_sema_binding_id_t context_binding;
    st_sema_scope_id_t root_scope;
    const st_ast_node_t *method;
    const st_sema_catalog_t *catalog;
} sema_impl_t;

typedef struct {
    st_sema_result_t *result;
    sema_impl_t *impl;
} analyzer_t;

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

static void release_memory(sema_impl_t *impl, void *pointer)
{
    if (pointer != NULL)
        impl->allocator.deallocate(impl->allocator.user, pointer);
}

static bool string_valid(st_ast_string_t string)
{
    return string.data != NULL && string.length != 0u;
}

static bool string_equal(st_ast_string_t left, st_ast_string_t right)
{
    return left.length == right.length
        && (left.length == 0u
            || memcmp(left.data, right.data, left.length) == 0);
}

static bool string_is(st_ast_string_t string, const char *literal)
{
    size_t length = strlen(literal);
    return string.length == length
        && (length == 0u || memcmp(string.data, literal, length) == 0);
}

static st_ast_string_t static_string(const char *literal)
{
    st_ast_string_t result;
    result.data = literal;
    result.length = strlen(literal);
    return result;
}

static uint64_t string_hash(st_ast_string_t string)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0u; index < string.length; index++) {
        hash ^= (unsigned char)string.data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0u ? UINT64_C(1) : hash;
}

static void set_status(analyzer_t *analyzer, st_sema_status_t status)
{
    if (analyzer->result->status == ST_SEMA_OK) {
        analyzer->result->status = status;
    }
}

static bool checked_add_size(analyzer_t *analyzer, size_t left, size_t right,
                             size_t *sum)
{
    if (left > SIZE_MAX - right) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return false;
    }
    *sum = left + right;
    return true;
}

static void *allocate_zeroed(analyzer_t *analyzer, size_t count,
                             size_t element_size)
{
    size_t bytes;
    void *memory;
    if (count == 0u || element_size == 0u) {
        set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    if (count > SIZE_MAX / element_size) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return NULL;
    }
    bytes = count * element_size;
    memory = analyzer->impl->allocator.allocate(
        analyzer->impl->allocator.user, bytes);
    if (memory == NULL) {
        set_status(analyzer, ST_SEMA_ERR_OUT_OF_MEMORY);
        return NULL;
    }
    memset(memory, 0, bytes);
    return memory;
}

static bool reserve_array(analyzer_t *analyzer, void **array,
                          size_t *capacity, size_t count,
                          size_t required, size_t element_size)
{
    size_t new_capacity;
    void *replacement;
    if ((*capacity == 0u) != (*array == NULL)) {
        set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
        return false;
    }
    if (required <= *capacity) {
        return true;
    }
    new_capacity = *capacity == 0u ? 8u : *capacity;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2u) {
            set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
            return false;
        }
        new_capacity *= 2u;
    }
    replacement = allocate_zeroed(analyzer, new_capacity, element_size);
    if (replacement == NULL) {
        return false;
    }
    if (count > SIZE_MAX / element_size) {
        release_memory(analyzer->impl, replacement);
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return false;
    }
    if (*array != NULL && count != 0u) {
        memcpy(replacement, *array, count * element_size);
    }
    release_memory(analyzer->impl, *array);
    *array = replacement;
    *capacity = new_capacity;
    return true;
}

static bool append_diagnostic(analyzer_t *analyzer,
                              st_sema_diagnostic_code_t code,
                              const st_ast_node_t *node,
                              st_ast_string_t name,
                              const st_ast_node_t *related)
{
    st_sema_result_t *result = analyzer->result;
    st_sema_diagnostic_t *diagnostic;
    size_t required;
    if (!checked_add_size(analyzer, result->diagnostic_count, 1u,
                          &required)) {
        return false;
    }
    if (!reserve_array(analyzer, (void **)&result->diagnostics,
                       &analyzer->impl->diagnostic_capacity,
                       result->diagnostic_count,
                       required,
                       sizeof(*result->diagnostics))) {
        return false;
    }
    diagnostic = &result->diagnostics[result->diagnostic_count++];
    diagnostic->code = code;
    if (node != NULL) {
        diagnostic->span = node->span;
    }
    diagnostic->name = name;
    if (related != NULL) {
        diagnostic->related_span = related->span;
        diagnostic->has_related_span = true;
    }
    return true;
}

static st_sema_binding_id_t append_binding(analyzer_t *analyzer,
                                           st_sema_binding_t binding)
{
    st_sema_result_t *result = analyzer->result;
    if (result->binding_count >= (size_t)ST_SEMA_INVALID_ID) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return ST_SEMA_INVALID_ID;
    }
    if (!reserve_array(analyzer, (void **)&result->bindings,
                       &analyzer->impl->binding_capacity,
                       result->binding_count,
                       result->binding_count + 1u,
                       sizeof(*result->bindings))) {
        return ST_SEMA_INVALID_ID;
    }
    result->bindings[result->binding_count] = binding;
    return (st_sema_binding_id_t)result->binding_count++;
}

static bool binding_table_rebuild(analyzer_t *analyzer, size_t capacity)
{
    binding_slot_t *slots;
    size_t old_capacity = analyzer->impl->binding_slot_capacity;
    binding_slot_t *old_slots = analyzer->impl->binding_slots;
    size_t index;
    slots = allocate_zeroed(analyzer, capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    analyzer->impl->binding_slots = slots;
    analyzer->impl->binding_slot_capacity = capacity;
    analyzer->impl->binding_slot_count = 0u;
    for (index = 0u; index < old_capacity; index++) {
        binding_slot_t old = old_slots[index];
        if (old.binding_plus_one != 0u) {
            size_t slot = (size_t)(old.hash & (uint64_t)(capacity - 1u));
            while (slots[slot].binding_plus_one != 0u) {
                slot = (slot + 1u) & (capacity - 1u);
            }
            slots[slot] = old;
            analyzer->impl->binding_slot_count++;
        }
    }
    release_memory(analyzer->impl, old_slots);
    return true;
}

static bool binding_table_prepare(analyzer_t *analyzer)
{
    size_t capacity = analyzer->impl->binding_slot_capacity;
    if (capacity == 0u) {
        return binding_table_rebuild(analyzer, 16u);
    }
    if (analyzer->impl->binding_slot_count + 1u <= capacity / 2u) {
        return true;
    }
    if (capacity > SIZE_MAX / 2u) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return false;
    }
    return binding_table_rebuild(analyzer, capacity * 2u);
}

static st_sema_binding_id_t binding_lookup_in_scope(
    const analyzer_t *analyzer, st_sema_scope_id_t scope,
    st_ast_string_t name)
{
    const sema_impl_t *impl = analyzer->impl;
    uint64_t hash;
    size_t slot;
    if (impl->binding_slot_capacity == 0u) {
        return ST_SEMA_INVALID_ID;
    }
    hash = string_hash(name) ^ ((uint64_t)scope * UINT64_C(11400714819323198485));
    if (hash == 0u) hash = 1u;
    slot = (size_t)(hash & (uint64_t)(impl->binding_slot_capacity - 1u));
    while (impl->binding_slots[slot].binding_plus_one != 0u) {
        const binding_slot_t *entry = &impl->binding_slots[slot];
        st_sema_binding_id_t binding = entry->binding_plus_one - 1u;
        if (entry->hash == hash && entry->scope == scope
                && string_equal(analyzer->result->bindings[binding].name,
                                name)) {
            return binding;
        }
        slot = (slot + 1u) & (impl->binding_slot_capacity - 1u);
    }
    return ST_SEMA_INVALID_ID;
}

static bool binding_table_insert(analyzer_t *analyzer,
                                 st_sema_scope_id_t scope,
                                 st_sema_binding_id_t binding)
{
    st_ast_string_t name = analyzer->result->bindings[binding].name;
    uint64_t hash = string_hash(name)
        ^ ((uint64_t)scope * UINT64_C(11400714819323198485));
    size_t slot;
    if (hash == 0u) hash = 1u;
    if (!binding_table_prepare(analyzer)) {
        return false;
    }
    slot = (size_t)(hash
        & (uint64_t)(analyzer->impl->binding_slot_capacity - 1u));
    while (analyzer->impl->binding_slots[slot].binding_plus_one != 0u) {
        slot = (slot + 1u)
            & (analyzer->impl->binding_slot_capacity - 1u);
    }
    analyzer->impl->binding_slots[slot].hash = hash;
    analyzer->impl->binding_slots[slot].scope = scope;
    analyzer->impl->binding_slots[slot].binding_plus_one = binding + 1u;
    analyzer->impl->binding_slot_count++;
    return true;
}

static bool reference_table_rebuild(analyzer_t *analyzer, size_t capacity)
{
    node_slot_t *slots;
    node_slot_t *old_slots = analyzer->impl->reference_slots;
    size_t old_capacity = analyzer->impl->reference_slot_capacity;
    size_t index;
    slots = allocate_zeroed(analyzer, capacity, sizeof(*slots));
    if (slots == NULL) return false;
    analyzer->impl->reference_slots = slots;
    analyzer->impl->reference_slot_capacity = capacity;
    analyzer->impl->reference_slot_count = 0u;
    for (index = 0u; index < old_capacity; index++) {
        node_slot_t old = old_slots[index];
        if (old.index_plus_one != 0u) {
            size_t slot = ((uintptr_t)old.node >> 3u) & (capacity - 1u);
            while (slots[slot].index_plus_one != 0u) {
                slot = (slot + 1u) & (capacity - 1u);
            }
            slots[slot] = old;
            analyzer->impl->reference_slot_count++;
        }
    }
    release_memory(analyzer->impl, old_slots);
    return true;
}

static bool reference_table_insert(analyzer_t *analyzer,
                                   const st_ast_node_t *node,
                                   size_t reference_index)
{
    size_t capacity = analyzer->impl->reference_slot_capacity;
    size_t slot;
    if (capacity == 0u) {
        if (!reference_table_rebuild(analyzer, 16u)) return false;
        capacity = 16u;
    } else if (analyzer->impl->reference_slot_count + 1u > capacity / 2u) {
        if (capacity > SIZE_MAX / 2u) {
            set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
            return false;
        }
        if (!reference_table_rebuild(analyzer, capacity * 2u)) return false;
        capacity *= 2u;
    }
    slot = ((uintptr_t)node >> 3u) & (capacity - 1u);
    while (analyzer->impl->reference_slots[slot].index_plus_one != 0u) {
        if (analyzer->impl->reference_slots[slot].node == node) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                              node, (st_ast_string_t){0}, NULL);
            return false;
        }
        slot = (slot + 1u) & (capacity - 1u);
    }
    analyzer->impl->reference_slots[slot].node = node;
    analyzer->impl->reference_slots[slot].index_plus_one = reference_index + 1u;
    analyzer->impl->reference_slot_count++;
    return true;
}

static bool append_reference(analyzer_t *analyzer,
                             const st_ast_node_t *site,
                             st_sema_binding_id_t binding,
                             st_sema_scope_id_t scope,
                             st_sema_access_t access)
{
    st_sema_result_t *result = analyzer->result;
    st_sema_reference_t reference;
    size_t required;
    if (!checked_add_size(analyzer, result->reference_count, 1u,
                          &required)) {
        return false;
    }
    if (!reserve_array(analyzer, (void **)&result->references,
                       &analyzer->impl->reference_capacity,
                       result->reference_count,
                       required,
                       sizeof(*result->references))) {
        return false;
    }
    reference.site = site;
    reference.binding = binding;
    reference.scope = scope;
    reference.access = access;
    result->references[result->reference_count] = reference;
    if (!reference_table_insert(analyzer, site, result->reference_count)) {
        return false;
    }
    result->reference_count++;
    return true;
}

static st_sema_scope_id_t append_scope(analyzer_t *analyzer,
                                       st_sema_scope_id_t parent,
                                       st_sema_block_id_t block)
{
    st_sema_result_t *result = analyzer->result;
    st_sema_scope_t scope;
    if (result->scope_count >= (size_t)ST_SEMA_INVALID_ID) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return ST_SEMA_INVALID_ID;
    }
    if (!reserve_array(analyzer, (void **)&result->scopes,
                       &analyzer->impl->scope_capacity,
                       result->scope_count, result->scope_count + 1u,
                       sizeof(*result->scopes))) {
        return ST_SEMA_INVALID_ID;
    }
    scope.parent = parent;
    scope.block = block;
    scope.first_binding = result->binding_count;
    scope.binding_count = 0u;
    result->scopes[result->scope_count] = scope;
    return (st_sema_scope_id_t)result->scope_count++;
}

static st_sema_block_id_t append_block(analyzer_t *analyzer,
                                       const st_ast_node_t *node,
                                       st_sema_block_id_t parent)
{
    st_sema_result_t *result = analyzer->result;
    st_sema_block_t block;
    size_t needed;
    if (result->block_count >= (size_t)ST_SEMA_INVALID_ID) {
        set_status(analyzer, ST_SEMA_ERR_OVERFLOW);
        return ST_SEMA_INVALID_ID;
    }
    if (!reserve_array(analyzer, (void **)&result->blocks,
                       &analyzer->impl->block_capacity,
                       result->block_count, result->block_count + 1u,
                       sizeof(*result->blocks))) {
        return ST_SEMA_INVALID_ID;
    }
    needed = result->block_count + 1u;
    if (!reserve_array(analyzer, (void **)&analyzer->impl->block_captures,
                       &analyzer->impl->block_capture_capacity,
                       result->block_count, needed,
                       sizeof(*analyzer->impl->block_captures))) {
        return ST_SEMA_INVALID_ID;
    }
    memset(&block, 0, sizeof(block));
    block.node = node;
    block.scope = ST_SEMA_INVALID_ID;
    block.parent = parent;
    result->blocks[result->block_count] = block;
    return (st_sema_block_id_t)result->block_count++;
}

static bool append_return(analyzer_t *analyzer,
                          const st_ast_node_t *expression,
                          st_sema_return_kind_t kind,
                          st_sema_block_id_t block)
{
    st_sema_result_t *result = analyzer->result;
    st_sema_return_t item;
    size_t required;
    if (!checked_add_size(analyzer, result->return_count, 1u, &required)) {
        return false;
    }
    if (!reserve_array(analyzer, (void **)&result->returns,
                       &analyzer->impl->return_capacity,
                       result->return_count, required,
                       sizeof(*result->returns))) {
        return false;
    }
    item.expression = expression;
    item.kind = kind;
    item.block = block;
    result->returns[result->return_count++] = item;
    return true;
}

static bool capture_add(analyzer_t *analyzer, st_sema_block_id_t block,
                        st_sema_binding_id_t binding)
{
    capture_work_t *captures;
    size_t index;
    size_t required;
    if (block == ST_SEMA_INVALID_ID || block >= analyzer->result->block_count) {
        return true;
    }
    captures = &analyzer->impl->block_captures[block];
    for (index = 0u; index < captures->count; index++) {
        if (captures->items[index] == binding) return true;
    }
    if (!checked_add_size(analyzer, captures->count, 1u, &required)) {
        return false;
    }
    if (!reserve_array(analyzer, (void **)&captures->items,
                       &captures->capacity, captures->count,
                       required, sizeof(*captures->items))) {
        return false;
    }
    captures->items[captures->count++] = binding;
    return true;
}

static st_ast_string_t declaration_name(const st_ast_node_t *node)
{
    if (node == NULL) return (st_ast_string_t){0};
    if (node->kind == ST_AST_VARIABLE) return node->as.variable.name;
    if (node->kind == ST_AST_NIL || node->kind == ST_AST_TRUE
            || node->kind == ST_AST_FALSE) return node->as.text;
    return (st_ast_string_t){0};
}

static bool reserved_name(st_ast_string_t name)
{
    return string_is(name, "self") || string_is(name, "super")
        || string_is(name, "thisContext") || string_is(name, "nil")
        || string_is(name, "true") || string_is(name, "false");
}

static st_sema_binding_id_t declare_local(analyzer_t *analyzer,
                                          st_sema_scope_id_t scope,
                                          const st_ast_node_t *declaration,
                                          st_sema_binding_kind_t kind)
{
    st_ast_string_t name = declaration_name(declaration);
    st_sema_binding_id_t existing;
    st_sema_binding_t binding;
    st_sema_binding_id_t id;
    if (!string_valid(name)) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          declaration, name, NULL);
        return ST_SEMA_INVALID_ID;
    }
    if (declaration->kind != ST_AST_VARIABLE || reserved_name(name)) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_RESERVED_DECLARATION,
                          declaration, name, NULL);
        return ST_SEMA_INVALID_ID;
    }
    existing = binding_lookup_in_scope(analyzer, scope, name);
    if (existing != ST_SEMA_INVALID_ID) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_DUPLICATE_DECLARATION,
                          declaration, name,
                          analyzer->result->bindings[existing].declaration);
        return existing;
    }
    memset(&binding, 0, sizeof(binding));
    binding.kind = kind;
    binding.name = name;
    binding.declaration = declaration;
    binding.scope = scope;
    binding.slot = (uint32_t)analyzer->result->scopes[scope].binding_count;
    binding.external_id = ST_SEMA_INVALID_ID;
    if (kind == ST_SEMA_BIND_METHOD_ARGUMENT
            || kind == ST_SEMA_BIND_BLOCK_ARGUMENT) {
        binding.flags |= ST_SEMA_BINDING_READONLY;
    }
    binding.has_type = declaration->as.variable.has_type;
    binding.type_name = declaration->as.variable.type_name;
    id = append_binding(analyzer, binding);
    if (id == ST_SEMA_INVALID_ID) return id;
    if (!binding_table_insert(analyzer, scope, id)) return ST_SEMA_INVALID_ID;
    analyzer->result->scopes[scope].binding_count++;
    return id;
}

static st_sema_binding_id_t pseudo_binding(analyzer_t *analyzer,
                                           st_sema_binding_kind_t kind)
{
    st_sema_binding_id_t *cached;
    st_sema_binding_t binding;
    const char *name;
    switch (kind) {
    case ST_SEMA_BIND_SELF:
        cached = &analyzer->impl->self_binding;
        name = "self";
        break;
    case ST_SEMA_BIND_SUPER:
        cached = &analyzer->impl->super_binding;
        name = "super";
        break;
    case ST_SEMA_BIND_THIS_CONTEXT:
        cached = &analyzer->impl->context_binding;
        name = "thisContext";
        break;
    default:
        set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
        return ST_SEMA_INVALID_ID;
    }
    if (*cached != ST_SEMA_INVALID_ID) return *cached;
    memset(&binding, 0, sizeof(binding));
    binding.kind = kind;
    binding.name = static_string(name);
    binding.scope = analyzer->impl->root_scope;
    binding.slot = ST_SEMA_INVALID_ID;
    binding.external_id = ST_SEMA_INVALID_ID;
    binding.flags = ST_SEMA_BINDING_READONLY | ST_SEMA_BINDING_IMPLICIT;
    *cached = append_binding(analyzer, binding);
    return *cached;
}

static st_sema_binding_kind_t external_binding_kind(
    st_sema_external_kind_t kind)
{
    switch (kind) {
    case ST_SEMA_EXTERNAL_INSTANCE_VARIABLE:
        return ST_SEMA_BIND_INSTANCE_VARIABLE;
    case ST_SEMA_EXTERNAL_CLASS_VARIABLE:
        return ST_SEMA_BIND_CLASS_VARIABLE;
    case ST_SEMA_EXTERNAL_GLOBAL:
        return ST_SEMA_BIND_GLOBAL;
    case ST_SEMA_EXTERNAL_FORWARD_GLOBAL:
        return ST_SEMA_BIND_FORWARD_GLOBAL;
    default:
        return ST_SEMA_BIND_GLOBAL;
    }
}

static st_sema_binding_id_t external_binding(analyzer_t *analyzer,
                                             size_t catalog_index)
{
    st_sema_binding_id_t id = analyzer->impl->external_bindings[catalog_index];
    const st_sema_external_t *external;
    st_sema_binding_t binding;
    if (id != ST_SEMA_INVALID_ID) return id;
    external = &analyzer->impl->catalog->entries[catalog_index];
    memset(&binding, 0, sizeof(binding));
    binding.kind = external_binding_kind(external->kind);
    binding.name = external->name;
    binding.scope = ST_SEMA_INVALID_ID;
    binding.slot = external->slot;
    binding.external_id = external->external_id;
    binding.flags = ST_SEMA_BINDING_EXTERNAL;
    id = append_binding(analyzer, binding);
    analyzer->impl->external_bindings[catalog_index] = id;
    return id;
}

static size_t catalog_find(const analyzer_t *analyzer, st_ast_string_t name)
{
    const st_sema_catalog_t *catalog = analyzer->impl->catalog;
    st_sema_external_kind_t wanted;
    size_t index;
    for (wanted = ST_SEMA_EXTERNAL_INSTANCE_VARIABLE;
         wanted <= ST_SEMA_EXTERNAL_FORWARD_GLOBAL;
         wanted = (st_sema_external_kind_t)(wanted + 1)) {
        for (index = 0u; index < catalog->count; index++) {
            if (catalog->entries[index].kind == wanted
                    && string_equal(catalog->entries[index].name, name)) {
                return index;
            }
        }
    }
    return SIZE_MAX;
}

static st_sema_binding_id_t lexical_lookup(const analyzer_t *analyzer,
                                           st_sema_scope_id_t scope,
                                           st_ast_string_t name,
                                           st_sema_scope_id_t *declaration_scope)
{
    while (scope != ST_SEMA_INVALID_ID) {
        st_sema_binding_id_t binding = binding_lookup_in_scope(
            analyzer, scope, name);
        if (binding != ST_SEMA_INVALID_ID) {
            *declaration_scope = scope;
            return binding;
        }
        scope = analyzer->result->scopes[scope].parent;
    }
    return ST_SEMA_INVALID_ID;
}

static bool capture_between(analyzer_t *analyzer,
                            st_sema_scope_id_t use_scope,
                            st_sema_scope_id_t declaration_scope,
                            st_sema_binding_id_t binding)
{
    st_sema_scope_id_t scope = use_scope;
    while (scope != declaration_scope && scope != ST_SEMA_INVALID_ID) {
        st_sema_block_id_t block = analyzer->result->scopes[scope].block;
        if (block != ST_SEMA_INVALID_ID && !capture_add(analyzer, block, binding)) {
            return false;
        }
        scope = analyzer->result->scopes[scope].parent;
    }
    if (scope != declaration_scope) {
        set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
        return false;
    }
    if (use_scope != declaration_scope) {
        analyzer->result->bindings[binding].flags |= ST_SEMA_BINDING_CAPTURED;
    }
    return true;
}

static bool capture_self(analyzer_t *analyzer, st_sema_scope_id_t scope)
{
    st_sema_binding_id_t self = pseudo_binding(analyzer, ST_SEMA_BIND_SELF);
    if (self == ST_SEMA_INVALID_ID) return false;
    return capture_between(analyzer, scope, analyzer->impl->root_scope, self);
}

static st_sema_binding_id_t resolve_variable(analyzer_t *analyzer,
                                             const st_ast_node_t *site,
                                             st_sema_scope_id_t scope,
                                             st_sema_access_t access)
{
    st_ast_string_t name;
    st_sema_binding_id_t binding = ST_SEMA_INVALID_ID;
    st_sema_scope_id_t declaration_scope = ST_SEMA_INVALID_ID;
    size_t external_index;
    bool write = access == ST_SEMA_ACCESS_WRITE;
    if (site == NULL || site->kind != ST_AST_VARIABLE
            || !string_valid(site->as.variable.name)) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          site, (st_ast_string_t){0}, NULL);
        return ST_SEMA_INVALID_ID;
    }
    name = site->as.variable.name;
    if (string_is(name, "self") || string_is(name, "super")
            || string_is(name, "thisContext")) {
        st_sema_binding_kind_t kind = string_is(name, "self")
            ? ST_SEMA_BIND_SELF : string_is(name, "super")
                ? ST_SEMA_BIND_SUPER : ST_SEMA_BIND_THIS_CONTEXT;
        binding = pseudo_binding(analyzer, kind);
        if (kind == ST_SEMA_BIND_SUPER && !analyzer->impl->catalog->has_lexical_super) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_INVALID_SUPER,
                              site, name, NULL);
        }
        if (write) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_READONLY_ASSIGNMENT,
                              site, name, NULL);
        } else if (kind == ST_SEMA_BIND_THIS_CONTEXT) {
            analyzer->result->requires_context = true;
        } else {
            capture_self(analyzer, scope);
        }
        append_reference(analyzer, site, binding, scope, access);
        return binding;
    }

    binding = lexical_lookup(analyzer, scope, name, &declaration_scope);
    if (binding != ST_SEMA_INVALID_ID) {
        if (write) {
            if ((analyzer->result->bindings[binding].flags
                    & ST_SEMA_BINDING_READONLY) != 0u) {
                append_diagnostic(analyzer, ST_SEMA_DIAG_READONLY_ASSIGNMENT,
                                  site, name,
                                  analyzer->result->bindings[binding].declaration);
            } else {
                analyzer->result->bindings[binding].flags
                    |= ST_SEMA_BINDING_ASSIGNED;
            }
        }
        capture_between(analyzer, scope, declaration_scope, binding);
        append_reference(analyzer, site, binding, scope, access);
        return binding;
    }

    external_index = catalog_find(analyzer, name);
    if (external_index != SIZE_MAX) {
        binding = external_binding(analyzer, external_index);
        if (binding != ST_SEMA_INVALID_ID && write) {
            analyzer->result->bindings[binding].flags
                |= ST_SEMA_BINDING_ASSIGNED;
        }
        if (binding != ST_SEMA_INVALID_ID
                && analyzer->result->bindings[binding].kind
                    == ST_SEMA_BIND_INSTANCE_VARIABLE) {
            capture_self(analyzer, scope);
        }
        append_reference(analyzer, site, binding, scope, access);
        return binding;
    }

    append_diagnostic(analyzer, ST_SEMA_DIAG_UNDEFINED_NAME,
                      site, name, NULL);
    return ST_SEMA_INVALID_ID;
}

static void analyze_value(analyzer_t *analyzer, const st_ast_node_t *node,
                          st_sema_scope_id_t scope,
                          st_sema_block_id_t current_block);

static bool list_well_formed(const st_ast_list_t *list)
{
    return list != NULL && (list->count == 0u || list->items != NULL)
        && list->count <= list->capacity;
}

static void analyze_expression(analyzer_t *analyzer,
                               const st_ast_node_t *expression,
                               st_sema_scope_id_t scope,
                               st_sema_block_id_t current_block)
{
    const st_ast_node_t *receiver;
    size_t index;
    bool receiver_is_super;
    if (expression == NULL || expression->kind != ST_AST_EXPRESSION
            || !list_well_formed(&expression->as.expression.assignments)
            || !list_well_formed(&expression->as.expression.messages)) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          expression, (st_ast_string_t){0}, NULL);
        return;
    }
    receiver = expression->as.expression.receiver;
    if (receiver == NULL) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          expression, (st_ast_string_t){0}, NULL);
        return;
    }
    receiver_is_super = receiver->kind == ST_AST_VARIABLE
        && string_is(receiver->as.variable.name, "super");
    if (receiver_is_super) {
        resolve_variable(analyzer, receiver, scope,
            expression->as.expression.messages.count == 0u
                ? ST_SEMA_ACCESS_READ : ST_SEMA_ACCESS_SUPER_RECEIVER);
    } else {
        analyze_value(analyzer, receiver, scope, current_block);
    }
    for (index = 0u; index < expression->as.expression.messages.count; index++) {
        const st_ast_node_t *message = expression->as.expression.messages.items[index];
        size_t argument;
        if (message == NULL || message->kind != ST_AST_MESSAGE
                || !string_valid(message->as.message.selector)
                || !list_well_formed(&message->as.message.arguments)) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                              message, (st_ast_string_t){0}, NULL);
            continue;
        }
        if (message->as.message.super_send != receiver_is_super) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                              message, message->as.message.selector, receiver);
        }
        for (argument = 0u; argument < message->as.message.arguments.count;
             argument++) {
            analyze_value(analyzer, message->as.message.arguments.items[argument],
                          scope, current_block);
        }
    }
    for (index = 0u; index < expression->as.expression.assignments.count; index++) {
        const st_ast_node_t *target = expression->as.expression.assignments.items[index];
        if (target == NULL) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                              expression, (st_ast_string_t){0}, NULL);
        } else if (target->kind == ST_AST_VARIABLE) {
            resolve_variable(analyzer, target, scope, ST_SEMA_ACCESS_WRITE);
        } else if (target->kind == ST_AST_NIL || target->kind == ST_AST_TRUE
                || target->kind == ST_AST_FALSE) {
            append_diagnostic(analyzer, ST_SEMA_DIAG_READONLY_ASSIGNMENT,
                              target, target->as.text, NULL);
        } else {
            append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                              target, (st_ast_string_t){0}, NULL);
        }
    }
    if (expression->as.expression.returns) {
        if (current_block == ST_SEMA_INVALID_ID) {
            append_return(analyzer, expression, ST_SEMA_RETURN_LOCAL_METHOD,
                          ST_SEMA_INVALID_ID);
        } else {
            append_return(analyzer, expression, ST_SEMA_RETURN_HOME_METHOD,
                          current_block);
            analyzer->result->blocks[current_block].has_nonlocal_return = true;
            analyzer->result->requires_context = true;
            analyzer->result->may_be_nonlocal_return_home = true;
        }
    }
}

static void analyze_block(analyzer_t *analyzer, const st_ast_node_t *node,
                          st_sema_scope_id_t parent_scope,
                          st_sema_block_id_t parent_block)
{
    st_sema_block_id_t block;
    st_sema_scope_id_t scope;
    size_t index;
    if (node == NULL || node->kind != ST_AST_BLOCK
            || !list_well_formed(&node->as.block.arguments)
            || !list_well_formed(&node->as.block.temporaries)
            || !list_well_formed(&node->as.block.expressions)) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          node, (st_ast_string_t){0}, NULL);
        return;
    }
    block = append_block(analyzer, node, parent_block);
    if (block == ST_SEMA_INVALID_ID) return;
    scope = append_scope(analyzer, parent_scope, block);
    if (scope == ST_SEMA_INVALID_ID) return;
    analyzer->result->blocks[block].scope = scope;
    for (index = 0u; index < node->as.block.arguments.count; index++) {
        declare_local(analyzer, scope, node->as.block.arguments.items[index],
                      ST_SEMA_BIND_BLOCK_ARGUMENT);
    }
    for (index = 0u; index < node->as.block.temporaries.count; index++) {
        declare_local(analyzer, scope, node->as.block.temporaries.items[index],
                      ST_SEMA_BIND_TEMPORARY);
    }
    for (index = 0u; index < node->as.block.expressions.count; index++) {
        analyze_expression(analyzer, node->as.block.expressions.items[index],
                           scope, block);
    }
}

static void analyze_value(analyzer_t *analyzer, const st_ast_node_t *node,
                          st_sema_scope_id_t scope,
                          st_sema_block_id_t current_block)
{
    if (node == NULL) {
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          NULL, (st_ast_string_t){0}, NULL);
        return;
    }
    switch (node->kind) {
    case ST_AST_VARIABLE:
        resolve_variable(analyzer, node, scope, ST_SEMA_ACCESS_READ);
        break;
    case ST_AST_EXPRESSION:
        analyze_expression(analyzer, node, scope, current_block);
        break;
    case ST_AST_BLOCK:
        analyze_block(analyzer, node, scope, current_block);
        break;
    case ST_AST_NIL:
    case ST_AST_TRUE:
    case ST_AST_FALSE:
    case ST_AST_INTEGER:
    case ST_AST_FLOAT:
    case ST_AST_SCALED_DECIMAL:
    case ST_AST_SYMBOL:
    case ST_AST_STRING:
    case ST_AST_CHARACTER:
    case ST_AST_LITERAL_ARRAY:
        break;
    default:
        append_diagnostic(analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          node, (st_ast_string_t){0}, NULL);
        break;
    }
}

static bool validate_catalog(analyzer_t *analyzer)
{
    const st_sema_catalog_t *catalog = analyzer->impl->catalog;
    size_t index;
    size_t other;
    if (catalog->count != 0u && catalog->entries == NULL) {
        set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
        return false;
    }
    for (index = 0u; index < catalog->count; index++) {
        const st_sema_external_t *entry = &catalog->entries[index];
        if (!string_valid(entry->name)
                || entry->kind < ST_SEMA_EXTERNAL_INSTANCE_VARIABLE
                || entry->kind > ST_SEMA_EXTERNAL_FORWARD_GLOBAL) {
            set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
            return false;
        }
        for (other = 0u; other < index; other++) {
            if (catalog->entries[other].kind == entry->kind
                    && string_equal(catalog->entries[other].name, entry->name)) {
                set_status(analyzer, ST_SEMA_ERR_INVALID_ARGUMENT);
                return false;
            }
        }
    }
    return true;
}

static bool method_body_arguments_match(const st_ast_node_t *method)
{
    const st_ast_node_t *body = method->as.method.body;
    size_t index;
    if (body == NULL || body->kind != ST_AST_BLOCK
            || !list_well_formed(&method->as.method.arguments)
            || !list_well_formed(&body->as.block.arguments)
            || method->as.method.arguments.count != body->as.block.arguments.count) {
        return false;
    }
    for (index = 0u; index < method->as.method.arguments.count; index++) {
        if (method->as.method.arguments.items[index]
                != body->as.block.arguments.items[index]) return false;
    }
    return true;
}

static bool finalize_captures(analyzer_t *analyzer)
{
    st_sema_result_t *result = analyzer->result;
    size_t binding;
    size_t block;
    for (binding = 0u; binding < result->binding_count; binding++) {
        uint32_t flags = result->bindings[binding].flags;
        if ((flags & ST_SEMA_BINDING_CAPTURED) != 0u
                && (flags & ST_SEMA_BINDING_ASSIGNED) != 0u) {
            result->bindings[binding].flags |= ST_SEMA_BINDING_NEEDS_CELL;
        }
    }
    for (block = 0u; block < result->block_count; block++) {
        capture_work_t *work = &analyzer->impl->block_captures[block];
        size_t index;
        size_t required;
        result->blocks[block].capture_offset = result->capture_count;
        result->blocks[block].capture_count = work->count;
        if (!checked_add_size(analyzer, result->capture_count, work->count,
                              &required)) {
            return false;
        }
        if (!reserve_array(analyzer, (void **)&result->captures,
                           &analyzer->impl->capture_capacity,
                           result->capture_count,
                           required,
                           sizeof(*result->captures))) {
            return false;
        }
        for (index = 0u; index < work->count; index++) {
            st_sema_binding_id_t id = work->items[index];
            st_sema_capture_t capture;
            capture.binding = id;
            if (result->bindings[id].kind == ST_SEMA_BIND_SELF) {
                capture.mode = ST_SEMA_CAPTURE_SELF;
            } else if ((result->bindings[id].flags
                        & ST_SEMA_BINDING_NEEDS_CELL) != 0u) {
                capture.mode = ST_SEMA_CAPTURE_CELL;
            } else {
                capture.mode = ST_SEMA_CAPTURE_VALUE;
            }
            result->captures[result->capture_count++] = capture;
        }
    }
    return true;
}

void st_sema_result_init(st_sema_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = ST_SEMA_OK;
    }
}

void st_sema_result_destroy(st_sema_result_t *result)
{
    sema_impl_t *impl;
    size_t index;
    if (result == NULL) return;
    impl = result->implementation;
    if (impl == NULL) {
        memset(result, 0, sizeof(*result));
        return;
    }
    for (index = 0u; index < result->block_count; index++) {
        release_memory(impl, impl->block_captures[index].items);
    }
    release_memory(impl, result->bindings);
    release_memory(impl, result->references);
    release_memory(impl, result->scopes);
    release_memory(impl, result->blocks);
    release_memory(impl, result->captures);
    release_memory(impl, result->returns);
    release_memory(impl, result->diagnostics);
    release_memory(impl, impl->binding_slots);
    release_memory(impl, impl->reference_slots);
    release_memory(impl, impl->block_captures);
    release_memory(impl, impl->external_bindings);
    impl->allocator.deallocate(impl->allocator.user, impl);
    memset(result, 0, sizeof(*result));
}

st_sema_status_t st_sema_analyze_method(st_sema_result_t *result,
                                        const st_ast_node_t *method,
                                        const st_sema_catalog_t *catalog)
{
    st_sema_allocator_t allocator;
    sema_impl_t *impl;
    analyzer_t analyzer;
    const st_ast_node_t *body;
    size_t index;
    if (result == NULL || method == NULL || catalog == NULL
            || result->implementation != NULL || result->bindings != NULL
            || result->diagnostics != NULL) {
        if (result != NULL) result->status = ST_SEMA_ERR_INVALID_ARGUMENT;
        return ST_SEMA_ERR_INVALID_ARGUMENT;
    }
    allocator = catalog->allocator;
    if (allocator.allocate == NULL && allocator.deallocate == NULL) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
        allocator.user = NULL;
    } else if (allocator.allocate == NULL || allocator.deallocate == NULL) {
        result->status = ST_SEMA_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    impl = allocator.allocate(allocator.user, sizeof(*impl));
    if (impl == NULL) {
        result->status = ST_SEMA_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    memset(impl, 0, sizeof(*impl));
    impl->allocator = allocator;
    impl->self_binding = ST_SEMA_INVALID_ID;
    impl->super_binding = ST_SEMA_INVALID_ID;
    impl->context_binding = ST_SEMA_INVALID_ID;
    impl->root_scope = ST_SEMA_INVALID_ID;
    impl->method = method;
    impl->catalog = catalog;
    result->implementation = impl;
    result->status = ST_SEMA_OK;
    analyzer.result = result;
    analyzer.impl = impl;

    if (!validate_catalog(&analyzer)) return result->status;
    if (catalog->count != 0u) {
        impl->external_bindings = allocate_zeroed(
            &analyzer, catalog->count, sizeof(*impl->external_bindings));
        if (impl->external_bindings == NULL) return result->status;
        impl->external_binding_count = catalog->count;
        for (index = 0u; index < catalog->count; index++) {
            impl->external_bindings[index] = ST_SEMA_INVALID_ID;
        }
    }
    if (method->kind != ST_AST_METHOD || !string_valid(method->as.method.selector)
            || !method_body_arguments_match(method)) {
        append_diagnostic(&analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          method, (st_ast_string_t){0}, NULL);
        return result->status;
    }
    body = method->as.method.body;
    if (!list_well_formed(&body->as.block.temporaries)
            || !list_well_formed(&body->as.block.expressions)) {
        append_diagnostic(&analyzer, ST_SEMA_DIAG_MALFORMED_AST,
                          body, (st_ast_string_t){0}, NULL);
        return result->status;
    }
    impl->root_scope = append_scope(&analyzer, ST_SEMA_INVALID_ID,
                                    ST_SEMA_INVALID_ID);
    if (impl->root_scope == ST_SEMA_INVALID_ID) return result->status;
    for (index = 0u; index < method->as.method.arguments.count; index++) {
        declare_local(&analyzer, impl->root_scope,
                      method->as.method.arguments.items[index],
                      ST_SEMA_BIND_METHOD_ARGUMENT);
    }
    for (index = 0u; index < body->as.block.temporaries.count; index++) {
        declare_local(&analyzer, impl->root_scope,
                      body->as.block.temporaries.items[index],
                      ST_SEMA_BIND_TEMPORARY);
    }
    for (index = 0u; index < body->as.block.expressions.count; index++) {
        analyze_expression(&analyzer, body->as.block.expressions.items[index],
                           impl->root_scope, ST_SEMA_INVALID_ID);
    }
    if (result->status == ST_SEMA_OK) finalize_captures(&analyzer);
    return result->status;
}

bool st_sema_succeeded(const st_sema_result_t *result)
{
    return result != NULL && result->status == ST_SEMA_OK
        && result->diagnostic_count == 0u;
}

const st_sema_reference_t *st_sema_reference_for_node(
    const st_sema_result_t *result, const st_ast_node_t *node)
{
    const sema_impl_t *impl;
    size_t slot;
    if (result == NULL || node == NULL || result->implementation == NULL) {
        return NULL;
    }
    impl = result->implementation;
    if (impl->reference_slot_capacity == 0u) return NULL;
    slot = ((uintptr_t)node >> 3u) & (impl->reference_slot_capacity - 1u);
    while (impl->reference_slots[slot].index_plus_one != 0u) {
        if (impl->reference_slots[slot].node == node) {
            return &result->references[
                impl->reference_slots[slot].index_plus_one - 1u];
        }
        slot = (slot + 1u) & (impl->reference_slot_capacity - 1u);
    }
    return NULL;
}

const st_sema_block_t *st_sema_block_for_node(
    const st_sema_result_t *result, const st_ast_node_t *node)
{
    size_t index;
    if (result == NULL || node == NULL) return NULL;
    for (index = 0u; index < result->block_count; index++) {
        if (result->blocks[index].node == node) return &result->blocks[index];
    }
    return NULL;
}

const char *st_sema_status_string(st_sema_status_t status)
{
    switch (status) {
    case ST_SEMA_OK: return "ok";
    case ST_SEMA_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_SEMA_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_SEMA_ERR_OVERFLOW: return "size overflow";
    default: return "invalid semantic-analysis status";
    }
}

const char *st_sema_diagnostic_string(st_sema_diagnostic_code_t code)
{
    switch (code) {
    case ST_SEMA_DIAG_MALFORMED_AST: return "malformed AST";
    case ST_SEMA_DIAG_DUPLICATE_DECLARATION: return "duplicate declaration";
    case ST_SEMA_DIAG_RESERVED_DECLARATION: return "reserved name declaration";
    case ST_SEMA_DIAG_READONLY_ASSIGNMENT: return "assignment to readonly name";
    case ST_SEMA_DIAG_UNDEFINED_NAME: return "undefined name";
    case ST_SEMA_DIAG_INVALID_SUPER: return "super has no lexical superclass";
    case ST_SEMA_DIAG_RETURN_WITHOUT_HOME: return "return has no home method";
    default: return "invalid semantic diagnostic";
    }
}
