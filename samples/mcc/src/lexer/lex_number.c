/*
 * MCC - Micro C Compiler
 * Lexer - Number literal processing with C standard checks
 */

#include "lex_internal.h"

mcc_token_t *lex_number(mcc_lexer_t *lex)
{
    size_t start = lex->pos;
    int start_col = lex->column;
    bool is_float = false;
    bool is_hex_float = false;
    int base = 10;
    
    /* Check for hex/octal/binary prefix */
    if (lex->current == '0') {
        lex_advance(lex);
        if (lex->current == 'x' || lex->current == 'X') {
            base = 16;
            lex_advance(lex);
        } else if (lex->current == 'b' || lex->current == 'B') {
            /* Binary literals (C23 or GNU extension) */
            if (lex_has_binary_literals(lex)) {
                base = 2;
                lex_advance(lex);
            } else {
                mcc_warning(lex->ctx, "Binary literals are a C23 feature");
                base = 2;
                lex_advance(lex);
            }
        } else if (isdigit(lex->current)) {
            base = 8;
        }
    }
    
    /* Read digits (with optional C23 digit separators) */
    while (1) {
        /* C23: Digit separator */
        if (lex->current == '\'') {
            int next = lex_peek_next(lex);
            if ((base == 16 && isxdigit(next)) ||
                (base == 2 && (next == '0' || next == '1')) ||
                (base == 10 && isdigit(next)) ||
                (base == 8 && next >= '0' && next <= '7')) {
                lex_advance(lex);  /* Skip the separator */
                continue;
            }
            break;
        }
        if (base == 16 && isxdigit(lex->current)) {
            lex_advance(lex);
        } else if (base == 2 && (lex->current == '0' || lex->current == '1')) {
            lex_advance(lex);
        } else if (isdigit(lex->current)) {
            lex_advance(lex);
        } else {
            break;
        }
    }
    
    /* Check for decimal point */
    if (lex->current == '.') {
        if (base == 10) {
            is_float = true;
            lex_advance(lex);
            /* Read fractional digits with optional digit separators */
            while (1) {
                if (lex->current == '\'' && isdigit(lex_peek_next(lex))) {
                    lex_advance(lex);  /* Skip separator */
                    continue;
                }
                if (isdigit(lex->current)) {
                    lex_advance(lex);
                } else {
                    break;
                }
            }
        } else if (base == 16) {
            /* Hex float (C99) */
            if (lex_has_hex_floats(lex)) {
                is_float = true;
                is_hex_float = true;
                lex_advance(lex);
                while (isxdigit(lex->current)) {
                    lex_advance(lex);
                }
            } else {
                mcc_warning(lex->ctx, "Hexadecimal floating constants require C99 or later");
                is_float = true;
                is_hex_float = true;
                lex_advance(lex);
                while (isxdigit(lex->current)) {
                    lex_advance(lex);
                }
            }
        }
    }
    
    /* Check for exponent */
    if (base == 10 && (lex->current == 'e' || lex->current == 'E')) {
        is_float = true;
        lex_advance(lex);
        if (lex->current == '+' || lex->current == '-') {
            lex_advance(lex);
        }
        while (isdigit(lex->current)) {
            lex_advance(lex);
        }
    } else if (is_hex_float && (lex->current == 'p' || lex->current == 'P')) {
        /* Hex float exponent (required for hex floats) */
        lex_advance(lex);
        if (lex->current == '+' || lex->current == '-') {
            lex_advance(lex);
        }
        while (isdigit(lex->current)) {
            lex_advance(lex);
        }
    }
    
    /* Read suffix */
    mcc_int_suffix_t int_suffix = INT_SUFFIX_NONE;
    mcc_float_suffix_t float_suffix = FLOAT_SUFFIX_NONE;
    
    if (is_float) {
        if (lex->current == 'f' || lex->current == 'F') {
            float_suffix = FLOAT_SUFFIX_F;
            lex_advance(lex);
        } else if (lex->current == 'l' || lex->current == 'L') {
            float_suffix = FLOAT_SUFFIX_L;
            lex_advance(lex);
        }
    } else {
        bool has_u = false, has_l = false, has_ll = false;
        while (1) {
            if ((lex->current == 'u' || lex->current == 'U') && !has_u) {
                has_u = true;
                lex_advance(lex);
            } else if ((lex->current == 'l' || lex->current == 'L') && !has_ll) {
                if (has_l) {
                    /* Check for long long (C99) */
                    if (!lex_has_long_long(lex)) {
                        mcc_warning(lex->ctx, "long long is a C99 feature");
                    }
                    has_ll = true;
                    has_l = false;
                } else {
                    has_l = true;
                }
                lex_advance(lex);
            } else {
                break;
            }
        }
        if (has_u && has_ll) int_suffix = INT_SUFFIX_ULL;
        else if (has_ll) int_suffix = INT_SUFFIX_LL;
        else if (has_u && has_l) int_suffix = INT_SUFFIX_UL;
        else if (has_l) int_suffix = INT_SUFFIX_L;
        else if (has_u) int_suffix = INT_SUFFIX_U;
    }
    
    size_t len = lex->pos - start;
    char *text = mcc_alloc(lex->ctx, len + 1);
    memcpy(text, lex->source + start, len);
    text[len] = '\0';
    
    mcc_token_t *tok = lex_make_token(lex, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT);
    tok->location.column = start_col;
    tok->text = text;
    tok->text_len = len;
    
    if (is_float) {
        tok->literal.float_val.value = strtod(text, NULL);
        tok->literal.float_val.suffix = float_suffix;
    } else {
        tok->literal.int_val.value = strtoull(text, NULL, base);
        tok->literal.int_val.suffix = int_suffix;
    }
    
    return tok;
}
