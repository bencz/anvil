/*
 * MCC - Micro C Compiler
 * Preprocessor - Macro Management
 * 
 * This file handles macro definition, lookup, and expansion.
 */

#include "pp_internal.h"

/* ============================================================
 * Hash Function
 * ============================================================ */

unsigned pp_hash_string(const char *s)
{
    unsigned h = 0;
    while (*s) {
        h = h * 31 + (unsigned char)*s++;
    }
    return h;
}

/* ============================================================
 * Macro Lookup
 * ============================================================ */

mcc_macro_t *pp_lookup_macro(mcc_preprocessor_t *pp, const char *name)
{
    unsigned h = pp_hash_string(name) % pp->macro_table_size;
    for (mcc_macro_t *m = pp->macros[h]; m; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

/* ============================================================
 * Expansion Stack (Recursion Prevention)
 * ============================================================ */

bool pp_is_expanding(mcc_preprocessor_t *pp, const char *name)
{
    for (size_t i = 0; i < pp->num_expanding; i++) {
        if (strcmp(pp->expanding_macros[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void pp_push_expanding(mcc_preprocessor_t *pp, const char *name)
{
    size_t n = pp->num_expanding;
    const char **new_stack = mcc_realloc(pp->ctx, (void*)pp->expanding_macros,
                                          n * sizeof(char*), (n + 1) * sizeof(char*));
    new_stack[n] = name;
    pp->expanding_macros = new_stack;
    pp->num_expanding = n + 1;
}

void pp_pop_expanding(mcc_preprocessor_t *pp)
{
    if (pp->num_expanding > 0) {
        pp->num_expanding--;
    }
}

/* ============================================================
 * Macro Expansion
 * ============================================================ */

void pp_expand_macro(mcc_preprocessor_t *pp, mcc_macro_t *macro)
{
    pp_push_expanding(pp, macro->name);
    
    if (macro->is_function_like) {
        /* Read arguments */
        mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
        if (tok->type != TOK_LPAREN) {
            /* Not a function-like invocation, emit as identifier */
            mcc_token_t *ident = mcc_token_create(pp->ctx);
            ident->type = TOK_IDENT;
            ident->text = macro->name;
            pp_emit_token(pp, ident);
            pp_pop_expanding(pp);
            return;
        }
        
        mcc_lexer_next(pp->lexer); /* Consume ( */
        
        /* Collect arguments */
        mcc_token_t **args = NULL;
        int num_args = 0;
        int paren_depth = 0;
        mcc_token_t *arg_head = NULL, *arg_tail = NULL;
        
        while (1) {
            tok = mcc_lexer_next(pp->lexer);
            
            if (tok->type == TOK_EOF) {
                mcc_error(pp->ctx, "Unterminated macro argument list");
                break;
            }
            
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
                continue;
            }
            
            /* Add token to current argument */
            tok = mcc_token_copy(pp->ctx, tok);
            tok->next = NULL;
            if (!arg_head) arg_head = tok;
            if (arg_tail) arg_tail->next = tok;
            arg_tail = tok;
        }
        
        /* Check argument count */
        if (num_args != macro->num_params && !macro->is_variadic) {
            mcc_error(pp->ctx, "Macro '%s' expects %d arguments, got %d",
                      macro->name, macro->num_params, num_args);
        }
        
        /* Pre-expand arguments (C standard requires this) */
        /* Note: Arguments are expanded BEFORE the macro is pushed to the expansion stack */
        /* So we need to temporarily pop the current macro to allow recursive expansion */
        mcc_token_t **expanded_args = NULL;
        if (num_args > 0) {
            expanded_args = mcc_alloc(pp->ctx, num_args * sizeof(mcc_token_t*));
            
            /* Temporarily pop current macro from expansion stack */
            pp_pop_expanding(pp);
            
            for (int i = 0; i < num_args; i++) {
                /* Save current output */
                mcc_token_t *saved_head = pp->output_head;
                mcc_token_t *saved_tail = pp->output_tail;
                pp->output_head = pp->output_tail = NULL;
                
                /* Expand argument */
                pp_process_token_list(pp, args[i]);
                
                /* Save expanded argument */
                expanded_args[i] = pp->output_head;
                
                /* Restore output */
                pp->output_head = saved_head;
                pp->output_tail = saved_tail;
            }
            
            /* Re-push current macro to expansion stack */
            pp_push_expanding(pp, macro->name);
        }
        
        /* Build expanded token list with argument substitution */
        mcc_token_t *expanded_head = NULL, *expanded_tail = NULL;
        
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
                    /* Substitute with pre-expanded argument tokens */
                    mcc_token_t *arg_list = expanded_args ? expanded_args[param_idx] : args[param_idx];
                    for (mcc_token_t *arg_tok = arg_list; arg_tok; arg_tok = arg_tok->next) {
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, arg_tok);
                        copy->next = NULL;
                        if (!expanded_head) expanded_head = copy;
                        if (expanded_tail) expanded_tail->next = copy;
                        expanded_tail = copy;
                    }
                    continue;
                }
                
                /* Check for __VA_ARGS__ (C99+) */
                if (macro->is_variadic && strcmp(body_tok->text, "__VA_ARGS__") == 0) {
                    if (pp_has_variadic_macros(pp)) {
                        /* Add all variadic arguments */
                        for (int i = macro->num_params; i < num_args; i++) {
                            if (i > macro->num_params) {
                                /* Add comma between variadic args */
                                mcc_token_t *comma = mcc_token_create(pp->ctx);
                                comma->type = TOK_COMMA;
                                comma->text = ",";
                                comma->next = NULL;
                                if (!expanded_head) expanded_head = comma;
                                if (expanded_tail) expanded_tail->next = comma;
                                expanded_tail = comma;
                            }
                            for (mcc_token_t *arg_tok = args[i]; arg_tok; arg_tok = arg_tok->next) {
                                mcc_token_t *copy = mcc_token_copy(pp->ctx, arg_tok);
                                copy->next = NULL;
                                if (!expanded_head) expanded_head = copy;
                                if (expanded_tail) expanded_tail->next = copy;
                                expanded_tail = copy;
                            }
                        }
                        continue;
                    } else {
                        mcc_error(pp->ctx, "__VA_ARGS__ requires C99 or later (-std=c99)");
                    }
                }
            }
            
            /* Copy token to expanded list */
            mcc_token_t *copy = mcc_token_copy(pp->ctx, body_tok);
            copy->next = NULL;
            if (!expanded_head) expanded_head = copy;
            if (expanded_tail) expanded_tail->next = copy;
            expanded_tail = copy;
        }
        
        /* Process expanded tokens (handles nested macros) */
        if (expanded_head) {
            pp_process_token_list(pp, expanded_head);
        }
        
    } else {
        /* Object-like macro - build expanded token list */
        mcc_token_t *expanded_head = NULL, *expanded_tail = NULL;
        for (mcc_token_t *body_tok = macro->body; body_tok; body_tok = body_tok->next) {
            mcc_token_t *copy = mcc_token_copy(pp->ctx, body_tok);
            copy->next = NULL;
            if (!expanded_head) expanded_head = copy;
            if (expanded_tail) expanded_tail->next = copy;
            expanded_tail = copy;
        }
        
        /* Process expanded tokens (handles nested macros) */
        if (expanded_head) {
            pp_process_token_list(pp, expanded_head);
        }
    }
    
    pp_pop_expanding(pp);
}

/* ============================================================
 * #define Processing
 * ============================================================ */

void pp_process_define(mcc_preprocessor_t *pp)
{
    mcc_token_t *name_tok = mcc_lexer_next(pp->lexer);
    if (name_tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #define");
        pp_skip_line(pp);
        return;
    }
    
    const char *name = mcc_strdup(pp->ctx, name_tok->text);
    
    mcc_macro_t *macro = mcc_alloc(pp->ctx, sizeof(mcc_macro_t));
    macro->name = name;
    macro->def_loc = name_tok->location;
    
    /* Check for function-like macro (no space before paren) */
    mcc_token_t *next = mcc_lexer_peek(pp->lexer);
    if (next->type == TOK_LPAREN && !next->has_space) {
        macro->is_function_like = true;
        mcc_lexer_next(pp->lexer); /* Consume ( */
        
        /* Read parameters */
        mcc_macro_param_t *param_head = NULL, *param_tail = NULL;
        
        if (mcc_lexer_peek(pp->lexer)->type != TOK_RPAREN) {
            while (1) {
                mcc_token_t *param_tok = mcc_lexer_next(pp->lexer);
                
                if (param_tok->type == TOK_ELLIPSIS) {
                    /* Variadic macro - check C standard */
                    if (!pp_has_variadic_macros(pp)) {
                        mcc_warning(pp->ctx, "Variadic macros are a C99 feature");
                    }
                    macro->is_variadic = true;
                    break;
                }
                
                if (param_tok->type != TOK_IDENT) {
                    mcc_error(pp->ctx, "Expected parameter name");
                    pp_skip_line(pp);
                    return;
                }
                
                mcc_macro_param_t *param = mcc_alloc(pp->ctx, sizeof(mcc_macro_param_t));
                param->name = mcc_strdup(pp->ctx, param_tok->text);
                
                if (!param_head) param_head = param;
                if (param_tail) param_tail->next = param;
                param_tail = param;
                macro->num_params++;
                
                if (mcc_lexer_peek(pp->lexer)->type == TOK_COMMA) {
                    mcc_lexer_next(pp->lexer);
                } else {
                    break;
                }
            }
        }
        
        macro->params = param_head;
        mcc_lexer_expect(pp->lexer, TOK_RPAREN, ")");
    }
    
    /* Read replacement list */
    mcc_token_t *body_head = NULL, *body_tail = NULL;
    mcc_token_t *tok;
    
    while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_NEWLINE &&
           tok->type != TOK_EOF) {
        tok = mcc_token_copy(pp->ctx, tok);
        tok->next = NULL;
        if (!body_head) body_head = tok;
        if (body_tail) body_tail->next = tok;
        body_tail = tok;
    }
    
    macro->body = body_head;
    
    /* Check for redefinition */
    mcc_macro_t *existing = pp_lookup_macro(pp, name);
    if (existing) {
        /* TODO: Check if definitions are identical */
        mcc_warning(pp->ctx, "Macro '%s' redefined", name);
    }
    
    /* Insert into hash table */
    unsigned h = pp_hash_string(name) % pp->macro_table_size;
    macro->next = pp->macros[h];
    pp->macros[h] = macro;
}

/* ============================================================
 * Public API Implementation
 * ============================================================ */

void mcc_preprocessor_define(mcc_preprocessor_t *pp, const char *name, const char *value)
{
    /* Check if already defined */
    mcc_macro_t *existing = pp_lookup_macro(pp, name);
    if (existing) {
        /* Redefine */
        existing->body = NULL;
        if (value) {
            mcc_lexer_t *lex = mcc_lexer_create(pp->ctx);
            mcc_lexer_init_string(lex, value, "<define>");
            
            mcc_token_t *head = NULL, *tail = NULL;
            mcc_token_t *tok;
            while ((tok = mcc_lexer_next(lex))->type != TOK_EOF &&
                   tok->type != TOK_NEWLINE) {
                tok = mcc_token_copy(pp->ctx, tok);
                if (!head) head = tok;
                if (tail) tail->next = tok;
                tail = tok;
            }
            existing->body = head;
        }
        return;
    }
    
    /* Create new macro */
    mcc_macro_t *macro = mcc_alloc(pp->ctx, sizeof(mcc_macro_t));
    macro->name = mcc_strdup(pp->ctx, name);
    macro->is_function_like = false;
    
    if (value) {
        mcc_lexer_t *lex = mcc_lexer_create(pp->ctx);
        mcc_lexer_init_string(lex, value, "<define>");
        
        mcc_token_t *head = NULL, *tail = NULL;
        mcc_token_t *tok;
        while ((tok = mcc_lexer_next(lex))->type != TOK_EOF &&
               tok->type != TOK_NEWLINE) {
            tok = mcc_token_copy(pp->ctx, tok);
            if (!head) head = tok;
            if (tail) tail->next = tok;
            tail = tok;
        }
        macro->body = head;
    }
    
    /* Insert into hash table */
    unsigned h = pp_hash_string(name) % pp->macro_table_size;
    macro->next = pp->macros[h];
    pp->macros[h] = macro;
}

void mcc_preprocessor_undef(mcc_preprocessor_t *pp, const char *name)
{
    unsigned h = pp_hash_string(name) % pp->macro_table_size;
    mcc_macro_t **p = &pp->macros[h];
    while (*p) {
        if (strcmp((*p)->name, name) == 0) {
            *p = (*p)->next;
            return;
        }
        p = &(*p)->next;
    }
}

mcc_macro_t *mcc_preprocessor_lookup_macro(mcc_preprocessor_t *pp, const char *name)
{
    return pp_lookup_macro(pp, name);
}

bool mcc_preprocessor_is_defined(mcc_preprocessor_t *pp, const char *name)
{
    return pp_lookup_macro(pp, name) != NULL;
}

/* ============================================================
 * Builtin Macros
 * ============================================================ */

void pp_define_standard_macros(mcc_preprocessor_t *pp)
{
    /* Get predefined macros for the current C standard */
    size_t count;
    const mcc_predefined_macro_t *macros = mcc_c_std_get_predefined_macros(
        mcc_ctx_get_std(pp->ctx), &count);
    
    for (size_t i = 0; i < count; i++) {
        mcc_preprocessor_define(pp, macros[i].name, macros[i].value);
    }
    
    /* Compiler identification */
    mcc_preprocessor_define(pp, "__MCC__", "1");
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", MCC_VERSION_MAJOR);
    mcc_preprocessor_define(pp, "__MCC_VERSION_MAJOR__", buf);
    snprintf(buf, sizeof(buf), "%d", MCC_VERSION_MINOR);
    mcc_preprocessor_define(pp, "__MCC_VERSION_MINOR__", buf);
    
    /* Date and time (set at preprocessing time) */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    snprintf(buf, sizeof(buf), "\"%s %2d %d\"",
             months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    mcc_preprocessor_define(pp, "__DATE__", buf);
    
    snprintf(buf, sizeof(buf), "\"%02d:%02d:%02d\"",
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    mcc_preprocessor_define(pp, "__TIME__", buf);
}

void mcc_preprocessor_define_builtins(mcc_preprocessor_t *pp)
{
    pp_define_standard_macros(pp);
}
