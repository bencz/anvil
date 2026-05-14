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

/* When > 0, the evaluator is walking tokens but the result is not used —
 * suppress arithmetic errors (division by zero, etc.) so `#if 0 && 1/0`
 * matches C short-circuit semantics. */
static int pp_eval_skip_depth = 0;
#define PP_IN_SKIP() (pp_eval_skip_depth > 0)

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

        /* C23 / GNU: __has_include("hdr") or __has_include(<hdr>) returns 1
         * if the header can be located on the include path. */
        if (strcmp(tok->text, "__has_include") == 0) {
            mcc_lexer_expect(pp->lexer, TOK_LPAREN, "(");
            mcc_token_t *name_tok = mcc_lexer_next(pp->lexer);
            const char *filename = NULL;
            bool is_system = false;
            char buf[256];
            if (name_tok->type == TOK_STRING_LIT) {
                filename = name_tok->literal.string_val.value;
            } else if (name_tok->type == TOK_LT) {
                is_system = true;
                size_t len = 0;
                mcc_token_t *t;
                while ((t = mcc_lexer_next(pp->lexer))->type != TOK_GT &&
                       t->type != TOK_NEWLINE && t->type != TOK_EOF) {
                    const char *s = mcc_token_to_string(t);
                    size_t sl = strlen(s);
                    if (len + sl < sizeof(buf) - 1) {
                        memcpy(buf + len, s, sl);
                        len += sl;
                    }
                }
                buf[len] = '\0';
                filename = buf;
            } else {
                mcc_error(pp->ctx, "Expected header name in __has_include");
                return 0;
            }
            mcc_lexer_expect(pp->lexer, TOK_RPAREN, ")");

            char path[1024];
            FILE *f = pp_find_include_file(pp, filename, is_system, path, sizeof(path));
            if (f) { fclose(f); return 1; }
            return 0;
        }

        /* C23: __has_c_attribute(attr) returns the standardisation date of
         * the attribute or 0 if unknown. */
        if (strcmp(tok->text, "__has_c_attribute") == 0) {
            mcc_lexer_expect(pp->lexer, TOK_LPAREN, "(");
            mcc_token_t *name_tok = mcc_lexer_next(pp->lexer);
            /* Optional `std::attr` — consume '::' if present. */
            if (mcc_lexer_peek(pp->lexer)->type == TOK_COLON) {
                mcc_lexer_next(pp->lexer);
                if (mcc_lexer_peek(pp->lexer)->type == TOK_COLON) {
                    mcc_lexer_next(pp->lexer);
                    name_tok = mcc_lexer_next(pp->lexer);
                }
            }
            int result = 0;
            if (name_tok && name_tok->type == TOK_IDENT) {
                const char *n = name_tok->text;
                /* Dates correspond to when the attribute was standardised,
                 * matching gcc/clang conventions. */
                if (strcmp(n, "deprecated")    == 0) result = 201904;
                else if (strcmp(n, "fallthrough")   == 0) result = 201904;
                else if (strcmp(n, "maybe_unused")  == 0) result = 201904;
                else if (strcmp(n, "nodiscard")     == 0) result = 202003;
                else if (strcmp(n, "noreturn")      == 0) result = 202202;
                else if (strcmp(n, "_Noreturn")     == 0) result = 202202;
                else if (strcmp(n, "reproducible")  == 0) result = 202207;
                else if (strcmp(n, "unsequenced")   == 0) result = 202207;
            }
            mcc_lexer_expect(pp->lexer, TOK_RPAREN, ")");
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
                if (!PP_IN_SKIP()) {
                    mcc_error(pp->ctx, "Division by zero in preprocessor expression");
                }
                return 0;
            }
            left /= right;
        } else if (tok->type == TOK_PERCENT) {
            mcc_lexer_next(pp->lexer);
            int64_t right = pp_eval_primary(pp);
            if (right == 0) {
                if (!PP_IN_SKIP()) {
                    mcc_error(pp->ctx, "Division by zero in preprocessor expression");
                }
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
        /* Standard C short-circuit: the right side is still consumed (for
         * token stream correctness), but when `left` is already false we
         * enter skip mode so any divide-by-zero or other error on the RHS
         * is silenced. */
        bool was_false = !left;
        if (was_false) pp_eval_skip_depth++;
        int64_t right = pp_eval_bitor(pp);
        if (was_false) pp_eval_skip_depth--;
        left = left && right;
    }

    return left;
}

static int64_t pp_eval_logor(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_logand(pp);

    while (mcc_lexer_peek(pp->lexer)->type == TOK_OR) {
        mcc_lexer_next(pp->lexer);
        bool was_true = !!left;
        if (was_true) pp_eval_skip_depth++;
        int64_t right = pp_eval_logand(pp);
        if (was_true) pp_eval_skip_depth--;
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
