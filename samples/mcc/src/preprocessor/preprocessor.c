/*
 * MCC - Micro C Compiler
 * Preprocessor - Main Module
 * 
 * This file contains the public API and main preprocessing loop.
 */

#include "pp_internal.h"

/* ============================================================
 * Preprocessor Creation/Destruction
 * ============================================================ */

mcc_preprocessor_t *mcc_preprocessor_create(mcc_context_t *ctx)
{
    mcc_preprocessor_t *pp = mcc_alloc(ctx, sizeof(mcc_preprocessor_t));
    if (!pp) return NULL;
    
    pp->ctx = ctx;
    pp->lexer = mcc_lexer_create(ctx);
    if (!pp->lexer) return NULL;
    
    pp->macro_table_size = PP_MACRO_TABLE_SIZE;
    pp->macros = mcc_alloc(ctx, PP_MACRO_TABLE_SIZE * sizeof(mcc_macro_t*));
    if (!pp->macros) return NULL;
    
    return pp;
}

void mcc_preprocessor_destroy(mcc_preprocessor_t *pp)
{
    (void)pp; /* Arena allocated */
}

/* ============================================================
 * Token Output
 * ============================================================ */

void pp_emit_token(mcc_preprocessor_t *pp, mcc_token_t *tok)
{
    if (!pp || !tok) return;
    tok = mcc_token_copy(pp->ctx, tok);
    if (!tok) return;
    tok->next = NULL;
    
    /* Apply saved has_space if set */
    if (pp->use_next_has_space) {
        tok->has_space = pp->next_has_space;
        pp->use_next_has_space = false;
    }
    
    if (!pp->output_head) {
        pp->output_head = tok;
    }
    if (pp->output_tail) {
        pp->output_tail->next = tok;
    }
    pp->output_tail = tok;
}

/* ============================================================
 * Token Processing
 * ============================================================ */

void pp_process_token(mcc_preprocessor_t *pp, mcc_token_t *tok)
{
    /* Check for macro expansion */
    if (tok->type == TOK_IDENT && !pp->skip_mode) {
        /* _Pragma requires destringizing and processing the resulting pragma.
         * Reject it until that pipeline exists instead of silently discarding
         * the requested semantics. */
        if (strcmp(tok->text, "_Pragma") == 0) {
            mcc_error_at(pp->ctx, tok->location,
                         "_Pragma is not implemented by MCC");
            mcc_token_t *peek = mcc_lexer_peek(pp->lexer);
            if (peek->type == TOK_LPAREN) {
                int depth = 0;
                do {
                    mcc_token_t *part = mcc_lexer_next(pp->lexer);
                    if (part->type == TOK_LPAREN) depth++;
                    else if (part->type == TOK_RPAREN) depth--;
                    else if (part->type == TOK_EOF || part->type == TOK_NEWLINE)
                        break;
                } while (depth > 0);
            }
            return;
        }

        /* Handle special predefined macros */
        if (strcmp(tok->text, "__FILE__") == 0) {
            mcc_token_t *str_tok = mcc_token_create(pp->ctx);
            str_tok->type = TOK_STRING_LIT;
            char buf[512];
            snprintf(buf, sizeof(buf), "\"%s\"", pp->lexer->filename ? pp->lexer->filename : "");
            str_tok->text = mcc_strdup(pp->ctx, buf);
            str_tok->literal.string_val.value = pp->lexer->filename ? pp->lexer->filename : "";
            str_tok->literal.string_val.length = strlen(str_tok->literal.string_val.value);
            str_tok->location = tok->location;
            pp_emit_token(pp, str_tok);
            return;
        }
        if (strcmp(tok->text, "__LINE__") == 0) {
            mcc_token_t *int_tok = mcc_token_create(pp->ctx);
            int_tok->type = TOK_INT_LIT;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", tok->location.line);
            int_tok->text = mcc_strdup(pp->ctx, buf);
            int_tok->literal.int_val.value = tok->location.line;
            int_tok->literal.int_val.suffix = INT_SUFFIX_NONE;
            int_tok->location = tok->location;
            pp_emit_token(pp, int_tok);
            return;
        }
        
        mcc_macro_t *macro = pp_lookup_macro(pp, tok->text);
        if (macro && !pp_is_expanding(pp, tok->text)) {
            /* Collect tokens from lexer for expansion */
            /* Start with the macro name token */
            mcc_token_t *token_list = mcc_token_copy(pp->ctx, tok);
            token_list->next = NULL;
            mcc_token_t *list_tail = token_list;
            
            /* For function-like macros, collect arguments */
            if (macro->is_function_like) {
                mcc_token_t *peek = mcc_lexer_peek(pp->lexer);
                
                /* Skip empty object-like macros to find ( */
                while (peek->type == TOK_IDENT) {
                    mcc_macro_t *m = pp_lookup_macro(pp, peek->text);
                    if (m && !m->is_function_like && !m->body) {
                        /* Empty macro - collect it and continue looking */
                        mcc_token_t *next = mcc_lexer_next(pp->lexer);
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, next);
                        copy->next = NULL;
                        list_tail->next = copy;
                        list_tail = copy;
                        peek = mcc_lexer_peek(pp->lexer);
                    } else {
                        break;
                    }
                }
                
                if (peek->type == TOK_LPAREN) {
                    /* Collect all tokens until matching ) */
                    int paren_depth = 0;
                    do {
                        mcc_token_t *next = mcc_lexer_next(pp->lexer);
                        /* Skip newlines inside macro arguments */
                        if (next->type == TOK_NEWLINE) continue;
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, next);
                        copy->next = NULL;
                        list_tail->next = copy;
                        list_tail = copy;
                        
                        if (next->type == TOK_LPAREN) paren_depth++;
                        else if (next->type == TOK_RPAREN) paren_depth--;
                        else if (next->type == TOK_EOF) break;
                    } while (paren_depth > 0);
                }
            } else {
                /* For object-like macros, check if next token is ( 
                 * This enables deferred expansion where an object-like macro
                 * expands to a function-like macro name */
                mcc_token_t *peek = mcc_lexer_peek(pp->lexer);
                if (peek->type == TOK_LPAREN) {
                    /* Collect potential function call for deferred expansion */
                    int paren_depth = 0;
                    do {
                        mcc_token_t *next = mcc_lexer_next(pp->lexer);
                        /* Skip newlines inside macro arguments */
                        if (next->type == TOK_NEWLINE) continue;
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, next);
                        copy->next = NULL;
                        list_tail->next = copy;
                        list_tail = copy;
                        
                        if (next->type == TOK_LPAREN) paren_depth++;
                        else if (next->type == TOK_RPAREN) paren_depth--;
                        else if (next->type == TOK_EOF) break;
                    } while (paren_depth > 0);
                }
            }
            
            /* Now expand the collected tokens */
            pp->next_has_space = tok->has_space;
            pp->use_next_has_space = true;
            pp_expand_and_emit(pp, token_list);
            return;
        }
    }
    
    /* Emit token as-is */
    if (!pp->skip_mode) {
        pp_emit_token(pp, tok);
    }
}

