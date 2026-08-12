#include "st_primitive.h"

#include <stdlib.h>
#include <string.h>

struct st_primitive_catalog_entry {
    st_primitive_t primitive;
    uint64_t hash;
    char payload[];
};

struct st_primitive_catalog_slot {
    uint64_t hash;
    st_primitive_catalog_entry_t *entry;
};

typedef struct {
    st_primitive_result_t value;
    size_t binding_capacity;
    size_t diagnostic_capacity;
} result_builder_t;

#define ST_PRIMITIVE_MAX_AST_DEPTH 512u
#define ST_PRIMITIVE_INITIAL_ENTRY_CAPACITY 8u
#define ST_PRIMITIVE_INITIAL_TABLE_CAPACITY 16u

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

static bool allocator_normalize(st_primitive_allocator_t input,
                                st_primitive_allocator_t *output)
{
    if (!output) return false;
    if (!input.allocate && !input.deallocate) {
        output->allocate = default_allocate;
        output->deallocate = default_deallocate;
        output->user = NULL;
        return true;
    }
    if (!input.allocate || !input.deallocate) return false;
    *output = input;
    return true;
}

static void release(st_primitive_allocator_t allocator, void *pointer)
{
    if (pointer) allocator.deallocate(allocator.user, pointer);
}

static bool add_size(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0u && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

static bool identifier_valid(const char *bytes, size_t length)
{
    size_t index;
    unsigned char byte;
    if (!bytes || length == 0u) return false;
    byte = (unsigned char)bytes[0];
    if (!((byte >= 'A' && byte <= 'Z') ||
          (byte >= 'a' && byte <= 'z') || byte == '_')) return false;
    for (index = 1u; index < length; index++) {
        byte = (unsigned char)bytes[index];
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) return false;
    }
    return true;
}

static bool string_equal(st_ast_string_t string,
                         const void *bytes, size_t length)
{
    return string.length == length &&
        (length == 0u || (string.data && bytes &&
         memcmp(string.data, bytes, length) == 0));
}

static uint64_t primitive_hash(const unsigned char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0u; index < length; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    /* Zero is permitted as a hash because an empty slot is identified by its
     * null entry, not by a sentinel hash. */
    return hash;
}

static size_t probe_distance(size_t index, uint64_t hash, size_t mask)
{
    return (index - ((size_t)hash & mask)) & mask;
}

static void table_insert(st_primitive_catalog_slot_t *slots, size_t capacity,
                         st_primitive_catalog_entry_t *entry)
{
    size_t mask = capacity - 1u;
    size_t index = (size_t)entry->hash & mask;
    size_t distance = 0u;
    st_primitive_catalog_slot_t incoming = { entry->hash, entry };
    for (;;) {
        st_primitive_catalog_slot_t *slot = &slots[index];
        size_t resident_distance;
        if (!slot->entry) {
            *slot = incoming;
            return;
        }
        resident_distance = probe_distance(index, slot->hash, mask);
        if (resident_distance < distance) {
            st_primitive_catalog_slot_t displaced = *slot;
            *slot = incoming;
            incoming = displaced;
            distance = resident_distance;
        }
        index = (index + 1u) & mask;
        distance++;
    }
}

static st_primitive_catalog_entry_t *table_lookup(
    const st_primitive_catalog_t *catalog, const void *name, size_t length,
    uint64_t hash)
{
    size_t mask;
    size_t index;
    size_t distance = 0u;
    if (!catalog->slots || catalog->table_capacity == 0u) return NULL;
    mask = catalog->table_capacity - 1u;
    index = (size_t)hash & mask;
    for (;;) {
        const st_primitive_catalog_slot_t *slot = &catalog->slots[index];
        if (!slot->entry ||
            probe_distance(index, slot->hash, mask) < distance) return NULL;
        if (slot->hash == hash &&
            string_equal(slot->entry->primitive.name, name, length))
            return slot->entry;
        index = (index + 1u) & mask;
        distance++;
        if (distance == catalog->table_capacity) return NULL;
    }
}

static bool next_capacity(size_t current, size_t initial, size_t item_size,
                          size_t *capacity_out)
{
    size_t next = current ? current * 2u : initial;
    if ((current && current > SIZE_MAX / 2u) || next > SIZE_MAX / item_size)
        return false;
    *capacity_out = next;
    return true;
}

