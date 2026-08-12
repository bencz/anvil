#include "st_selector.h"

#include <stdlib.h>
#include <string.h>

#define ST_SELECTOR_INITIAL_TABLE_CAPACITY 16u
#define ST_SELECTOR_INITIAL_VECTOR_CAPACITY 8u

struct st_selector_table_entry {
    uint64_t hash;
    st_selector_id_t id;
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

static void release(st_selector_table_t *table, void *pointer)
{
    if (pointer)
        table->allocator.deallocate(table->allocator.user, pointer);
}

static bool is_identifier_begin(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool is_identifier_continue(unsigned char byte)
{
    return is_identifier_begin(byte) || (byte >= '0' && byte <= '9');
}

static bool is_binary_character(unsigned char byte)
{
    switch (byte) {
    case '!': case '%': case '&': case '*': case '+': case ',': case '-':
    case '/': case '<': case '=': case '>': case '?': case '@': case '\\':
    case '|': case '~':
        return true;
    default:
        return false;
    }
}

static bool classify_selector(const unsigned char *bytes, size_t length,
                              st_selector_kind_t *kind_out,
                              uint32_t *arity_out)
{
    size_t index;
    uint32_t arity = 0;
    if (!bytes || length == 0 || !kind_out || !arity_out) return false;

    if (is_identifier_begin(bytes[0])) {
        index = 0;
        while (index < length) {
            if (!is_identifier_begin(bytes[index])) return false;
            do {
                index++;
            } while (index < length && is_identifier_continue(bytes[index]));
            if (index == length) {
                if (arity != 0) return false;
                *kind_out = ST_SELECTOR_UNARY;
                *arity_out = 0;
                return true;
            }
            if (bytes[index] != ':' || arity == UINT32_MAX) return false;
            arity++;
            index++;
            if (index == length) {
                *kind_out = ST_SELECTOR_KEYWORD;
                *arity_out = arity;
                return true;
            }
        }
        return false;
    }

    for (index = 0; index < length; index++) {
        if (!is_binary_character(bytes[index])) return false;
    }
    *kind_out = ST_SELECTOR_BINARY;
    *arity_out = 1;
    return true;
}

static uint64_t selector_hash(const unsigned char *bytes, size_t length,
                              uint64_t seed)
{
    uint64_t hash = UINT64_C(1469598103934665603) ^ seed;
    size_t index;
    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    /* MurmurHash3 finalizer removes the weak low-bit pattern of FNV before
     * those bits are selected by the power-of-two table mask. */
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash;
}

static size_t probe_distance(size_t index, uint64_t hash, size_t mask)
{
    return (index - ((size_t)hash & mask)) & mask;
}

static void insert_entry(st_selector_table_entry_t *entries, size_t capacity,
                         uint64_t hash, st_selector_id_t id)
{
    size_t mask = capacity - 1;
    size_t index = (size_t)hash & mask;
    size_t distance = 0;
    st_selector_table_entry_t incoming = { hash, id };
    for (;;) {
        st_selector_table_entry_t *slot = &entries[index];
        if (slot->id == ST_SELECTOR_INVALID_ID) {
            *slot = incoming;
            return;
        }
        size_t resident_distance = probe_distance(index, slot->hash, mask);
        if (resident_distance < distance) {
            st_selector_table_entry_t displaced = *slot;
            *slot = incoming;
            incoming = displaced;
            distance = resident_distance;
        }
        index = (index + 1) & mask;
        distance++;
    }
}

static bool selector_equals(const st_selector_table_t *table,
                            st_selector_id_t id, const unsigned char *bytes,
                            size_t length, uint64_t hash)
{
    const st_selector_t *selector = st_selector_get(table, id);
    return selector && selector->hash == hash && selector->length == length &&
           memcmp(selector->bytes, bytes, length) == 0;
}

static bool find_selector(const st_selector_table_t *table,
                          const unsigned char *bytes, size_t length,
                          uint64_t hash, st_selector_id_t *id_out)
{
    size_t mask;
    size_t index;
    size_t distance = 0;
    if (!table || table->table_capacity == 0) return false;
    mask = table->table_capacity - 1;
    index = (size_t)hash & mask;
    for (;;) {
        const st_selector_table_entry_t *slot = &table->entries[index];
        if (slot->id == ST_SELECTOR_INVALID_ID) return false;
        if (probe_distance(index, slot->hash, mask) < distance) return false;
        if (slot->hash == hash &&
            selector_equals(table, slot->id, bytes, length, hash)) {
            if (id_out) *id_out = slot->id;
            return true;
        }
        index = (index + 1) & mask;
        distance++;
        if (distance == table->table_capacity) return false;
    }
}

static bool next_capacity(size_t current, size_t initial,
                          size_t item_size, size_t *capacity_out)
{
    size_t capacity = current == 0 ? initial : current;
    if (current != 0) {
        if (current > SIZE_MAX / 2) return false;
        capacity = current * 2;
    }
    if (capacity > SIZE_MAX / item_size) return false;
    *capacity_out = capacity;
    return true;
}

bool st_selector_table_init(st_selector_table_t *table,
                            st_selector_allocator_t allocator,
                            uint64_t hash_seed)
{
    if (!table || table->initialized) return false;
    if ((allocator.allocate == NULL) != (allocator.deallocate == NULL)) {
        table->status = ST_SELECTOR_ERR_INVALID_ARGUMENT;
        return false;
    }
    memset(table, 0, sizeof(*table));
    if (!allocator.allocate) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
    }
    table->allocator = allocator;
    table->hash_seed = hash_seed;
    table->status = ST_SELECTOR_OK;
    table->initialized = true;
    return true;
}

void st_selector_table_destroy(st_selector_table_t *table)
{
    size_t index;
    if (!table) return;
    if (table->initialized && table->allocator.deallocate) {
        for (index = 0; index < table->count; index++)
            table->allocator.deallocate(table->allocator.user,
                                        (void *)table->selectors[index].bytes);
        release(table, table->selectors);
        release(table, table->entries);
    }
    memset(table, 0, sizeof(*table));
}

bool st_selector_table_freeze(st_selector_table_t *table)
{
    if (!table || !table->initialized) return false;
    table->frozen = true;
    table->status = ST_SELECTOR_OK;
    return true;
}

bool st_selector_table_is_frozen(const st_selector_table_t *table)
{
    return table && table->initialized && table->frozen;
}

typedef enum {
    SELECTOR_WALK_METHODS,
    SELECTOR_WALK_MESSAGES
} selector_walk_pass_t;

static st_selector_status_t walk_selector_node(
    st_selector_table_t *table,
    const st_ast_node_t *node,
    selector_walk_pass_t pass);

static st_selector_status_t walk_selector_list(
    st_selector_table_t *table,
    const st_ast_list_t *list,
    selector_walk_pass_t pass)
{
    size_t index;

    if (list == NULL || (list->count != 0u && list->items == NULL)) {
        return ST_SELECTOR_ERR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < list->count; index++) {
        st_selector_status_t status;

        if (list->items[index] == NULL) {
            return ST_SELECTOR_ERR_INVALID_ARGUMENT;
        }
        status = walk_selector_node(table, list->items[index], pass);
        if (status != ST_SELECTOR_OK) {
            return status;
        }
    }
    return ST_SELECTOR_OK;
}

static st_selector_status_t intern_node_selector(
    st_selector_table_t *table,
    st_ast_string_t selector)
{
    st_selector_id_t ignored;

    if (selector.data == NULL || selector.length == 0u) {
        return ST_SELECTOR_ERR_INVALID_ARGUMENT;
    }
    return st_selector_intern(
        table, selector.data, selector.length, &ignored);
}

static st_selector_status_t walk_selector_node(
    st_selector_table_t *table,
    const st_ast_node_t *node,
    selector_walk_pass_t pass)
{
    st_selector_status_t status;

    if (node == NULL) {
        return ST_SELECTOR_ERR_INVALID_ARGUMENT;
    }
    switch (node->kind) {
    case ST_AST_CLASS:
        status = walk_selector_list(
            table, &node->as.class_decl.members, pass);
        if (status != ST_SELECTOR_OK) {
            return status;
        }
        return walk_selector_list(
            table, &node->as.class_decl.methods, pass);

    case ST_AST_METHOD:
        if (pass == SELECTOR_WALK_METHODS) {
            status = intern_node_selector(table, node->as.method.selector);
            if (status != ST_SELECTOR_OK) {
                return status;
            }
        }
        return walk_selector_node(table, node->as.method.body, pass);

    case ST_AST_BLOCK:
        return walk_selector_list(
            table, &node->as.block.expressions, pass);

    case ST_AST_EXPRESSION:
        status = walk_selector_list(
            table, &node->as.expression.assignments, pass);
        if (status != ST_SELECTOR_OK) {
            return status;
        }
        status = walk_selector_node(
            table, node->as.expression.receiver, pass);
        if (status != ST_SELECTOR_OK) {
            return status;
        }
        return walk_selector_list(
            table, &node->as.expression.messages, pass);

    case ST_AST_MESSAGE:
        if (pass == SELECTOR_WALK_MESSAGES) {
            status = intern_node_selector(table, node->as.message.selector);
            if (status != ST_SELECTOR_OK) {
                return status;
            }
        }
        return walk_selector_list(
            table, &node->as.message.arguments, pass);

    case ST_AST_LITERAL_ARRAY:
        return walk_selector_list(table, &node->as.array.elements, pass);

    case ST_AST_VARIABLE:
    case ST_AST_NIL:
    case ST_AST_TRUE:
    case ST_AST_FALSE:
    case ST_AST_INTEGER:
    case ST_AST_FLOAT:
    case ST_AST_SCALED_DECIMAL:
    case ST_AST_SYMBOL:
    case ST_AST_STRING:
    case ST_AST_CHARACTER:
        return ST_SELECTOR_OK;
    }
    return ST_SELECTOR_ERR_INVALID_ARGUMENT;
}

st_selector_status_t st_selector_table_build_for_units(
    st_selector_table_t *table,
    const st_ast_unit_t *const *units,
    size_t unit_count,
    st_selector_allocator_t allocator,
    uint64_t hash_seed)
{
    st_selector_table_t built = {0};
    selector_walk_pass_t pass;
    size_t unit_index;

    if (table == NULL || table->initialized
            || (unit_count != 0u && units == NULL)) {
        return ST_SELECTOR_ERR_INVALID_ARGUMENT;
    }
    if (!st_selector_table_init(&built, allocator, hash_seed)) {
        return st_selector_table_status(&built);
    }
    for (pass = SELECTOR_WALK_METHODS;
         pass <= SELECTOR_WALK_MESSAGES;
         pass = (selector_walk_pass_t)(pass + 1)) {
        for (unit_index = 0u; unit_index < unit_count; unit_index++) {
            st_selector_status_t status;

            if (units[unit_index] == NULL) {
                st_selector_table_destroy(&built);
                return ST_SELECTOR_ERR_INVALID_ARGUMENT;
            }
            status = walk_selector_list(
                &built, &units[unit_index]->forms, pass);
            if (status != ST_SELECTOR_OK) {
                st_selector_table_destroy(&built);
                return status;
            }
        }
    }
    if (!st_selector_table_freeze(&built)) {
        st_selector_status_t status = st_selector_table_status(&built);
        st_selector_table_destroy(&built);
        return status == ST_SELECTOR_OK
            ? ST_SELECTOR_ERR_INVALID_ARGUMENT : status;
    }
    *table = built;
    return ST_SELECTOR_OK;
}

st_selector_status_t st_selector_intern(st_selector_table_t *table,
                                        const void *bytes, size_t length,
                                        st_selector_id_t *id_out)
{
    const unsigned char *source = bytes;
    st_selector_kind_t kind;
    uint32_t arity;
    uint64_t hash;
    size_t new_vector_capacity = 0;
    size_t new_table_capacity = 0;
    st_selector_t *new_selectors = NULL;
    st_selector_table_entry_t *new_entries = NULL;
    char *copy = NULL;
    bool grow_vector;
    bool grow_table;

    if (id_out) *id_out = ST_SELECTOR_INVALID_ID;
    if (!table) return ST_SELECTOR_ERR_INVALID_ARGUMENT;
    if (!table->initialized || !id_out || !bytes || length == 0) {
        table->status = ST_SELECTOR_ERR_INVALID_ARGUMENT;
        return table->status;
    }
    if (table->frozen) {
        table->status = ST_SELECTOR_ERR_FROZEN;
        return table->status;
    }
    if (!classify_selector(source, length, &kind, &arity)) {
        table->status = ST_SELECTOR_ERR_INVALID_SPELLING;
        return table->status;
    }
    hash = selector_hash(source, length, table->hash_seed);
    if (find_selector(table, source, length, hash, id_out)) {
        table->status = ST_SELECTOR_OK;
        return ST_SELECTOR_OK;
    }
    if (table->count >= UINT32_MAX) {
        table->status = ST_SELECTOR_ERR_ID_EXHAUSTED;
        return table->status;
    }
    if (length == SIZE_MAX) {
        table->status = ST_SELECTOR_ERR_OVERFLOW;
        return table->status;
    }

    grow_vector = table->count == table->selector_capacity;
    grow_table = table->table_capacity == 0 ||
        table->count + 1 > table->table_capacity - table->table_capacity / 4;
    if (grow_vector && !next_capacity(table->selector_capacity,
                                      ST_SELECTOR_INITIAL_VECTOR_CAPACITY,
                                      sizeof(*new_selectors),
                                      &new_vector_capacity)) {
        table->status = ST_SELECTOR_ERR_OVERFLOW;
        return table->status;
    }
    if (grow_table && !next_capacity(table->table_capacity,
                                     ST_SELECTOR_INITIAL_TABLE_CAPACITY,
                                     sizeof(*new_entries),
                                     &new_table_capacity)) {
        table->status = ST_SELECTOR_ERR_OVERFLOW;
        return table->status;
    }

    copy = table->allocator.allocate(table->allocator.user, length + 1);
    if (!copy) goto out_of_memory;
    memcpy(copy, source, length);
    copy[length] = '\0';

    if (grow_vector) {
        new_selectors = table->allocator.allocate(
            table->allocator.user,
            new_vector_capacity * sizeof(*new_selectors));
        if (!new_selectors) goto out_of_memory;
        if (table->count != 0)
            memcpy(new_selectors, table->selectors,
                   table->count * sizeof(*new_selectors));
    }
    if (grow_table) {
        size_t index;
        new_entries = table->allocator.allocate(
            table->allocator.user,
            new_table_capacity * sizeof(*new_entries));
        if (!new_entries) goto out_of_memory;
        memset(new_entries, 0, new_table_capacity * sizeof(*new_entries));
        for (index = 0; index < table->count; index++) {
            insert_entry(new_entries, new_table_capacity,
                         table->selectors[index].hash,
                         (st_selector_id_t)(index + 1));
        }
    }

    if (grow_vector) {
        release(table, table->selectors);
        table->selectors = new_selectors;
        table->selector_capacity = new_vector_capacity;
        new_selectors = NULL;
    }
    if (grow_table) {
        release(table, table->entries);
        table->entries = new_entries;
        table->table_capacity = new_table_capacity;
        new_entries = NULL;
    }

    table->selectors[table->count] = (st_selector_t) {
        .bytes = copy,
        .length = length,
        .hash = hash,
        .arity = arity,
        .kind = kind
    };
    table->count++;
    *id_out = (st_selector_id_t)table->count;
    insert_entry(table->entries, table->table_capacity, hash, *id_out);
    table->status = ST_SELECTOR_OK;
    return ST_SELECTOR_OK;

out_of_memory:
    release(table, copy);
    release(table, new_selectors);
    release(table, new_entries);
    table->status = ST_SELECTOR_ERR_OUT_OF_MEMORY;
    return table->status;
}

bool st_selector_lookup(const st_selector_table_t *table,
                        const void *bytes, size_t length,
                        st_selector_id_t *id_out)
{
    st_selector_kind_t kind;
    uint32_t arity;
    if (id_out) *id_out = ST_SELECTOR_INVALID_ID;
    if (!table || !table->initialized || !id_out ||
        !classify_selector(bytes, length, &kind, &arity)) return false;
    (void)kind;
    (void)arity;
    return find_selector(table, bytes, length,
                         selector_hash(bytes, length, table->hash_seed),
                         id_out);
}

const st_selector_t *st_selector_get(const st_selector_table_t *table,
                                     st_selector_id_t id)
{
    if (!table || !table->initialized || id == ST_SELECTOR_INVALID_ID ||
        (size_t)id > table->count) return NULL;
    return &table->selectors[id - 1u];
}

size_t st_selector_count(const st_selector_table_t *table)
{
    return table && table->initialized ? table->count : 0;
}

st_selector_status_t st_selector_table_status(const st_selector_table_t *table)
{
    return table ? table->status : ST_SELECTOR_ERR_INVALID_ARGUMENT;
}

const char *st_selector_status_string(st_selector_status_t status)
{
    switch (status) {
    case ST_SELECTOR_OK: return "ok";
    case ST_SELECTOR_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_SELECTOR_ERR_INVALID_SPELLING: return "invalid selector spelling";
    case ST_SELECTOR_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_SELECTOR_ERR_OVERFLOW: return "size overflow";
    case ST_SELECTOR_ERR_ID_EXHAUSTED: return "selector id space exhausted";
    case ST_SELECTOR_ERR_FROZEN: return "selector table is frozen";
    default: return "invalid selector status";
    }
}
