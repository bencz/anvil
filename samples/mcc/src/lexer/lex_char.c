/*
 * MCC - Micro C Compiler
 * Lexer - Character processing
 */

#include "lex_internal.h"

int lex_peek(mcc_lexer_t *lex)
{
    if (lex->pos >= lex->source_len) return '\0';
    return lex->source[lex->pos];
}

int lex_peek_next(mcc_lexer_t *lex)
{
    if (lex->pos + 1 >= lex->source_len) return '\0';
    return lex->source[lex->pos + 1];
}

int lex_advance(mcc_lexer_t *lex)
{
    int c = lex->current;
    if (lex->pos < lex->source_len) {
        lex->pos++;
        lex->column++;
        if (c == '\n') {
            lex->line++;
            lex->column = 1;
            lex->at_bol = true;
        }
        lex->current = lex_peek(lex);
    }
    return c;
}

void lex_skip_whitespace(mcc_lexer_t *lex)
{
    while (lex->current == ' ' || lex->current == '\t' ||
           lex->current == '\r' || lex->current == '\f' ||
           lex->current == '\v') {
        lex->has_space = true;
        lex_advance(lex);
    }
}
