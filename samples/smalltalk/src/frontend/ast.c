#include "st_ast.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define ST_AST_DEFAULT_BLOCK_SIZE (16u * 1024u)

struct st_ast_arena_block {
    st_ast_arena_block_t *next;
    size_t capacity;
    size_t used;
    max_align_t alignment_anchor;
    unsigned char data[];
};

static bool is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool align_up(size_t value, size_t alignment, size_t *result)
{
    size_t mask;
    if (!is_power_of_two(alignment)) {
        return false;
    }
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

static st_ast_arena_block_t *new_block(st_ast_unit_t *unit,
                                        size_t minimum_capacity)
{
    size_t capacity = unit->arena.block_size;
    size_t allocation_size;
    st_ast_arena_block_t *block;

    if (capacity < minimum_capacity) {
        capacity = minimum_capacity;
    }
    if (capacity > SIZE_MAX - sizeof(*block)) {
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return NULL;
    }
    allocation_size = sizeof(*block) + capacity;
    block = malloc(allocation_size);
    if (block == NULL) {
        unit->arena.status = ST_AST_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0u;
    if (unit->arena.bytes_reserved > SIZE_MAX - allocation_size) {
        free(block);
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return NULL;
    }
    unit->arena.bytes_reserved += allocation_size;
    if (unit->arena.current != NULL) {
        unit->arena.current->next = block;
    } else {
        unit->arena.first = block;
    }
    unit->arena.current = block;
    return block;
}

bool st_ast_unit_init(st_ast_unit_t *unit, const char *source_name)
{
    if (unit == NULL || source_name == NULL) {
        if (unit != NULL) {
            memset(unit, 0, sizeof(*unit));
            unit->arena.status = ST_AST_ERR_INVALID_ARGUMENT;
        }
        return false;
    }
    memset(unit, 0, sizeof(*unit));
    unit->arena.block_size = ST_AST_DEFAULT_BLOCK_SIZE;
    unit->arena.status = ST_AST_OK;
    return st_ast_copy_string(unit, source_name, strlen(source_name),
                              &unit->source_name);
}

void st_ast_unit_destroy(st_ast_unit_t *unit)
{
    st_ast_arena_block_t *block;
    if (unit == NULL) {
        return;
    }
    block = unit->arena.first;
    while (block != NULL) {
        st_ast_arena_block_t *next = block->next;
        free(block);
        block = next;
    }
    memset(unit, 0, sizeof(*unit));
}

st_ast_status_t st_ast_unit_status(const st_ast_unit_t *unit)
{
    return unit == NULL ? ST_AST_ERR_INVALID_ARGUMENT : unit->arena.status;
}

const char *st_ast_status_string(st_ast_status_t status)
{
    switch (status) {
    case ST_AST_OK: return "ok";
    case ST_AST_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_AST_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_AST_ERR_OVERFLOW: return "size overflow";
    default: return "invalid AST status";
    }
}

void *st_ast_alloc(st_ast_unit_t *unit, size_t size, size_t alignment)
{
    st_ast_arena_block_t *block;
    size_t offset;
    size_t required;
    void *result;

    if (unit == NULL || unit->arena.status != ST_AST_OK
            || size == 0u || !is_power_of_two(alignment)
            || alignment > alignof(max_align_t)) {
        if (unit != NULL && unit->arena.status == ST_AST_OK) {
            unit->arena.status = ST_AST_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    block = unit->arena.current;
    if (block == NULL || !align_up(block->used, alignment, &offset)
            || offset > block->capacity
            || size > block->capacity - offset) {
        size_t minimum;
        if (!align_up(0u, alignment, &offset) || size > SIZE_MAX - offset) {
            unit->arena.status = ST_AST_ERR_OVERFLOW;
            return NULL;
        }
        minimum = offset + size;
        block = new_block(unit, minimum);
        if (block == NULL) {
            return NULL;
        }
        offset = 0u;
    }

    if (size > SIZE_MAX - offset) {
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return NULL;
    }
    required = offset + size;
    if (required > block->capacity) {
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return NULL;
    }
    result = block->data + offset;
    block->used = required;
    memset(result, 0, size);
    return result;
}

bool st_ast_copy_string(st_ast_unit_t *unit, const void *bytes, size_t length,
                        st_ast_string_t *string_out)
{
    char *copy;
    if (string_out != NULL) {
        memset(string_out, 0, sizeof(*string_out));
    }
    if (unit == NULL || string_out == NULL || (bytes == NULL && length != 0u)) {
        if (unit != NULL && unit->arena.status == ST_AST_OK) {
            unit->arena.status = ST_AST_ERR_INVALID_ARGUMENT;
        }
        return false;
    }
    if (length == SIZE_MAX) {
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return false;
    }
    copy = st_ast_alloc(unit, length + 1u, alignof(char));
    if (copy == NULL) {
        return false;
    }
    if (length != 0u) {
        memcpy(copy, bytes, length);
    }
    copy[length] = '\0';
    string_out->data = copy;
    string_out->length = length;
    return true;
}

st_ast_node_t *st_ast_new_node(st_ast_unit_t *unit, st_ast_kind_t kind,
                               st_source_span_t span)
{
    st_ast_node_t *node;
    if (kind < ST_AST_CLASS || kind > ST_AST_LITERAL_ARRAY) {
        if (unit != NULL && unit->arena.status == ST_AST_OK) {
            unit->arena.status = ST_AST_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    node = st_ast_alloc(unit, sizeof(*node), alignof(st_ast_node_t));
    if (node == NULL) {
        return NULL;
    }
    node->kind = kind;
    node->span = span;
    return node;
}

bool st_ast_list_append(st_ast_unit_t *unit, st_ast_list_t *list,
                        st_ast_node_t *node)
{
    st_ast_node_t **items;
    size_t capacity;
    size_t bytes;

    if (unit == NULL || list == NULL || node == NULL) {
        if (unit != NULL && unit->arena.status == ST_AST_OK) {
            unit->arena.status = ST_AST_ERR_INVALID_ARGUMENT;
        }
        return false;
    }
    if (list->count < list->capacity) {
        list->items[list->count++] = node;
        return true;
    }
    capacity = list->capacity == 0u ? 4u : list->capacity * 2u;
    if (capacity < list->capacity
            || capacity > SIZE_MAX / sizeof(*items)) {
        unit->arena.status = ST_AST_ERR_OVERFLOW;
        return false;
    }
    bytes = capacity * sizeof(*items);
    items = st_ast_alloc(unit, bytes, alignof(st_ast_node_t *));
    if (items == NULL) {
        return false;
    }
    if (list->count != 0u) {
        memcpy(items, list->items, list->count * sizeof(*items));
    }
    list->items = items;
    list->capacity = capacity;
    list->items[list->count++] = node;
    return true;
}

const char *st_ast_kind_name(st_ast_kind_t kind)
{
    switch (kind) {
    case ST_AST_CLASS: return "class";
    case ST_AST_METHOD: return "method";
    case ST_AST_BLOCK: return "block";
    case ST_AST_EXPRESSION: return "expression";
    case ST_AST_MESSAGE: return "message";
    case ST_AST_VARIABLE: return "variable";
    case ST_AST_NIL: return "nil";
    case ST_AST_TRUE: return "true";
    case ST_AST_FALSE: return "false";
    case ST_AST_INTEGER: return "integer";
    case ST_AST_FLOAT: return "float";
    case ST_AST_SCALED_DECIMAL: return "scaled decimal";
    case ST_AST_SYMBOL: return "symbol";
    case ST_AST_STRING: return "string";
    case ST_AST_CHARACTER: return "character";
    case ST_AST_LITERAL_ARRAY: return "literal array";
    default: return "invalid AST node";
    }
}
