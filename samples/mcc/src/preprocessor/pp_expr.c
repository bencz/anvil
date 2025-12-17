/*
 * MCC - Micro C Compiler
 * Preprocessor - Expression Evaluation
 * 
 * This file handles evaluation of preprocessor constant expressions
 * used in #if and #elif directives.
 */

#include "pp_internal.h"

/* Forward declarations for recursive descent parser */
static int64_t pp_eval_ternary(mcc_preprocessor_t *pp);

/* ============================================================
 * Primary Expressions
 * ============================================================ */

static int64_t pp_eval_primary(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    
    if (tok->type == TOK_INT_LIT) {
        return (int64_t)tok->literal.int_val.value;
    }
    
    if (tok->type == TOK_CHAR_LIT) {
        return tok->literal.char_val.value;
    }
    
    if (tok->type == TOK_IDENT) {
        if (strcmp(tok->text, "defined") == 0) {
            /* defined(name) or defined name */
            bool has_paren = false;
            tok = mcc_lexer_peek(pp->lexer);
            if (tok->type == TOK_LPAREN) {
                mcc_lexer_next(pp->lexer);
                has_paren = true;
            }
            tok = mcc_lexer_next(pp->lexer);
            if (tok->type != TOK_IDENT) {
                mcc_error(pp->ctx, "Expected identifier after 'defined'");
                return 0;
            }
            int result = mcc_preprocessor_is_defined(pp, tok->text) ? 1 : 0;
            if (has_paren) {
                mcc_lexer_expect(pp->lexer, TOK_RPAREN, ")");
            }
            return result;
        }
        
        /* Check for true/false in C23 or GNU mode */
        if (pp_has_feature(pp, MCC_FEAT_TRUE_FALSE)) {
            if (strcmp(tok->text, "true") == 0) return 1;
            if (strcmp(tok->text, "false") == 0) return 0;
        }
        
        /* Check if it's a macro and expand it */
        mcc_macro_t *macro = pp_lookup_macro(pp, tok->text);
        if (macro && !macro->is_function_like) {
            /* Object-like macro - get its value */
            if (macro->body && macro->body->type == TOK_INT_LIT) {
                return (int64_t)macro->body->literal.int_val.value;
            }
            /* Macro expands to identifier or complex expression - evaluate recursively */
            if (macro->body) {
                /* Save current output */
                mcc_token_t *saved_head = pp->output_head;
                mcc_token_t *saved_tail = pp->output_tail;
                pp->output_head = pp->output_tail = NULL;
                
                /* Expand macro */
                pp_expand_macro(pp, macro);
                
                /* If expansion produced a single integer, return it */
                if (pp->output_head && pp->output_head->type == TOK_INT_LIT && !pp->output_head->next) {
                    int64_t val = (int64_t)pp->output_head->literal.int_val.value;
                    pp->output_head = saved_head;
                    pp->output_tail = saved_tail;
                    return val;
                }
                
                /* Restore output */
                pp->output_head = saved_head;
                pp->output_tail = saved_tail;
            }
        }
        
        /* Unknown identifier evaluates to 0 */
        return 0;
    }
    
    if (tok->type == TOK_LPAREN) {
        int64_t val = pp_eval_expr(pp);
        mcc_lexer_expect(pp->lexer, TOK_RPAREN, ")");
        return val;
    }
    
    if (tok->type == TOK_NOT) {
        return !pp_eval_primary(pp);
    }
    
    if (tok->type == TOK_TILDE) {
        return ~pp_eval_primary(pp);
    }
    
    if (tok->type == TOK_MINUS) {
        return -pp_eval_primary(pp);
    }
    
    if (tok->type == TOK_PLUS) {
        return pp_eval_primary(pp);
    }
    
    mcc_error(pp->ctx, "Unexpected token in preprocessor expression: %s",
              mcc_token_to_string(tok));
    return 0;
}

/* ============================================================
 * Binary Operators (in precedence order)
 * ============================================================ */

