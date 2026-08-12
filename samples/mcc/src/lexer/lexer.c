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
    if (!lex || !source) return;
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
    
    if (fseek(f, 0, SEEK_END) != 0) {
        mcc_fatal(lex->ctx, "Cannot seek file: %s", filename);
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0 || (uintmax_t)size >= (uintmax_t)SIZE_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        mcc_fatal(lex->ctx, "Invalid file size: %s", filename);
        fclose(f);
        return;
    }
    
    size_t content_size = (size_t)size;
    char *buf = mcc_alloc(lex->ctx, content_size + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, content_size, f) != content_size) {
        mcc_fatal(lex->ctx, "Cannot read file: %s", filename);
        fclose(f);
        return;
    }
    buf[content_size] = '\0';
    fclose(f);
    
    mcc_lexer_init_string(lex, buf, filename);
}

/* ============================================================
 * Token scanning - main loop without goto
 * ============================================================ */

/* Skip line continuation (backslash-newline) sequences */
static void lex_skip_line_continuation(mcc_lexer_t *lex)
{
    while (lex->current == '\\') {
        /* Check if next char is newline */
        if (lex->pos + 1 < lex->source_len) {
            char next = lex->source[lex->pos + 1];
            if (next == '\n') {
                /* Skip backslash */
                lex->pos++;
                /* Skip newline */
                lex->pos++;
                lex->line++;
                lex->column = 1;
                /* Don't set at_bol - we're continuing a logical line */
                lex->current = lex_peek(lex);
            } else if (next == '\r' && lex->pos + 2 < lex->source_len &&
                       lex->source[lex->pos + 2] == '\n') {
                /* Handle \r\n (Windows line endings) */
                lex->pos += 3;
                lex->line++;
                lex->column = 1;
                lex->current = lex_peek(lex);
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

static mcc_token_t *lex_scan_token(mcc_lexer_t *lex)
{
    /* Loop to handle whitespace and comments without goto */
    while (1) {
        lex->has_space = false;
        
        /* Handle line continuation at start of scan */
        lex_skip_line_continuation(lex);
        
        lex_skip_whitespace(lex);
        
        /* Handle line continuation after whitespace */
        lex_skip_line_continuation(lex);
        
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
