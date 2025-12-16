/*
 * MCC - Micro C Compiler
 * Lexer - String and character literal processing
 */

#include "lex_internal.h"

int lex_escape_char(mcc_lexer_t *lex)
{
    lex_advance(lex); /* Skip backslash */
    int c = lex->current;
    lex_advance(lex);
    
    switch (c) {
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'v':  return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '?':  return '?';
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* Octal escape */
            int val = c - '0';
            for (int i = 0; i < 2 && lex->current >= '0' && lex->current <= '7'; i++) {
                val = val * 8 + (lex->current - '0');
                lex_advance(lex);
            }
            return val;
        }
        case 'x': {
            /* Hex escape */
            int val = 0;
            while (isxdigit(lex->current)) {
                int d = lex->current;
                if (d >= '0' && d <= '9') d -= '0';
                else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                else d = d - 'A' + 10;
                val = val * 16 + d;
                lex_advance(lex);
            }
            return val;
        }
        case 'u':
            /* Universal character name \uXXXX (C99) */
            if (lex_has_universal_char(lex)) {
                int val = 0;
                for (int i = 0; i < 4 && isxdigit(lex->current); i++) {
                    int d = lex->current;
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                    else d = d - 'A' + 10;
                    val = val * 16 + d;
                    lex_advance(lex);
                }
                return val;
            } else {
                mcc_warning(lex->ctx, "\\u escape sequence requires C99 or later");
                return 'u';
            }
        case 'U':
            /* Universal character name \UXXXXXXXX (C99) */
            if (lex_has_universal_char(lex)) {
                int val = 0;
                for (int i = 0; i < 8 && isxdigit(lex->current); i++) {
                    int d = lex->current;
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                    else d = d - 'A' + 10;
                    val = val * 16 + d;
                    lex_advance(lex);
                }
                return val;
            } else {
                mcc_warning(lex->ctx, "\\U escape sequence requires C99 or later");
                return 'U';
            }
        default:
            mcc_warning(lex->ctx, "Unknown escape sequence '\\%c'", c);
            return c;
    }
}

mcc_token_t *lex_char_literal(mcc_lexer_t *lex)
{
    int start_col = lex->column;
    size_t start_pos = lex->pos; /* Position of opening quote */
    
    lex_advance(lex); /* Skip opening quote */
    
    int value;
    if (lex->current == '\\') {
        value = lex_escape_char(lex);
    } else {
        value = lex->current;
        lex_advance(lex);
    }
    
    if (lex->current != '\'') {
        mcc_error(lex->ctx, "Unterminated character literal");
    } else {
        lex_advance(lex);
    }
    
    mcc_token_t *tok = lex_make_token(lex, TOK_CHAR_LIT);
    tok->location.column = start_col;
    tok->literal.char_val.value = value;
    
    /* Store raw text with quotes for preprocessor output */
    size_t raw_len = lex->pos - start_pos;
    char *raw = mcc_alloc(lex->ctx, raw_len + 1);
    memcpy(raw, lex->source + start_pos, raw_len);
    raw[raw_len] = '\0';
    tok->raw_text = raw;
    
    return tok;
}

mcc_token_t *lex_string_literal(mcc_lexer_t *lex)
{
    int start_col = lex->column;
    size_t start_pos = lex->pos; /* Position of opening quote */
    
    lex_advance(lex); /* Skip opening quote */
    
    char buf[LEX_MAX_STRING_LEN];
    size_t len = 0;
    
    while (lex->current && lex->current != '"' && lex->current != '\n') {
        int c;
        if (lex->current == '\\') {
            c = lex_escape_char(lex);
        } else {
            c = lex->current;
            lex_advance(lex);
        }
        if (len < LEX_MAX_STRING_LEN - 1) {
            buf[len++] = (char)c;
        }
    }
    buf[len] = '\0';
    
    if (lex->current != '"') {
        mcc_error(lex->ctx, "Unterminated string literal");
    } else {
        lex_advance(lex);
    }
    
    mcc_token_t *tok = lex_make_token(lex, TOK_STRING_LIT);
    tok->location.column = start_col;
    tok->literal.string_val.value = mcc_strdup(lex->ctx, buf);
    tok->literal.string_val.length = len;
    tok->text = tok->literal.string_val.value;
    tok->text_len = len;
    
    /* Store raw text with quotes for preprocessor output */
    size_t raw_len = lex->pos - start_pos;
    char *raw = mcc_alloc(lex->ctx, raw_len + 1);
    memcpy(raw, lex->source + start_pos, raw_len);
    raw[raw_len] = '\0';
    tok->raw_text = raw;
    
    return tok;
}