static bool spec_valid(const st_primitive_spec_t *spec,
                       st_primitive_status_t *status)
{
    if (!spec || !status) return false;
    if (!identifier_valid(spec->name, spec->name_length)) {
        *status = ST_PRIMITIVE_ERR_INVALID_NAME;
        return false;
    }
    if (spec->receiver_policy > ST_PRIMITIVE_INSTANCE_OR_CLASS ||
        spec->failure_policy > ST_PRIMITIVE_FALL_THROUGH ||
        spec->implementation_kind > ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL) {
        *status = ST_PRIMITIVE_ERR_INVALID_ARGUMENT;
        return false;
    }
    if (spec->implementation_kind == ST_PRIMITIVE_INTRINSIC) {
        if (spec->intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID ||
            spec->runtime_symbol || spec->runtime_symbol_length != 0u) {
            *status = ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION;
            return false;
        }
    } else if (spec->intrinsic_id != ST_PRIMITIVE_INVALID_INTRINSIC_ID
               || !identifier_valid(spec->runtime_symbol,
                                    spec->runtime_symbol_length)) {
        *status = ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION;
        return false;
    }
    return true;
}

static bool implementation_equal(const st_primitive_t *old,
                                 const st_primitive_spec_t *spec)
{
    if (old->method_arity != spec->method_arity ||
        old->receiver_policy != spec->receiver_policy ||
        old->failure_policy != spec->failure_policy ||
        old->implementation_kind != spec->implementation_kind ||
        old->intrinsic_id != spec->intrinsic_id) return false;
    if (old->implementation_kind != ST_PRIMITIVE_INTRINSIC)
        return string_equal(old->runtime_symbol, spec->runtime_symbol,
                            spec->runtime_symbol_length);
    return true;
}

bool st_primitive_catalog_init(st_primitive_catalog_t *catalog,
                               st_primitive_allocator_t allocator)
{
    st_primitive_allocator_t normalized;
    if (!catalog || catalog->initialized ||
        !allocator_normalize(allocator, &normalized)) return false;
    memset(catalog, 0, sizeof(*catalog));
    catalog->allocator = normalized;
    catalog->status = ST_PRIMITIVE_OK;
    catalog->initialized = true;
    return true;
}

void st_primitive_catalog_destroy(st_primitive_catalog_t *catalog)
{
    size_t index;
    if (!catalog) return;
    for (index = 0u; index < catalog->count; index++)
        release(catalog->allocator, catalog->entries[index]);
    release(catalog->allocator, catalog->entries);
    release(catalog->allocator, catalog->slots);
    memset(catalog, 0, sizeof(*catalog));
}

const st_primitive_t *st_primitive_catalog_lookup(
    const st_primitive_catalog_t *catalog, const void *name, size_t length)
{
    uint64_t hash;
    st_primitive_catalog_entry_t *entry;
    if (!catalog || !catalog->initialized || !name || length == 0u)
        return NULL;
    hash = primitive_hash(name, length);
    entry = table_lookup(catalog, name, length, hash);
    return entry ? &entry->primitive : NULL;
}

