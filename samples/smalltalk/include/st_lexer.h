#ifndef ANVIL_SMALLTALK_LEXER_H
#define ANVIL_SMALLTALK_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The lexer is deliberately independent of the Smalltalk object model.  Its
 * output can be consumed before a heap, class table, or GC exists.
 *
 * Token kinds are bit flags because grammar productions frequently accept a
 * set of alternatives.  ST_TOKEN_NONE is zero and is never emitted.
 */
typedef uint32_t st_token_kind_t;

enum {
    ST_TOKEN_NONE                  = 0,
    ST_TOKEN_UNKNOWN               = UINT32_C(1) << 0,
    ST_TOKEN_NUMBER                = UINT32_C(1) << 1,
    ST_TOKEN_IDENTIFIER            = UINT32_C(1) << 2,
    ST_TOKEN_SYMBOL_PREFIX         = UINT32_C(1) << 3,
    ST_TOKEN_CHARACTER             = UINT32_C(1) << 4,
    ST_TOKEN_STRING                = UINT32_C(1) << 5,
    ST_TOKEN_BINARY_SELECTOR       = UINT32_C(1) << 6,
    ST_TOKEN_KEYWORD               = UINT32_C(1) << 7,
    ST_TOKEN_ASSIGN                = UINT32_C(1) << 8,
    ST_TOKEN_RETURN                = UINT32_C(1) << 9,
    ST_TOKEN_PERIOD                = UINT32_C(1) << 10,
    ST_TOKEN_LITERAL_ARRAY_BEGIN   = UINT32_C(1) << 11,
    ST_TOKEN_LEFT_PAREN            = UINT32_C(1) << 12,
    ST_TOKEN_RIGHT_PAREN           = UINT32_C(1) << 13,
    ST_TOKEN_LEFT_BRACKET          = UINT32_C(1) << 14,
    ST_TOKEN_RIGHT_BRACKET         = UINT32_C(1) << 15,
    ST_TOKEN_EOF                   = UINT32_C(1) << 16,
    ST_TOKEN_COLON                 = UINT32_C(1) << 17,
    ST_TOKEN_SEMICOLON             = UINT32_C(1) << 18,
    ST_TOKEN_VERTICAL_BAR          = UINT32_C(1) << 19,
    ST_TOKEN_LESS_THAN             = UINT32_C(1) << 20,
    ST_TOKEN_GREATER_THAN          = UINT32_C(1) << 21,
    ST_TOKEN_MINUS                 = UINT32_C(1) << 22,
    ST_TOKEN_UNTERMINATED_STRING   = UINT32_C(1) << 23,
    ST_TOKEN_UNTERMINATED_COMMENT  = UINT32_C(1) << 24,
    ST_TOKEN_UNTERMINATED_CHARACTER = UINT32_C(1) << 25
};

typedef struct {
    size_t offset; /* zero-based byte offset */
    size_t line;   /* one-based line */
    size_t column; /* one-based byte column */
} st_source_position_t;

typedef struct {
    st_source_position_t begin;
    st_source_position_t end; /* exclusive */
} st_source_span_t;

#define ST_TOKEN_INLINE_CAPACITY 64u

typedef struct {
    st_token_kind_t kind;
    st_source_span_t span;
    bool separated;
    size_t length;
    size_t capacity;
    char *text;
    char inline_text[ST_TOKEN_INLINE_CAPACITY];
} st_token_t;

typedef enum {
    ST_LEXER_OK = 0,
    ST_LEXER_ERR_INVALID_ARGUMENT,
    ST_LEXER_ERR_OUT_OF_MEMORY,
    ST_LEXER_ERR_IO,
    ST_LEXER_ERR_LOOKAHEAD
} st_lexer_status_t;

/* Six tokens matches the deepest lookahead required by the reference parser. */
#define ST_LEXER_LOOKAHEAD 6u
#define ST_LEXER_TOKEN_SLOTS (ST_LEXER_LOOKAHEAD + 2u)

typedef struct {
    const unsigned char *source;
    size_t source_length;
    size_t offset;
    size_t line;
    size_t column;
    unsigned char *owned_source;

    /* current + previous + the full forward lookahead window */
    st_token_t tokens[ST_LEXER_TOKEN_SLOTS];
    size_t token_serial[ST_LEXER_TOKEN_SLOTS];
    size_t cursor;
    size_t produced;
    st_lexer_status_t status;
} st_lexer_t;

/* The memory remains borrowed and must outlive the lexer. */
bool st_lexer_init_memory(st_lexer_t *lexer, const void *source, size_t length);
bool st_lexer_init_cstr(st_lexer_t *lexer, const char *source);

/* Reads the remaining stream into owned storage; non-seekable streams work. */
bool st_lexer_init_file(st_lexer_t *lexer, FILE *file);

/* Reinitialization is deliberately explicit: the init functions accept fresh
 * or destroyed storage, while these functions first release a live lexer's
 * owned source and expanded token buffers. */
bool st_lexer_reinit_memory(st_lexer_t *lexer, const void *source,
                            size_t length);
bool st_lexer_reinit_cstr(st_lexer_t *lexer, const char *source);
bool st_lexer_reinit_file(st_lexer_t *lexer, FILE *file);

void st_lexer_destroy(st_lexer_t *lexer);
st_lexer_status_t st_lexer_status(const st_lexer_t *lexer);
const char *st_lexer_status_string(st_lexer_status_t status);

const st_token_t *st_lexer_current(const st_lexer_t *lexer);
bool st_lexer_advance(st_lexer_t *lexer, const st_token_t **token_out);
bool st_lexer_peek(st_lexer_t *lexer, size_t distance,
                   const st_token_t **token_out);
const st_token_t *st_lexer_previous(const st_lexer_t *lexer);

bool st_token_is(const st_token_t *token, st_token_kind_t accepted_kinds);
const char *st_token_kind_name(st_token_kind_t kind);

#endif
