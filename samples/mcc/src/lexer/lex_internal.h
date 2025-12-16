/*
 * MCC - Micro C Compiler
 * Lexer internal header
 */

#ifndef MCC_LEX_INTERNAL_H
#define MCC_LEX_INTERNAL_H

#include "mcc.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define LEX_MAX_STRING_LEN MCC_MAX_STRING_LEN

/* ============================================================
 * Keyword table entry
 * ============================================================ */

typedef struct {
    const char *name;
    mcc_token_type_t type;
    mcc_feature_id_t required_feature;  /* MCC_FEAT_COUNT means always available */
} lex_keyword_entry_t;

/* ============================================================
 * Character processing (lex_char.c)
 * ============================================================ */

int lex_peek(mcc_lexer_t *lex);
int lex_peek_next(mcc_lexer_t *lex);
int lex_advance(mcc_lexer_t *lex);
void lex_skip_whitespace(mcc_lexer_t *lex);

/* ============================================================
 * Token creation (lex_token.c)
 * ============================================================ */

mcc_token_t *lex_make_token(mcc_lexer_t *lex, mcc_token_type_t type);

/* ============================================================
 * Comment processing (lex_comment.c)
 * ============================================================ */

typedef enum {
    LEX_COMMENT_NONE,       /* No comment found */
    LEX_COMMENT_LINE,       /* // comment processed */
    LEX_COMMENT_BLOCK       /* /* comment processed */
} lex_comment_result_t;

lex_comment_result_t lex_try_skip_comment(mcc_lexer_t *lex);

/* ============================================================
 * String and character literals (lex_string.c)
 * ============================================================ */

int lex_escape_char(mcc_lexer_t *lex);
mcc_token_t *lex_char_literal(mcc_lexer_t *lex);
mcc_token_t *lex_string_literal(mcc_lexer_t *lex);

/* ============================================================
 * Number literals (lex_number.c)
 * ============================================================ */

mcc_token_t *lex_number(mcc_lexer_t *lex);

/* ============================================================
 * Identifiers and keywords (lex_ident.c)
 * ============================================================ */

mcc_token_type_t lex_lookup_keyword(mcc_lexer_t *lex, const char *name, size_t len);
mcc_token_t *lex_identifier(mcc_lexer_t *lex);

/* ============================================================
 * Operators and punctuation (lex_operator.c)
 * ============================================================ */

mcc_token_t *lex_operator(mcc_lexer_t *lex);

/* ============================================================
 * C Standard feature helpers
 * ============================================================ */

static inline bool lex_has_feature(mcc_lexer_t *lex, mcc_feature_id_t feat)
{
    return mcc_ctx_has_feature(lex->ctx, feat);
}

static inline bool lex_has_line_comments(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_LINE_COMMENT);
}

static inline bool lex_has_long_long(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_LONG_LONG);
}

static inline bool lex_has_hex_floats(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_HEX_FLOAT);
}

static inline bool lex_has_universal_char(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_UNIVERSAL_CHAR);
}

static inline bool lex_has_digraphs(mcc_lexer_t *lex)
{
    /* Digraphs are available in C95 (Amendment 1) and later, and GNU modes */
    /* For now, enable if C99+ or GNU mode (check via line comments as proxy) */
    return lex_has_feature(lex, MCC_FEAT_LINE_COMMENT);
}

static inline bool lex_has_binary_literals(mcc_lexer_t *lex)
{
    return lex_has_feature(lex, MCC_FEAT_BINARY_LIT);
}

#endif /* MCC_LEX_INTERNAL_H */
