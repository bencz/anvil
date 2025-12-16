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
    
    pp->macro_table_size = PP_MACRO_TABLE_SIZE;
    pp->macros = mcc_alloc(ctx, PP_MACRO_TABLE_SIZE * sizeof(mcc_macro_t*));
    
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
    tok = mcc_token_copy(pp->ctx, tok);
    tok->next = NULL;
    
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
            pp_expand_macro(pp, macro);
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
    mcc_token_t *tok = tokens;
    
    while (tok) {
        if (tok->type == TOK_IDENT && !pp->skip_mode) {
            mcc_macro_t *macro = pp_lookup_macro(pp, tok->text);
            if (macro && !pp_is_expanding(pp, tok->text)) {
                if (macro->is_function_like) {
                    /* Check if next token is '(' */
                    mcc_token_t *next = tok->next;
                    if (next && next->type == TOK_LPAREN) {
                        /* Collect arguments from token list */
                        pp_push_expanding(pp, macro->name);
                        
                        mcc_token_t **args = NULL;
                        int num_args = 0;
                        int paren_depth = 0;
                        mcc_token_t *arg_head = NULL, *arg_tail = NULL;
                        
                        tok = next->next; /* Skip past '(' */
                        
                        while (tok) {
                            if (tok->type == TOK_LPAREN) {
                                paren_depth++;
                            } else if (tok->type == TOK_RPAREN) {
                                if (paren_depth == 0) {
                                    /* End of arguments */
                                    if (arg_head || num_args > 0) {
                                        args = mcc_realloc(pp->ctx, args,
                                                           num_args * sizeof(mcc_token_t*),
                                                           (num_args + 1) * sizeof(mcc_token_t*));
                                        args[num_args++] = arg_head;
                                    }
                                    tok = tok->next; /* Move past ')' */
                                    break;
                                }
                                paren_depth--;
                            } else if (tok->type == TOK_COMMA && paren_depth == 0) {
                                /* Next argument */
                                args = mcc_realloc(pp->ctx, args,
                                                   num_args * sizeof(mcc_token_t*),
                                                   (num_args + 1) * sizeof(mcc_token_t*));
                                args[num_args++] = arg_head;
                                arg_head = arg_tail = NULL;
                                tok = tok->next;
                                continue;
                            }
                            
                            /* Add token to current argument */
                            mcc_token_t *arg_tok = mcc_token_copy(pp->ctx, tok);
                            arg_tok->next = NULL;
                            if (!arg_head) arg_head = arg_tok;
                            if (arg_tail) arg_tail->next = arg_tok;
                            arg_tail = arg_tok;
                            
                            tok = tok->next;
                        }
                        
                        /* Pre-expand arguments */
                        /* Temporarily pop current macro to allow recursive expansion */
                        mcc_token_t **expanded_args = NULL;
                        if (num_args > 0) {
                            expanded_args = mcc_alloc(pp->ctx, num_args * sizeof(mcc_token_t*));
                            pp_pop_expanding(pp);
                            for (int i = 0; i < num_args; i++) {
                                mcc_token_t *saved_head = pp->output_head;
                                mcc_token_t *saved_tail = pp->output_tail;
                                pp->output_head = pp->output_tail = NULL;
                                pp_process_token_list(pp, args[i]);
                                expanded_args[i] = pp->output_head;
                                pp->output_head = saved_head;
                                pp->output_tail = saved_tail;
                            }
                            pp_push_expanding(pp, macro->name);
                        }
                        
                        /* Build expanded body with argument substitution */
                        mcc_token_t *expanded_head = NULL, *expanded_tail = NULL;
                        
                        #define APPEND_EXP(t) do { \
                            if (!expanded_head) expanded_head = (t); \
                            if (expanded_tail) expanded_tail->next = (t); \
                            expanded_tail = (t); \
                        } while(0)
                        
                        for (mcc_token_t *body_tok = macro->body; body_tok; body_tok = body_tok->next) {
                            if (body_tok->type == TOK_IDENT) {
                                /* Check if it's a parameter */
                                int param_idx = -1;
                                mcc_macro_param_t *param = macro->params;
                                for (int i = 0; param; param = param->next, i++) {
                                    if (strcmp(param->name, body_tok->text) == 0) {
                                        param_idx = i;
                                        break;
                                    }
                                }
                                
                                if (param_idx >= 0 && param_idx < num_args) {
                                    /* Substitute with pre-expanded argument */
                                    mcc_token_t *arg_list = expanded_args ? expanded_args[param_idx] : args[param_idx];
                                    for (mcc_token_t *a = arg_list; a; a = a->next) {
                                        mcc_token_t *copy = mcc_token_copy(pp->ctx, a);
                                        copy->next = NULL;
                                        APPEND_EXP(copy);
                                    }
                                    continue;
                                }
                                
                                /* Check for __VA_ARGS__ (C99+) */
                                if (macro->is_variadic && strcmp(body_tok->text, "__VA_ARGS__") == 0) {
                                    if (!pp_has_variadic_macros(pp)) {
                                        mcc_error(pp->ctx, "__VA_ARGS__ requires C99 or later (-std=c99)");
                                    } else {
                                        /* Add all variadic arguments */
                                        for (int i = macro->num_params; i < num_args; i++) {
                                            if (i > macro->num_params) {
                                                mcc_token_t *comma = mcc_token_create(pp->ctx);
                                                comma->type = TOK_COMMA;
                                                comma->text = ",";
                                                comma->next = NULL;
                                                APPEND_EXP(comma);
                                            }
                                            mcc_token_t *arg_list = expanded_args ? expanded_args[i] : args[i];
                                            for (mcc_token_t *a = arg_list; a; a = a->next) {
                                                mcc_token_t *copy = mcc_token_copy(pp->ctx, a);
                                                copy->next = NULL;
                                                APPEND_EXP(copy);
                                            }
                                        }
                                    }
                                    continue;
                                }
                            }
                            
                            /* Copy token as-is */
                            mcc_token_t *copy = mcc_token_copy(pp->ctx, body_tok);
                            copy->next = NULL;
                            APPEND_EXP(copy);
                        }
                        
                        #undef APPEND_EXP
                        
                        /* Recursively process expanded tokens */
                        if (expanded_head) {
                            pp_process_token_list(pp, expanded_head);
                        }
                        
                        pp_pop_expanding(pp);
                        continue;
                    }
                }
                /* Object-like macro or function-like without '(' */
                pp_expand_macro(pp, macro);
                tok = tok->next;
                continue;
            }
        }
        
        /* Emit token as-is */
        if (!pp->skip_mode) {
            pp_emit_token(pp, tok);
        }
        tok = tok->next;
    }
}

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
