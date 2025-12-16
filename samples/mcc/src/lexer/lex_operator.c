/*
 * MCC - Micro C Compiler
 * Lexer - Operator and punctuation processing
 */

#include "lex_internal.h"

mcc_token_t *lex_operator(mcc_lexer_t *lex)
{
    int c = lex->current;
    int start_col = lex->column;
    lex_advance(lex);
    
    mcc_token_t *tok;
    
    switch (c) {
        case '+':
            if (lex->current == '+') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_INC);
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_PLUS_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_PLUS);
            }
            break;
            
        case '-':
            if (lex->current == '-') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_DEC);
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_MINUS_ASSIGN);
            } else if (lex->current == '>') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_ARROW);
            } else {
                tok = lex_make_token(lex, TOK_MINUS);
            }
            break;
            
        case '*':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_STAR_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_STAR);
            }
            break;
            
        case '/':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_SLASH_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_SLASH);
            }
            break;
            
        case '%':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_PERCENT_ASSIGN);
            } else if (lex->current == ':') {
                /* Digraph %: for # (C95/C99) */
                if (lex_has_digraphs(lex)) {
                    lex_advance(lex);
                    if (lex->current == '%' && lex_peek_next(lex) == ':') {
                        /* %:%: is ## */
                        lex_advance(lex);
                        lex_advance(lex);
                        tok = lex_make_token(lex, TOK_HASH_HASH);
                    } else {
                        tok = lex_make_token(lex, TOK_HASH);
                    }
                } else {
                    tok = lex_make_token(lex, TOK_PERCENT);
                }
            } else if (lex->current == '>') {
                /* Digraph %> for } (C95/C99) */
                if (lex_has_digraphs(lex)) {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_RBRACE);
                } else {
                    tok = lex_make_token(lex, TOK_PERCENT);
                }
            } else {
                tok = lex_make_token(lex, TOK_PERCENT);
            }
            break;
            
        case '=':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_EQ);
            } else {
                tok = lex_make_token(lex, TOK_ASSIGN);
            }
            break;
            
        case '!':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_NE);
            } else {
                tok = lex_make_token(lex, TOK_NOT);
            }
            break;
            
        case '<':
            if (lex->current == '<') {
                lex_advance(lex);
                if (lex->current == '=') {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_LSHIFT_ASSIGN);
                } else {
                    tok = lex_make_token(lex, TOK_LSHIFT);
                }
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_LE);
            } else if (lex->current == ':') {
                /* Digraph <: for [ (C95/C99) */
                if (lex_has_digraphs(lex)) {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_LBRACKET);
                } else {
                    tok = lex_make_token(lex, TOK_LT);
                }
            } else if (lex->current == '%') {
                /* Digraph <% for { (C95/C99) */
                if (lex_has_digraphs(lex)) {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_LBRACE);
                } else {
                    tok = lex_make_token(lex, TOK_LT);
                }
            } else {
                tok = lex_make_token(lex, TOK_LT);
            }
            break;
            
        case '>':
            if (lex->current == '>') {
                lex_advance(lex);
                if (lex->current == '=') {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_RSHIFT_ASSIGN);
                } else {
                    tok = lex_make_token(lex, TOK_RSHIFT);
                }
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_GE);
            } else {
                tok = lex_make_token(lex, TOK_GT);
            }
            break;
            
        case '&':
            if (lex->current == '&') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_AND);
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_AMP_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_AMP);
            }
            break;
            
        case '|':
            if (lex->current == '|') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_OR);
            } else if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_PIPE_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_PIPE);
            }
            break;
            
        case '^':
            if (lex->current == '=') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_CARET_ASSIGN);
            } else {
                tok = lex_make_token(lex, TOK_CARET);
            }
            break;
            
        case '~':
            tok = lex_make_token(lex, TOK_TILDE);
            break;
            
        case '?':
            tok = lex_make_token(lex, TOK_QUESTION);
            break;
            
        case ':':
            if (lex->current == '>') {
                /* Digraph :> for ] (C95/C99) */
                if (lex_has_digraphs(lex)) {
                    lex_advance(lex);
                    tok = lex_make_token(lex, TOK_RBRACKET);
                } else {
                    tok = lex_make_token(lex, TOK_COLON);
                }
            } else {
                tok = lex_make_token(lex, TOK_COLON);
            }
            break;
            
        case ';':
            tok = lex_make_token(lex, TOK_SEMICOLON);
            break;
            
        case ',':
            tok = lex_make_token(lex, TOK_COMMA);
            break;
            
        case '(':
            tok = lex_make_token(lex, TOK_LPAREN);
            break;
            
        case ')':
            tok = lex_make_token(lex, TOK_RPAREN);
            break;
            
        case '[':
            tok = lex_make_token(lex, TOK_LBRACKET);
            break;
            
        case ']':
            tok = lex_make_token(lex, TOK_RBRACKET);
            break;
            
        case '{':
            tok = lex_make_token(lex, TOK_LBRACE);
            break;
            
        case '}':
            tok = lex_make_token(lex, TOK_RBRACE);
            break;
            
        case '#':
            if (lex->current == '#') {
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_HASH_HASH);
            } else {
                tok = lex_make_token(lex, TOK_HASH);
            }
            break;
            
        case '.':
            if (lex->current == '.' && lex_peek_next(lex) == '.') {
                lex_advance(lex);
                lex_advance(lex);
                tok = lex_make_token(lex, TOK_ELLIPSIS);
            } else if (isdigit(lex->current)) {
                /* Float starting with . - back up and call lex_number */
                lex->pos--;
                lex->column--;
                lex->current = '.';
                return lex_number(lex);
            } else {
                tok = lex_make_token(lex, TOK_DOT);
            }
            break;
            
        default:
            mcc_error(lex->ctx, "Unexpected character '%c' (0x%02x)", c, c);
            tok = lex_make_token(lex, TOK_EOF);
            break;
    }
    
    tok->location.column = start_col;
    return tok;
}