static int64_t pp_eval_multiplicative(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_primary(pp);
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_STAR) {
            mcc_lexer_next(pp->lexer);
            left *= pp_eval_primary(pp);
        } else if (tok->type == TOK_SLASH) {
            mcc_lexer_next(pp->lexer);
            int64_t right = pp_eval_primary(pp);
            if (right == 0) {
                mcc_error(pp->ctx, "Division by zero in preprocessor expression");
                return 0;
            }
            left /= right;
        } else if (tok->type == TOK_PERCENT) {
            mcc_lexer_next(pp->lexer);
            int64_t right = pp_eval_primary(pp);
            if (right == 0) {
                mcc_error(pp->ctx, "Division by zero in preprocessor expression");
                return 0;
            }
            left %= right;
        } else {
            break;
        }
    }
    
    return left;
}

static int64_t pp_eval_additive(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_multiplicative(pp);
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_PLUS) {
            mcc_lexer_next(pp->lexer);
            left += pp_eval_multiplicative(pp);
        } else if (tok->type == TOK_MINUS) {
            mcc_lexer_next(pp->lexer);
            left -= pp_eval_multiplicative(pp);
        } else {
            break;
        }
    }
    
    return left;
}

static int64_t pp_eval_shift(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_additive(pp);
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_LSHIFT) {
            mcc_lexer_next(pp->lexer);
            left <<= pp_eval_additive(pp);
        } else if (tok->type == TOK_RSHIFT) {
            mcc_lexer_next(pp->lexer);
            left >>= pp_eval_additive(pp);
        } else {
            break;
        }
    }
    
    return left;
}

static int64_t pp_eval_relational(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_shift(pp);
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_LT) {
            mcc_lexer_next(pp->lexer);
            left = left < pp_eval_shift(pp);
        } else if (tok->type == TOK_GT) {
            mcc_lexer_next(pp->lexer);
            left = left > pp_eval_shift(pp);
        } else if (tok->type == TOK_LE) {
            mcc_lexer_next(pp->lexer);
            left = left <= pp_eval_shift(pp);
        } else if (tok->type == TOK_GE) {
            mcc_lexer_next(pp->lexer);
            left = left >= pp_eval_shift(pp);
        } else {
            break;
        }
    }
    
    return left;
}

static int64_t pp_eval_equality(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_relational(pp);
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_EQ) {
            mcc_lexer_next(pp->lexer);
            left = left == pp_eval_relational(pp);
        } else if (tok->type == TOK_NE) {
            mcc_lexer_next(pp->lexer);
            left = left != pp_eval_relational(pp);
        } else {
            break;
        }
    }
    
    return left;
}

static int64_t pp_eval_bitand(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_equality(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_AMP) {
        mcc_lexer_next(pp->lexer);
        left &= pp_eval_equality(pp);
    }
    
    return left;
}

static int64_t pp_eval_bitxor(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_bitand(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_CARET) {
        mcc_lexer_next(pp->lexer);
        left ^= pp_eval_bitand(pp);
    }
    
    return left;
}

static int64_t pp_eval_bitor(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_bitxor(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_PIPE) {
        mcc_lexer_next(pp->lexer);
        left |= pp_eval_bitxor(pp);
    }
    
    return left;
}

static int64_t pp_eval_logand(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_bitor(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_AND) {
        mcc_lexer_next(pp->lexer);
        /* Must evaluate right side to consume tokens, even if left is false */
        int64_t right = pp_eval_bitor(pp);
        left = left && right;
    }
    
    return left;
}

static int64_t pp_eval_logor(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_logand(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_OR) {
        mcc_lexer_next(pp->lexer);
        /* Must evaluate right side to consume tokens, even if left is true */
        int64_t right = pp_eval_logand(pp);
        left = left || right;
    }
    
    return left;
}

/* ============================================================
 * Ternary Operator
 * ============================================================ */

static int64_t pp_eval_ternary(mcc_preprocessor_t *pp)
{
    int64_t cond = pp_eval_logor(pp);
    
    if (mcc_lexer_peek(pp->lexer)->type == TOK_QUESTION) {
        mcc_lexer_next(pp->lexer);
        int64_t then_val = pp_eval_expr(pp);
        mcc_lexer_expect(pp->lexer, TOK_COLON, ":");
        int64_t else_val = pp_eval_ternary(pp);
        return cond ? then_val : else_val;
    }
    
    return cond;
}

/* ============================================================
 * Public API
 * ============================================================ */

int64_t pp_eval_expr(mcc_preprocessor_t *pp)
{
    return pp_eval_ternary(pp);
}
