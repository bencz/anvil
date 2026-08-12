#ifndef ANVIL_SMALLTALK_PARSER_H
#define ANVIL_SMALLTALK_PARSER_H

#include "st_ast.h"
#include "st_lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    ST_PARSE_OK = 0,
    ST_PARSE_ERR_INVALID_ARGUMENT,
    ST_PARSE_ERR_LEXER,
    ST_PARSE_ERR_AST,
    ST_PARSE_ERR_OUT_OF_MEMORY,
    ST_PARSE_ERR_UNEXPECTED_TOKEN,
    ST_PARSE_ERR_INVALID_NUMBER,
    ST_PARSE_ERR_INVALID_CHARACTER,
    ST_PARSE_ERR_NESTING_LIMIT,
    ST_PARSE_ERR_MISSING_SEPARATOR
} st_parse_status_t;

typedef struct {
    st_parse_status_t status;
    st_token_kind_t expected;
    st_token_kind_t actual;
    st_source_span_t span;
    st_lexer_status_t lexer_status;
    st_ast_status_t ast_status;
} st_parse_error_t;

typedef struct {
    st_lexer_t lexer;
    st_ast_unit_t *unit;
    st_parse_error_t error;
    size_t nesting_depth;
    size_t nesting_limit;
} st_parser_t;

#ifndef ST_PARSER_MAX_NESTING
#define ST_PARSER_MAX_NESTING 256u
#endif
#if ST_PARSER_MAX_NESTING == 0
#error "ST_PARSER_MAX_NESTING must be greater than zero"
#endif

bool st_parser_init_memory(st_parser_t *parser, st_ast_unit_t *unit,
                           const void *source, size_t length);
bool st_parser_init_cstr(st_parser_t *parser, st_ast_unit_t *unit,
                         const char *source);
bool st_parser_init_file(st_parser_t *parser, st_ast_unit_t *unit, FILE *file);

/* Init accepts fresh/destroyed storage.  Reinit is the safe operation for a
 * live parser and releases the old lexer before replacing its input. */
bool st_parser_reinit_memory(st_parser_t *parser, st_ast_unit_t *unit,
                             const void *source, size_t length);
bool st_parser_reinit_cstr(st_parser_t *parser, st_ast_unit_t *unit,
                           const char *source);
bool st_parser_reinit_file(st_parser_t *parser, st_ast_unit_t *unit,
                           FILE *file);

/* May be changed after init and before parsing.  Zero is rejected. */
bool st_parser_set_nesting_limit(st_parser_t *parser, size_t limit);
void st_parser_destroy(st_parser_t *parser);

st_parse_status_t st_parser_status(const st_parser_t *parser);
const st_parse_error_t *st_parser_error(const st_parser_t *parser);
const char *st_parse_status_string(st_parse_status_t status);
bool st_parser_at_end(const st_parser_t *parser);

st_ast_node_t *st_parse_class(st_parser_t *parser);
st_ast_node_t *st_parse_method(st_parser_t *parser);
st_ast_node_t *st_parse_block(st_parser_t *parser);
st_ast_node_t *st_parse_expression(st_parser_t *parser);
st_ast_node_t *st_parse_method_or_block(st_parser_t *parser);

/* Parse class/namespace declarations through EOF into unit->declarations. */
bool st_parse_compilation_unit(st_parser_t *parser);

/* Parse a loadable source file: class declarations and executable top-level
 * blocks/expressions.  All nodes enter unit->forms; classes also enter
 * unit->declarations. */
bool st_parse_source_unit(st_parser_t *parser);

#endif
