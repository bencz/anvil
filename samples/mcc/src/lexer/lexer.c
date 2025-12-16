/*
 * MCC - Micro C Compiler
 * Lexer - Main module and public API (no goto)
 */

#include "lex_internal.h"

/* ============================================================
 * Lexer lifecycle
 * ============================================================ */

mcc_lexer_t *mcc_lexer_create(mcc_context_t *ctx)
{
    mcc_lexer_t *lex = mcc_alloc(ctx, sizeof(mcc_lexer_t));
    if (!lex) return NULL;
    lex->ctx = ctx;
    lex->line = 1;
    lex->column = 1;
    lex->at_bol = true;
    return lex;
}

void mcc_lexer_destroy(mcc_lexer_t *lex)
{
    (void)lex; /* Arena allocated */
}

void mcc_lexer_init_string(mcc_lexer_t *lex, const char *source, const char *filename)
{
    lex->source = source;
    lex->source_len = strlen(source);
    lex->pos = 0;
    lex->filename = filename;
    lex->line = 1;
    lex->column = 1;
    lex->current = source[0];
    lex->at_bol = true;
    lex->has_space = false;
    lex->peek_token = NULL;
}

void mcc_lexer_init_file(mcc_lexer_t *lex, const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        mcc_fatal(lex->ctx, "Cannot open file: %s", filename);
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = mcc_alloc(lex->ctx, size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    
    mcc_lexer_init_string(lex, buf, filename);
}

/* ============================================================
 * Token scanning - main loop without goto
 * ============================================================ */

static mcc_token_t *lex_scan_token(mcc_lexer_t *lex)
{
    /* Loop to handle whitespace and comments without goto */
    while (1) {
        lex->has_space = false;
        lex_skip_whitespace(lex);
        
        /* Handle newlines (for preprocessor) */
        if (lex->current == '\n') {
            mcc_token_t *tok = lex_make_token(lex, TOK_NEWLINE);
            lex_advance(lex);
            return tok;
        }
        
        /* EOF */
        if (lex->current == '\0') {
            return lex_make_token(lex, TOK_EOF);
        }
        
        /* Try to skip comments - if successful, continue loop */
        lex_comment_result_t comment_result = lex_try_skip_comment(lex);
        if (comment_result != LEX_COMMENT_NONE) {
            continue;  /* Comment was skipped, try again */
        }
        
        /* Not whitespace, newline, EOF, or comment - break to process token */
        break;
    }
    
    /* Identifiers and keywords */
    if (isalpha(lex->current) || lex->current == '_') {
        return lex_identifier(lex);
    }
    
    /* Numbers */
    if (isdigit(lex->current)) {
        return lex_number(lex);
    }
    
    /* Character literal */
    if (lex->current == '\'') {
        return lex_char_literal(lex);
    }
    
    /* String literal */
    if (lex->current == '"') {
        return lex_string_literal(lex);
    }
    
    /* Operators and punctuation */
    return lex_operator(lex);
}

mcc_token_t *mcc_lexer_next(mcc_lexer_t *lex)
{
    /* Return peeked token if available */
    if (lex->peek_token) {
        mcc_token_t *tok = lex->peek_token;
        lex->peek_token = NULL;
        return tok;
    }
    
    return lex_scan_token(lex);
}

mcc_token_t *mcc_lexer_peek(mcc_lexer_t *lex)
{
    if (!lex->peek_token) {
        lex->peek_token = mcc_lexer_next(lex);
    }
    return lex->peek_token;
}

/* ============================================================
 * Token matching utilities
 * ============================================================ */

bool mcc_lexer_match(mcc_lexer_t *lex, mcc_token_type_t type)
{
    if (mcc_lexer_peek(lex)->type == type) {
        mcc_lexer_next(lex);
        return true;
    }
    return false;
}

bool mcc_lexer_check(mcc_lexer_t *lex, mcc_token_type_t type)
{
    return mcc_lexer_peek(lex)->type == type;
}

mcc_token_t *mcc_lexer_expect(mcc_lexer_t *lex, mcc_token_type_t type, const char *msg)
{
    mcc_token_t *tok = mcc_lexer_next(lex);
    if (tok->type != type) {
        mcc_error_at(lex->ctx, tok->location, "Expected %s, got '%s'",
                     msg ? msg : mcc_token_type_name(type),
                     mcc_token_to_string(tok));
    }
    return tok;
}

mcc_location_t mcc_lexer_location(mcc_lexer_t *lex)
{
    return (mcc_location_t){lex->filename, lex->line, lex->column};
}
