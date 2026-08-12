#ifndef ANVIL_SMALLTALK_AST_H
#define ANVIL_SMALLTALK_AST_H

#include "st_lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ST_AST_OK = 0,
    ST_AST_ERR_INVALID_ARGUMENT,
    ST_AST_ERR_OUT_OF_MEMORY,
    ST_AST_ERR_OVERFLOW
} st_ast_status_t;

typedef struct st_ast_arena_block st_ast_arena_block_t;

typedef struct {
    st_ast_arena_block_t *first;
    st_ast_arena_block_t *current;
    size_t block_size;
    size_t bytes_reserved;
    st_ast_status_t status;
} st_ast_arena_t;

typedef struct {
    const char *data;
    size_t length;
} st_ast_string_t;

typedef struct st_ast_node st_ast_node_t;

typedef struct {
    st_ast_node_t **items;
    size_t count;
    size_t capacity;
} st_ast_list_t;

typedef enum {
    ST_AST_CLASS,
    ST_AST_METHOD,
    ST_AST_BLOCK,
    ST_AST_EXPRESSION,
    ST_AST_MESSAGE,
    ST_AST_VARIABLE,
    ST_AST_NIL,
    ST_AST_TRUE,
    ST_AST_FALSE,
    ST_AST_INTEGER,
    ST_AST_FLOAT,
    ST_AST_SCALED_DECIMAL,
    ST_AST_SYMBOL,
    ST_AST_STRING,
    ST_AST_CHARACTER,
    ST_AST_LITERAL_ARRAY
} st_ast_kind_t;

typedef struct {
    st_ast_node_t *name;
    st_ast_node_t *super_name;
    st_ast_list_t pragmas;
    st_ast_list_t variables;
    st_ast_list_t methods;
    st_ast_list_t members;
    bool is_extension;
    bool is_namespace;
} st_ast_class_t;

typedef struct {
    st_ast_string_t class_name;
    st_ast_string_t selector;
    st_ast_list_t arguments;
    st_ast_list_t pragmas;
    st_ast_node_t *body;
    bool class_side;
} st_ast_method_t;

typedef struct {
    st_ast_list_t arguments;
    st_ast_list_t temporaries;
    st_ast_list_t expressions;
} st_ast_block_t;

typedef struct {
    bool returns;
    /* Preserves an explicit grouping boundary for cascade receiver selection. */
    bool parenthesized;
    st_ast_list_t assignments;
    st_ast_node_t *receiver;
    st_ast_list_t messages;
} st_ast_expression_t;

typedef struct {
    st_ast_string_t selector;
    st_ast_list_t arguments;
    bool super_send;
    /* True only for the first message following a semicolon.  Messages before
     * this boundary are evaluated as an ordinary chain; this and subsequent
     * cascade arms start again at the expression's original receiver. */
    bool starts_cascade;
} st_ast_message_t;

typedef struct {
    st_ast_string_t spelling;
    unsigned radix;
    bool negative;
} st_ast_integer_t;

typedef struct {
    st_ast_string_t spelling;
    bool negative;
} st_ast_real_t;

typedef struct {
    st_ast_string_t name;
    st_ast_string_t type_name;
    bool has_type;
} st_ast_variable_t;

typedef struct {
    st_ast_list_t elements;
} st_ast_array_t;

struct st_ast_node {
    st_ast_kind_t kind;
    st_source_span_t span;
    union {
        st_ast_class_t class_decl;
        st_ast_method_t method;
        st_ast_block_t block;
        st_ast_expression_t expression;
        st_ast_message_t message;
        st_ast_variable_t variable;
        st_ast_string_t text;       /* string/symbol/pseudo-variable */
        st_ast_integer_t integer;
        st_ast_real_t real;         /* float or scaled decimal exact spelling */
        uint32_t character;
        st_ast_array_t array;
    } as;
};

typedef struct {
    st_ast_arena_t arena;
    st_ast_string_t source_name;
    st_ast_list_t forms;        /* every top-level class, block, or expression */
    st_ast_list_t declarations;
} st_ast_unit_t;

bool st_ast_unit_init(st_ast_unit_t *unit, const char *source_name);
void st_ast_unit_destroy(st_ast_unit_t *unit);
st_ast_status_t st_ast_unit_status(const st_ast_unit_t *unit);
const char *st_ast_status_string(st_ast_status_t status);

void *st_ast_alloc(st_ast_unit_t *unit, size_t size, size_t alignment);
bool st_ast_copy_string(st_ast_unit_t *unit, const void *bytes, size_t length,
                        st_ast_string_t *string_out);
st_ast_node_t *st_ast_new_node(st_ast_unit_t *unit, st_ast_kind_t kind,
                               st_source_span_t span);
bool st_ast_list_append(st_ast_unit_t *unit, st_ast_list_t *list,
                        st_ast_node_t *node);

const char *st_ast_kind_name(st_ast_kind_t kind);

#endif
