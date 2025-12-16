/*
 * MCC - Micro C Compiler
 * Lexer - Comment processing with C standard checks
 */

#include "lex_internal.h"

static void lex_skip_line_comment(mcc_lexer_t *lex)
{
    while (lex->current && lex->current != '\n') {
        lex_advance(lex);
    }
}

static void lex_skip_block_comment(mcc_lexer_t *lex)
{
    lex_advance(lex); /* Skip * after / */
    while (lex->current) {
        if (lex->current == '*' && lex_peek_next(lex) == '/') {
            lex_advance(lex);
            lex_advance(lex);
            return;
        }
        lex_advance(lex);
    }
    mcc_error(lex->ctx, "Unterminated block comment");
}

lex_comment_result_t lex_try_skip_comment(mcc_lexer_t *lex)
{
    if (lex->current != '/') {
        return LEX_COMMENT_NONE;
    }
    
    int next = lex_peek_next(lex);
    
    /* Check for // line comment */
    if (next == '/') {
        if (lex_has_line_comments(lex)) {
            /* C99+ or GNU extension - allowed */
            lex_advance(lex);
            lex_advance(lex);
            lex_skip_line_comment(lex);
            lex->has_space = true;
            return LEX_COMMENT_LINE;
        } else {
            /* C89 strict mode - warn but still skip to avoid errors */
            mcc_warning(lex->ctx, "// comments are not allowed in C89 mode (use -std=c99 or -std=gnu89)");
            lex_advance(lex);
            lex_advance(lex);
            lex_skip_line_comment(lex);
            lex->has_space = true;
            return LEX_COMMENT_LINE;
        }
    }
    
    /* Check for /* block comment */
    if (next == '*') {
        lex_advance(lex);
        lex_skip_block_comment(lex);
        lex->has_space = true;
        return LEX_COMMENT_BLOCK;
    }
    
    return LEX_COMMENT_NONE;
}