/* Process a token list with support for nested macro expansion */
void pp_process_token_list(mcc_preprocessor_t *pp, mcc_token_t *tokens)
{
    /* Use the new expansion algorithm that correctly handles deferred expansion */
    pp_expand_and_emit(pp, tokens);
}

/* Old implementation - kept for reference but not used */

/* ============================================================
 * Main Preprocessing Loop
 * ============================================================ */

mcc_token_t *mcc_preprocessor_run(mcc_preprocessor_t *pp, const char *filename)
{
    mcc_lexer_init_file(pp->lexer, filename);
    return mcc_preprocessor_run_string(pp, pp->lexer->source, filename);
}

mcc_token_t *mcc_preprocessor_run_string(mcc_preprocessor_t *pp, const char *source,
                                          const char *filename)
{
    mcc_lexer_init_string(pp->lexer, source, filename);
    
    pp->output_head = NULL;
    pp->output_tail = NULL;
    
    while (1) {
        mcc_token_t *tok = mcc_lexer_next(pp->lexer);
        
        if (tok->type == TOK_EOF) {
            /* Check for pending includes */
            if (pp_pop_include(pp)) {
                continue;
            }
            
            /* Check for unclosed conditionals */
            if (pp->cond_stack) {
                mcc_error_at(pp->ctx, pp->cond_stack->location,
                             "Unterminated conditional directive");
            }
            
            /* Emit EOF */
            pp_emit_token(pp, tok);
            break;
        }
        
        if (tok->type == TOK_NEWLINE) {
            continue;
        }
        
        if (tok->type == TOK_HASH && tok->at_bol) {
            pp_process_directive(pp);
            continue;
        }
        
        if (pp->skip_mode) {
            continue;
        }
        
        pp_process_token(pp, tok);
    }
    
    return pp->output_head;
}

/* ============================================================
 * Token Access API
 * ============================================================ */

mcc_token_t *mcc_preprocessor_next(mcc_preprocessor_t *pp)
{
    if (!pp->current) {
        pp->current = pp->output_head;
    } else {
        pp->current = pp->current->next;
    }
    return pp->current ? pp->current : mcc_token_create(pp->ctx);
}

mcc_token_t *mcc_preprocessor_peek(mcc_preprocessor_t *pp)
{
    mcc_token_t *next = pp->current ? pp->current->next : pp->output_head;
    return next ? next : mcc_token_create(pp->ctx);
}