st_primitive_status_t st_primitive_catalog_register(
    st_primitive_catalog_t *catalog, const st_primitive_spec_t *spec,
    const st_primitive_t **primitive_out)
{
    st_primitive_status_t status = ST_PRIMITIVE_OK;
    const st_primitive_t *old;
    st_primitive_catalog_entry_t *entry;
    st_primitive_catalog_entry_t **new_entries = NULL;
    st_primitive_catalog_slot_t *new_slots = NULL;
    size_t new_entry_capacity = catalog ? catalog->entry_capacity : 0u;
    size_t new_table_capacity = catalog ? catalog->table_capacity : 0u;
    size_t total;
    size_t string_size;
    size_t allocation_size;
    size_t index;
    char *cursor;
    if (primitive_out) *primitive_out = NULL;
    if (!catalog || !catalog->initialized || !spec) {
        if (catalog) catalog->status = ST_PRIMITIVE_ERR_INVALID_ARGUMENT;
        return ST_PRIMITIVE_ERR_INVALID_ARGUMENT;
    }
    if (!spec_valid(spec, &status)) {
        catalog->status = status;
        return status;
    }
    old = st_primitive_catalog_lookup(catalog, spec->name, spec->name_length);
    if (old) {
        status = implementation_equal(old, spec)
            ? ST_PRIMITIVE_ERR_DUPLICATE : ST_PRIMITIVE_ERR_INCOMPATIBLE;
        catalog->status = status;
        return status;
    }
    if (catalog->count == SIZE_MAX ||
        !add_size(spec->name_length, 1u, &string_size) ||
        !add_size(sizeof(*entry), string_size, &total) ||
        (spec->implementation_kind != ST_PRIMITIVE_INTRINSIC &&
         (!add_size(spec->runtime_symbol_length, 1u, &string_size) ||
          !add_size(total, string_size, &total)))) {
        catalog->status = ST_PRIMITIVE_ERR_OVERFLOW;
        return ST_PRIMITIVE_ERR_OVERFLOW;
    }
    entry = catalog->allocator.allocate(catalog->allocator.user, total);
    if (!entry) {
        catalog->status = ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
        return ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
    }
    memset(entry, 0, sizeof(*entry));
    cursor = entry->payload;
    memcpy(cursor, spec->name, spec->name_length);
    cursor[spec->name_length] = '\0';
    entry->primitive.name.data = cursor;
    entry->primitive.name.length = spec->name_length;
    cursor += spec->name_length + 1u;
    entry->primitive.method_arity = spec->method_arity;
    entry->primitive.receiver_policy = spec->receiver_policy;
    entry->primitive.failure_policy = spec->failure_policy;
    entry->primitive.implementation_kind = spec->implementation_kind;
    entry->primitive.intrinsic_id = spec->intrinsic_id;
    entry->hash = primitive_hash((const unsigned char *)spec->name,
                                 spec->name_length);
    if (spec->implementation_kind != ST_PRIMITIVE_INTRINSIC) {
        memcpy(cursor, spec->runtime_symbol, spec->runtime_symbol_length);
        cursor[spec->runtime_symbol_length] = '\0';
        entry->primitive.runtime_symbol.data = cursor;
        entry->primitive.runtime_symbol.length = spec->runtime_symbol_length;
    }

    if (catalog->count == catalog->entry_capacity) {
        if (!next_capacity(catalog->entry_capacity,
                           ST_PRIMITIVE_INITIAL_ENTRY_CAPACITY,
                           sizeof(*new_entries), &new_entry_capacity) ||
            !multiply_size(new_entry_capacity, sizeof(*new_entries),
                           &allocation_size)) goto overflow;
        new_entries = catalog->allocator.allocate(catalog->allocator.user,
                                                   allocation_size);
        if (!new_entries) goto out_of_memory;
        if (catalog->count)
            memcpy(new_entries, catalog->entries,
                   catalog->count * sizeof(*new_entries));
    }
    if (catalog->table_capacity == 0u ||
        (catalog->count + 1u) > catalog->table_capacity * 3u / 4u) {
        if (!next_capacity(catalog->table_capacity,
                           ST_PRIMITIVE_INITIAL_TABLE_CAPACITY,
                           sizeof(*new_slots), &new_table_capacity) ||
            !multiply_size(new_table_capacity, sizeof(*new_slots),
                           &allocation_size)) goto overflow;
        new_slots = catalog->allocator.allocate(catalog->allocator.user,
                                                 allocation_size);
        if (!new_slots) goto out_of_memory;
        memset(new_slots, 0, allocation_size);
        for (index = 0u; index < catalog->count; index++)
            table_insert(new_slots, new_table_capacity,
                         catalog->entries[index]);
    }

    if (new_entries) {
        release(catalog->allocator, catalog->entries);
        catalog->entries = new_entries;
        catalog->entry_capacity = new_entry_capacity;
    }
    if (new_slots) {
        release(catalog->allocator, catalog->slots);
        catalog->slots = new_slots;
        catalog->table_capacity = new_table_capacity;
    }
    catalog->entries[catalog->count] = entry;
    table_insert(catalog->slots, catalog->table_capacity, entry);
    catalog->count++;
    catalog->status = ST_PRIMITIVE_OK;
    if (primitive_out) *primitive_out = &entry->primitive;
    return ST_PRIMITIVE_OK;
overflow:
    release(catalog->allocator, new_entries);
    release(catalog->allocator, new_slots);
    release(catalog->allocator, entry);
    catalog->status = ST_PRIMITIVE_ERR_OVERFLOW;
    return ST_PRIMITIVE_ERR_OVERFLOW;
out_of_memory:
    release(catalog->allocator, new_entries);
    release(catalog->allocator, new_slots);
    release(catalog->allocator, entry);
    catalog->status = ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
    return ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
}

