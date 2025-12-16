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
        
        /* Handle line continuation: backslash followed by newline */
        while (lex->current == '\\' && lex->pos + 1 < lex->source_len) {
            char next = lex->source[lex->pos + 1];
            if (next == '\n') {
                /* Skip backslash */
                lex->pos++;
                lex->column++;
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
        }
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