const st_primitive_t *st_primitive_catalog_get(
    const st_primitive_catalog_t *catalog, size_t index)
{
    if (!catalog || !catalog->initialized || index >= catalog->count)
        return NULL;
    return &catalog->entries[index]->primitive;
}

size_t st_primitive_catalog_count(const st_primitive_catalog_t *catalog)
{
    return catalog && catalog->initialized ? catalog->count : 0u;
}

void st_primitive_result_init(st_primitive_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
}

void st_primitive_result_destroy(st_primitive_result_t *result)
{
    if (!result) return;
    if (result->allocator.deallocate) {
        release(result->allocator, result->bindings);
        release(result->allocator, result->diagnostics);
    }
    memset(result, 0, sizeof(*result));
}

static bool count_class_work(const st_ast_node_t *node, size_t *count,
                             size_t depth)
{
    size_t index;
    if (*count == SIZE_MAX) return false;
    (*count)++;
    if (!node || node->kind != ST_AST_CLASS) return true;
    if (depth >= ST_PRIMITIVE_MAX_AST_DEPTH) return true;
    if ((node->as.class_decl.methods.count &&
         !node->as.class_decl.methods.items) ||
        (node->as.class_decl.members.count &&
         !node->as.class_decl.members.items)) return true;
    for (index = 0u; index < node->as.class_decl.methods.count; index++) {
        if (*count == SIZE_MAX) return false;
        (*count)++;
    }
    for (index = 0u; index < node->as.class_decl.members.count; index++)
        if (!count_class_work(node->as.class_decl.members.items[index], count,
                              depth + 1u))
            return false;
    return true;
}

static bool is_primitive_selector(const st_ast_node_t *pragma)
{
    static const char spelling[] = "primitive:";
    return pragma && pragma->kind == ST_AST_MESSAGE &&
        string_equal(pragma->as.message.selector, spelling,
                     sizeof(spelling) - 1u);
}

static bool append_diagnostic(result_builder_t *builder,
                              st_primitive_diagnostic_t diagnostic)
{
    if (builder->value.diagnostic_count >= builder->diagnostic_capacity)
        return false;
    builder->value.diagnostics[builder->value.diagnostic_count++] = diagnostic;
    return true;
}

static bool resolve_method(result_builder_t *builder,
                           const st_ast_node_t *method,
                           size_t unit_index, st_ast_string_t source_name,
                           const st_primitive_catalog_t *catalog)
{
    st_primitive_diagnostic_t diagnostic;
    const st_ast_node_t *primitive_pragma = NULL;
    st_ast_string_t requested = {0};
    size_t index;
    size_t primitive_count = 0u;
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.method = method;
    diagnostic.unit_index = unit_index;
    diagnostic.source_name = source_name;
    if (!method || method->kind != ST_AST_METHOD ||
        !method->as.method.body || method->as.method.body->kind != ST_AST_BLOCK ||
        (method->as.method.pragmas.count && !method->as.method.pragmas.items) ||
        (method->as.method.arguments.count && !method->as.method.arguments.items) ||
        (method->as.method.body->as.block.expressions.count &&
         !method->as.method.body->as.block.expressions.items)) {
        diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_AST;
        return append_diagnostic(builder, diagnostic);
    }
    for (index = 0u; index < method->as.method.pragmas.count; index++) {
        const st_ast_node_t *pragma = method->as.method.pragmas.items[index];
        if (!pragma || pragma->kind != ST_AST_MESSAGE ||
            (pragma->as.message.selector.length != 0u &&
             !pragma->as.message.selector.data) ||
            (pragma->as.message.arguments.count != 0u &&
             !pragma->as.message.arguments.items)) {
            diagnostic.pragma = pragma;
            diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_AST;
            return append_diagnostic(builder, diagnostic);
        }
        if (is_primitive_selector(pragma)) {
            if (primitive_count == SIZE_MAX) return false;
            primitive_count++;
            if (!primitive_pragma) primitive_pragma = pragma;
        }
    }
    if (primitive_count == 0u) return true;
    diagnostic.pragma = primitive_pragma;
    if (primitive_count != 1u) {
        diagnostic.code = ST_PRIMITIVE_DIAG_DUPLICATE_PRAGMA;
        return append_diagnostic(builder, diagnostic);
    }
    if (primitive_pragma->as.message.arguments.count != 1u ||
        !primitive_pragma->as.message.arguments.items ||
        !primitive_pragma->as.message.arguments.items[0] ||
        primitive_pragma->as.message.arguments.items[0]->kind != ST_AST_VARIABLE) {
        diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_PRAGMA;
        return append_diagnostic(builder, diagnostic);
    }
    requested = primitive_pragma->as.message.arguments.items[0]->as.variable.name;
    diagnostic.requested_name = requested;
    if (!identifier_valid(requested.data, requested.length)) {
        diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_PRAGMA;
        return append_diagnostic(builder, diagnostic);
    }
    const st_primitive_t *primitive = st_primitive_catalog_lookup(
        catalog, requested.data, requested.length);
    if (!primitive) {
        diagnostic.code = ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION;
        return append_diagnostic(builder, diagnostic);
    }
    if (method->as.method.arguments.count > UINT32_MAX) {
        diagnostic.code = ST_PRIMITIVE_DIAG_ARITY_MISMATCH;
        diagnostic.expected_arity = primitive->method_arity;
        diagnostic.actual_arity = UINT32_MAX;
        return append_diagnostic(builder, diagnostic);
    }
    diagnostic.expected_arity = primitive->method_arity;
    diagnostic.actual_arity = (uint32_t)method->as.method.arguments.count;
    if (diagnostic.actual_arity != diagnostic.expected_arity) {
        diagnostic.code = ST_PRIMITIVE_DIAG_ARITY_MISMATCH;
        return append_diagnostic(builder, diagnostic);
    }
    if ((primitive->receiver_policy == ST_PRIMITIVE_INSTANCE_ONLY &&
         method->as.method.class_side) ||
        (primitive->receiver_policy == ST_PRIMITIVE_CLASS_ONLY &&
         !method->as.method.class_side)) {
        diagnostic.code = ST_PRIMITIVE_DIAG_RECEIVER_MISMATCH;
        return append_diagnostic(builder, diagnostic);
    }
    if (primitive->failure_policy == ST_PRIMITIVE_FALL_THROUGH &&
        method->as.method.body->as.block.expressions.count == 0u) {
        diagnostic.code = ST_PRIMITIVE_DIAG_MISSING_FALLBACK;
        return append_diagnostic(builder, diagnostic);
    }
    if (builder->value.binding_count >= builder->binding_capacity) return false;
    builder->value.bindings[builder->value.binding_count++] =
        (st_primitive_binding_t){ method, primitive_pragma, primitive,
                                  unit_index, source_name };
    return true;
}

static bool resolve_class(result_builder_t *builder,
                          const st_ast_node_t *node, size_t unit_index,
                          st_ast_string_t source_name,
                          const st_primitive_catalog_t *catalog, size_t depth)
{
    size_t index;
    if (!node || node->kind != ST_AST_CLASS ||
        depth >= ST_PRIMITIVE_MAX_AST_DEPTH ||
        (node->as.class_decl.methods.count &&
         !node->as.class_decl.methods.items) ||
        (node->as.class_decl.members.count &&
         !node->as.class_decl.members.items)) {
        st_primitive_diagnostic_t diagnostic;
        memset(&diagnostic, 0, sizeof(diagnostic));
        diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_AST;
        diagnostic.method = node;
        diagnostic.unit_index = unit_index;
        diagnostic.source_name = source_name;
        return append_diagnostic(builder, diagnostic);
    }
    for (index = 0u; index < node->as.class_decl.methods.count; index++)
        if (!resolve_method(builder, node->as.class_decl.methods.items[index],
                            unit_index, source_name, catalog)) return false;
    for (index = 0u; index < node->as.class_decl.members.count; index++)
        if (!resolve_class(builder, node->as.class_decl.members.items[index],
                           unit_index, source_name, catalog, depth + 1u))
            return false;
    return true;
}

st_primitive_status_t st_primitive_resolve(
    st_primitive_result_t *result, const st_ast_unit_t *const *units,
    size_t unit_count, const st_primitive_catalog_t *catalog,
    const st_primitive_resolve_options_t *options)
{
    st_primitive_allocator_t allocator;
    result_builder_t builder;
    size_t work_count = 0u;
    size_t byte_count;
    size_t unit_index;
    if (!result || !catalog || !catalog->initialized ||
        (unit_count != 0u && !units) ||
        !allocator_normalize(options ? options->allocator
                                    : (st_primitive_allocator_t){0},
                             &allocator)) return ST_PRIMITIVE_ERR_INVALID_ARGUMENT;
    memset(&builder, 0, sizeof(builder));
    builder.value.allocator = allocator;
    builder.value.status = ST_PRIMITIVE_OK;
    builder.value.resolved = true;
    for (unit_index = 0u; unit_index < unit_count; unit_index++) {
        size_t index;
        const st_ast_unit_t *unit = units[unit_index];
        if (!unit || (unit->declarations.count && !unit->declarations.items)) {
            if (work_count == SIZE_MAX) return ST_PRIMITIVE_ERR_OVERFLOW;
            work_count++;
            continue;
        }
        for (index = 0u; index < unit->declarations.count; index++)
            if (!count_class_work(unit->declarations.items[index], &work_count,
                                  0u))
                return ST_PRIMITIVE_ERR_OVERFLOW;
    }
    builder.binding_capacity = work_count;
    builder.diagnostic_capacity = work_count;
    if (work_count != 0u) {
        if (!multiply_size(work_count, sizeof(*builder.value.bindings),
                           &byte_count)) return ST_PRIMITIVE_ERR_OVERFLOW;
        builder.value.bindings = allocator.allocate(allocator.user, byte_count);
        if (!builder.value.bindings) return ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
        if (!multiply_size(work_count, sizeof(*builder.value.diagnostics),
                           &byte_count)) {
            release(allocator, builder.value.bindings);
            return ST_PRIMITIVE_ERR_OVERFLOW;
        }
        builder.value.diagnostics = allocator.allocate(allocator.user,
                                                        byte_count);
        if (!builder.value.diagnostics) {
            release(allocator, builder.value.bindings);
            return ST_PRIMITIVE_ERR_OUT_OF_MEMORY;
        }
    }
    for (unit_index = 0u; unit_index < unit_count; unit_index++) {
        size_t index;
        const st_ast_unit_t *unit = units[unit_index];
        if (!unit || (unit->declarations.count && !unit->declarations.items)) {
            st_primitive_diagnostic_t diagnostic;
            memset(&diagnostic, 0, sizeof(diagnostic));
            diagnostic.code = ST_PRIMITIVE_DIAG_MALFORMED_AST;
            diagnostic.unit_index = unit_index;
            if (!append_diagnostic(&builder, diagnostic)) goto internal_error;
            continue;
        }
        for (index = 0u; index < unit->declarations.count; index++)
            if (!resolve_class(&builder, unit->declarations.items[index],
                               unit_index, unit->source_name, catalog, 0u))
                goto internal_error;
    }
    st_primitive_result_destroy(result);
    *result = builder.value;
    return ST_PRIMITIVE_OK;
internal_error:
    release(allocator, builder.value.bindings);
    release(allocator, builder.value.diagnostics);
    return ST_PRIMITIVE_ERR_OVERFLOW;
}

bool st_primitive_result_succeeded(const st_primitive_result_t *result)
{
    return result && result->resolved && result->status == ST_PRIMITIVE_OK &&
           result->diagnostic_count == 0u;
}

const char *st_primitive_status_string(st_primitive_status_t status)
{
    switch (status) {
    case ST_PRIMITIVE_OK: return "ok";
    case ST_PRIMITIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_PRIMITIVE_ERR_INVALID_NAME: return "invalid primitive name";
    case ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION: return "invalid implementation";
    case ST_PRIMITIVE_ERR_DUPLICATE: return "duplicate primitive";
    case ST_PRIMITIVE_ERR_INCOMPATIBLE: return "incompatible primitive";
    case ST_PRIMITIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_PRIMITIVE_ERR_OVERFLOW: return "size overflow";
    default: return "unknown primitive status";
    }
}

const char *st_primitive_diagnostic_string(
    st_primitive_diagnostic_code_t code)
{
    switch (code) {
    case ST_PRIMITIVE_DIAG_MALFORMED_AST: return "malformed AST";
    case ST_PRIMITIVE_DIAG_MALFORMED_PRAGMA: return "malformed primitive pragma";
    case ST_PRIMITIVE_DIAG_DUPLICATE_PRAGMA: return "duplicate primitive pragma";
    case ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION: return "missing primitive implementation";
    case ST_PRIMITIVE_DIAG_ARITY_MISMATCH: return "primitive arity mismatch";
    case ST_PRIMITIVE_DIAG_RECEIVER_MISMATCH: return "primitive receiver mismatch";
    case ST_PRIMITIVE_DIAG_MISSING_FALLBACK: return "fall-through primitive has no fallback";
    default: return "unknown primitive diagnostic";
    }
}
