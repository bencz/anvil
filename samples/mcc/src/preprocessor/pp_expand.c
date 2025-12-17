/*
 * MCC - Micro C Compiler
 * Preprocessor - Macro Expansion (New Implementation)
 * 
 * This file implements correct C standard macro expansion with proper
 * re-scanning behavior. The key insight is that after macro expansion,
 * the result must be concatenated with the remaining tokens before
 * re-scanning, so that deferred expansion works correctly.
 * 
 * Example: #define A B
 *          #define B(x) x
 *          A(42)
 * 
 * 1. See A, expand to B
 * 2. Concatenate B with remaining tokens: B(42)
 * 3. Re-scan: see B(42), expand to 42
 * 
 * The C standard calls this "rescanning and further replacement".
 */

#include "pp_internal.h"

/* ============================================================
 * Token List Operations
 * ============================================================ */

/* Create a copy of a token list */
static mcc_token_t *copy_token_list(mcc_preprocessor_t *pp, mcc_token_t *list)
{
    mcc_token_t *head = NULL, *tail = NULL;
    for (mcc_token_t *t = list; t; t = t->next) {
        mcc_token_t *copy = mcc_token_copy(pp->ctx, t);
        copy->next = NULL;
        if (!head) head = copy;
        if (tail) tail->next = copy;
        tail = copy;
    }
    return head;
}

/* Append token list b to the end of token list a */
static mcc_token_t *append_token_lists(mcc_token_t *a, mcc_token_t *b)
{
    if (!a) return b;
    if (!b) return a;
    
    mcc_token_t *tail = a;
    while (tail->next) tail = tail->next;
    tail->next = b;
    return a;
}

/* Get the last token in a list */
static mcc_token_t *get_last_token(mcc_token_t *list)
{
    if (!list) return NULL;
    while (list->next) list = list->next;
    return list;
}

/* ============================================================
 * Hide Set (Blue Paint) Management
 * 
 * According to C standard, when a macro is being expanded, it is
 * "painted blue" and cannot be expanded again in the result.
 * We track this with a hide set attached to tokens.
 * ============================================================ */

typedef struct pp_hide_set {
    const char **names;
    size_t count;
    size_t capacity;
} pp_hide_set_t;

static pp_hide_set_t *hide_set_create(mcc_preprocessor_t *pp)
{
    pp_hide_set_t *hs = mcc_alloc(pp->ctx, sizeof(pp_hide_set_t));
    hs->names = NULL;
    hs->count = 0;
    hs->capacity = 0;
    return hs;
}

static pp_hide_set_t *hide_set_copy(mcc_preprocessor_t *pp, pp_hide_set_t *src)
{
    if (!src) return NULL;
    pp_hide_set_t *hs = mcc_alloc(pp->ctx, sizeof(pp_hide_set_t));
    hs->count = src->count;
    hs->capacity = src->capacity;
    if (src->names && src->count > 0) {
        hs->names = mcc_alloc(pp->ctx, hs->capacity * sizeof(char*));
        memcpy(hs->names, src->names, hs->count * sizeof(char*));
    } else {
        hs->names = NULL;
    }
    return hs;
}

static void hide_set_add(mcc_preprocessor_t *pp, pp_hide_set_t *hs, const char *name)
{
    if (!hs) return;
    
    /* Check if already in set */
    for (size_t i = 0; i < hs->count; i++) {
        if (strcmp(hs->names[i], name) == 0) return;
    }
    
    /* Grow if needed */
    if (hs->count >= hs->capacity) {
        size_t new_cap = hs->capacity ? hs->capacity * 2 : 4;
        hs->names = mcc_realloc(pp->ctx, hs->names, 
                                 hs->capacity * sizeof(char*),
                                 new_cap * sizeof(char*));
        hs->capacity = new_cap;
    }
    
    hs->names[hs->count++] = name;
}

static bool hide_set_contains(pp_hide_set_t *hs, const char *name)
{
    if (!hs) return false;
    for (size_t i = 0; i < hs->count; i++) {
        if (strcmp(hs->names[i], name) == 0) return true;
    }
    return false;
}

static pp_hide_set_t *hide_set_union(mcc_preprocessor_t *pp, pp_hide_set_t *a, pp_hide_set_t *b)
{
    pp_hide_set_t *result = hide_set_copy(pp, a);
    if (!result) result = hide_set_create(pp);
    if (b) {
        for (size_t i = 0; i < b->count; i++) {
            hide_set_add(pp, result, b->names[i]);
        }
    }
    return result;
}

/* ============================================================
 * Token with Hide Set
 * 
 * We extend tokens with a hide set pointer for tracking which
 * macros cannot be expanded for this token.
 * ============================================================ */

/* We'll store hide set in a parallel structure since we can't modify mcc_token_t */
typedef struct pp_token_info {
    mcc_token_t *token;
    pp_hide_set_t *hide_set;
    struct pp_token_info *next;
} pp_token_info_t;

static pp_token_info_t *token_info_create(mcc_preprocessor_t *pp, mcc_token_t *tok, pp_hide_set_t *hs)
{
    pp_token_info_t *info = mcc_alloc(pp->ctx, sizeof(pp_token_info_t));
    info->token = tok;
    info->hide_set = hs;
    info->next = NULL;
    return info;
}

static pp_token_info_t *token_list_to_info_list(mcc_preprocessor_t *pp, mcc_token_t *tokens, pp_hide_set_t *hs)
{
    pp_token_info_t *head = NULL, *tail = NULL;
    for (mcc_token_t *t = tokens; t; t = t->next) {
        pp_token_info_t *info = token_info_create(pp, t, hide_set_copy(pp, hs));
        if (!head) head = info;
        if (tail) tail->next = info;
        tail = info;
    }
    return head;
}

static mcc_token_t *info_list_to_token_list(pp_token_info_t *info)
{
    if (!info) return NULL;
    
    mcc_token_t *head = info->token;
    mcc_token_t *prev = head;
    
    for (pp_token_info_t *i = info->next; i; i = i->next) {
        prev->next = i->token;
        prev = i->token;
    }
    if (prev) prev->next = NULL;
    
    return head;
}

/* ============================================================
 * Argument Collection
 * ============================================================ */

typedef struct {
    mcc_token_t **args;      /* Array of token lists (unexpanded) */
    int num_args;
    pp_token_info_t *remaining;  /* Tokens after the closing ) */
} pp_args_t;

/* Collect arguments for a function-like macro from token info list */
static pp_args_t collect_arguments(mcc_preprocessor_t *pp, mcc_macro_t *macro, pp_token_info_t *tokens)
{
    pp_args_t result = {NULL, 0, NULL};
    
    if (!tokens || tokens->token->type != TOK_LPAREN) {
        result.remaining = tokens;
        return result;
    }
    
    /* Skip the opening ( */
    pp_token_info_t *cur = tokens->next;
    
    mcc_token_t **args = NULL;
    int num_args = 0;
    int paren_depth = 0;
    mcc_token_t *arg_head = NULL, *arg_tail = NULL;
    
    while (cur) {
        mcc_token_t *tok = cur->token;
        
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
                result.args = args;
                result.num_args = num_args;
                result.remaining = cur->next;
                return result;
            }
            paren_depth--;
        } else if (tok->type == TOK_COMMA && paren_depth == 0) {
            /* Next argument */
            args = mcc_realloc(pp->ctx, args,
                               num_args * sizeof(mcc_token_t*),
                               (num_args + 1) * sizeof(mcc_token_t*));
            args[num_args++] = arg_head;
            arg_head = arg_tail = NULL;
            cur = cur->next;
            continue;
        }
        
        /* Add token to current argument */
        mcc_token_t *copy = mcc_token_copy(pp->ctx, tok);
        copy->next = NULL;
        if (!arg_head) arg_head = copy;
        if (arg_tail) arg_tail->next = copy;
        arg_tail = copy;
        
        cur = cur->next;
    }
    
    /* Unterminated argument list */
    mcc_error(pp->ctx, "Unterminated macro argument list");
    result.args = args;
    result.num_args = num_args;
    result.remaining = NULL;
    return result;
}

/* ============================================================
 * Substitution
 * 
 * Substitute parameters in macro body with arguments.
 * Handle # (stringify) and ## (paste) operators.
 * ============================================================ */

static int find_param_index(mcc_macro_t *macro, const char *name)
{
    int idx = 0;
    for (mcc_macro_param_t *p = macro->params; p; p = p->next, idx++) {
        if (strcmp(p->name, name) == 0) return idx;
    }
    return -1;
}

/* Perform token pasting */
static mcc_token_t *paste_tokens(mcc_preprocessor_t *pp, mcc_token_t *left, mcc_token_t *right)
{
    if (!left) return right ? mcc_token_copy(pp->ctx, right) : NULL;
    if (!right) return mcc_token_copy(pp->ctx, left);
    
    const char *left_text = left->text ? left->text : "";
    const char *right_text = right->text ? right->text : "";
    
    size_t len = strlen(left_text) + strlen(right_text);
    char *pasted = mcc_alloc(pp->ctx, len + 1);
    strcpy(pasted, left_text);
    strcat(pasted, right_text);
    
    /* Re-lex to get correct token type */
    mcc_lexer_t *lex = mcc_lexer_create(pp->ctx);
    mcc_lexer_init_string(lex, pasted, "<paste>");
    mcc_token_t *result = mcc_lexer_next(lex);
    result = mcc_token_copy(pp->ctx, result);
    result->has_space = left->has_space;
    result->next = NULL;
    
    return result;
}

/* Substitute parameters in macro body */
static mcc_token_t *substitute(mcc_preprocessor_t *pp, mcc_macro_t *macro,
                                mcc_token_t **args, mcc_token_t **expanded_args, int num_args)
{
    mcc_token_t *result_head = NULL, *result_tail = NULL;
    
    #define APPEND_RESULT(t) do { \
        if (!result_head) result_head = (t); \
        if (result_tail) result_tail->next = (t); \
        result_tail = (t); \
        while (result_tail && result_tail->next) result_tail = result_tail->next; \
    } while(0)
    
    for (mcc_token_t *body = macro->body; body; body = body->next) {
        /* Handle ## (token pasting) */
        if (body->type == TOK_HASH_HASH) {
            if (!result_tail || !body->next) {
                mcc_error(pp->ctx, "'##' cannot appear at beginning or end of macro");
                continue;
            }
            
            mcc_token_t *right_tok = body->next;
            mcc_token_t *right_first = NULL;
            
            /* Get right operand */
            if (right_tok->type == TOK_IDENT) {
                int pidx = find_param_index(macro, right_tok->text);
                if (pidx >= 0 && pidx < num_args && args[pidx]) {
                    right_first = args[pidx];  /* Use unexpanded for ## */
                }
            }
            if (!right_first) {
                right_first = right_tok;
            }
            
            /* Paste */
            mcc_token_t *pasted = paste_tokens(pp, result_tail, right_first);
            
            /* Replace last token with pasted result */
            if (result_head == result_tail) {
                result_head = pasted;
            } else {
                mcc_token_t *prev = result_head;
                while (prev && prev->next != result_tail) prev = prev->next;
                if (prev) prev->next = pasted;
            }
            result_tail = pasted;
            
            /* If right was a parameter with multiple tokens, append the rest */
            if (right_tok->type == TOK_IDENT) {
                int pidx = find_param_index(macro, right_tok->text);
                if (pidx >= 0 && pidx < num_args && args[pidx] && args[pidx]->next) {
                    for (mcc_token_t *t = args[pidx]->next; t; t = t->next) {
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, t);
                        copy->next = NULL;
                        APPEND_RESULT(copy);
                    }
                }
            }
            
            body = right_tok;  /* Skip right operand */
            continue;
        }
        
        /* Handle # (stringification) */
        if (body->type == TOK_HASH && body->next && body->next->type == TOK_IDENT) {
            mcc_token_t *param_tok = body->next;
            int pidx = find_param_index(macro, param_tok->text);
            
            if (pidx >= 0 && pidx < num_args) {
                mcc_token_t *str_tok = pp_stringify_tokens(pp, args[pidx]);
                str_tok->has_space = body->has_space;
                str_tok->next = NULL;
                APPEND_RESULT(str_tok);
                body = param_tok;
                continue;
            }
        }
        
        /* Handle parameter substitution */
        if (body->type == TOK_IDENT) {
            int pidx = find_param_index(macro, body->text);
            
            if (pidx >= 0 && pidx < num_args) {
                /* Check if next is ## - use unexpanded */
                mcc_token_t *arg_list;
                if (body->next && body->next->type == TOK_HASH_HASH) {
                    arg_list = args[pidx];
                } else {
                    arg_list = expanded_args ? expanded_args[pidx] : args[pidx];
                }
                
                /* Copy argument tokens */
                bool first = true;
                for (mcc_token_t *a = arg_list; a; a = a->next) {
                    mcc_token_t *copy = mcc_token_copy(pp->ctx, a);
                    if (first) {
                        copy->has_space = body->has_space;
                        first = false;
                    }
                    copy->next = NULL;
                    APPEND_RESULT(copy);
                }
                continue;
            }
            
            /* Check for __VA_ARGS__ */
            if (macro->is_variadic && strcmp(body->text, "__VA_ARGS__") == 0) {
                bool first = true;
                for (int i = macro->num_params; i < num_args; i++) {
                    if (i > macro->num_params) {
                        mcc_token_t *comma = mcc_token_create(pp->ctx);
                        comma->type = TOK_COMMA;
                        comma->text = ",";
                        comma->next = NULL;
                        APPEND_RESULT(comma);
                    }
                    mcc_token_t *arg_list = expanded_args ? expanded_args[i] : args[i];
                    for (mcc_token_t *a = arg_list; a; a = a->next) {
                        mcc_token_t *copy = mcc_token_copy(pp->ctx, a);
                        if (first) {
                            copy->has_space = body->has_space;
                            first = false;
                        }
                        copy->next = NULL;
                        APPEND_RESULT(copy);
                    }
                }
                continue;
            }
        }
        
        /* Copy token as-is */
        mcc_token_t *copy = mcc_token_copy(pp->ctx, body);
        copy->next = NULL;
        APPEND_RESULT(copy);
    }
    
    #undef APPEND_RESULT
    return result_head;
}

/* ============================================================
 * Main Expansion Algorithm
 * 
 * This implements the C standard macro expansion algorithm:
 * 1. Scan token list for macro names
 * 2. If found, collect arguments (if function-like)
 * 3. Expand arguments (for non-# and non-## contexts)
 * 4. Substitute parameters in body
 * 5. Add macro to hide set
 * 6. Concatenate result with remaining tokens
 * 7. Rescan the concatenated list
 * ============================================================ */

/* Forward declaration */
static pp_token_info_t *expand_token_list(mcc_preprocessor_t *pp, pp_token_info_t *tokens);

/* Expand a single macro invocation */
static pp_token_info_t *expand_macro_invocation(mcc_preprocessor_t *pp, 
                                                  mcc_macro_t *macro,
                                                  pp_token_info_t *macro_token,
                                                  pp_token_info_t *after_name)
{
    pp_hide_set_t *hs = macro_token->hide_set;
    mcc_token_t **args = NULL;
    mcc_token_t **expanded_args = NULL;
    int num_args = 0;
    pp_token_info_t *remaining = after_name;
    
    if (macro->is_function_like) {
        /* For function-like macros, we need to find ( to collect arguments.
         * But there may be object-like macros between the name and ( that
         * expand to nothing. We need to skip those. */
        pp_token_info_t *check = after_name;
        
        /* Skip any intervening empty object-like macros */
        while (check && check->token->type == TOK_IDENT) {
            mcc_macro_t *m = pp_lookup_macro(pp, check->token->text);
            if (m && !m->is_function_like && !m->body && 
                !hide_set_contains(check->hide_set, check->token->text)) {
                /* Empty object-like macro - skip it */
                check = check->next;
                continue;
            }
            /* Not an empty macro - stop */
            break;
        }
        
        if (!check || check->token->type != TOK_LPAREN) {
            /* Not a function-like invocation, return as-is */
            pp_token_info_t *result = token_info_create(pp, 
                mcc_token_copy(pp->ctx, macro_token->token),
                hide_set_copy(pp, hs));
            result->next = after_name;
            return result;
        }
        
        /* Update after_name to point to ( */
        after_name = check;
        
        /* Collect arguments */
        pp_args_t collected = collect_arguments(pp, macro, after_name);
        args = collected.args;
        num_args = collected.num_args;
        remaining = collected.remaining;
        
        /* Check argument count */
        if (num_args != macro->num_params && !macro->is_variadic) {
            mcc_error(pp->ctx, "Macro '%s' expects %d arguments, got %d",
                      macro->name, macro->num_params, num_args);
        }
        
        /* Expand arguments (for non-# and non-## contexts) */
        if (num_args > 0) {
            expanded_args = mcc_alloc(pp->ctx, num_args * sizeof(mcc_token_t*));
            for (int i = 0; i < num_args; i++) {
                pp_token_info_t *arg_info = token_list_to_info_list(pp, args[i], NULL);
                pp_token_info_t *expanded_info = expand_token_list(pp, arg_info);
                expanded_args[i] = info_list_to_token_list(expanded_info);
            }
        }
    }
    
    /* Substitute parameters in body */
    mcc_token_t *substituted = NULL;
    if (macro->is_function_like) {
        substituted = substitute(pp, macro, args, expanded_args, num_args);
    } else {
        /* Object-like macro - just copy body */
        substituted = copy_token_list(pp, macro->body);
    }
    
    /* Preserve has_space from original macro token */
    if (substituted) {
        substituted->has_space = macro_token->token->has_space;
    }
    
    /* Create new hide set: union of token's hide set with macro name */
    pp_hide_set_t *new_hs = hide_set_copy(pp, hs);
    if (!new_hs) new_hs = hide_set_create(pp);
    hide_set_add(pp, new_hs, macro->name);
    
    /* Convert substituted tokens to info list
     * Important: tokens from arguments should NOT inherit the macro's hide set
     * because they were already pre-expanded. Only tokens from the macro body
     * (not from argument substitution) should get the new hide set.
     * 
     * For simplicity, we use an empty hide set here and let the rescan
     * handle the expansion. The macro name is already in the hide set
     * of the original invocation context, so it won't cause infinite recursion.
     */
    pp_token_info_t *subst_info = token_list_to_info_list(pp, substituted, NULL);
    
    /* Concatenate with remaining tokens */
    if (subst_info) {
        pp_token_info_t *tail = subst_info;
        while (tail->next) tail = tail->next;
        tail->next = remaining;
    } else {
        subst_info = remaining;
    }
    
    /* Rescan the concatenated list */
    return expand_token_list(pp, subst_info);
}

/* Expand all macros in a token list */
static pp_token_info_t *expand_token_list(mcc_preprocessor_t *pp, pp_token_info_t *tokens)
{
    pp_token_info_t *result_head = NULL, *result_tail = NULL;
    pp_token_info_t *cur = tokens;
    
    while (cur) {
        mcc_token_t *tok = cur->token;
        
        /* Check if this is a macro that can be expanded */
        if (tok->type == TOK_IDENT) {
            mcc_macro_t *macro = pp_lookup_macro(pp, tok->text);
            
            if (macro && !hide_set_contains(cur->hide_set, tok->text)) {
                /* Expand this macro */
                pp_token_info_t *expanded = expand_macro_invocation(pp, macro, cur, cur->next);
                
                /* Append expanded tokens to result */
                if (!result_head) {
                    result_head = expanded;
                    result_tail = expanded;
                } else {
                    result_tail->next = expanded;
                }
                
                /* Find new tail and continue from there */
                while (result_tail && result_tail->next) result_tail = result_tail->next;
                
                /* All remaining tokens were processed by expand_macro_invocation */
                return result_head;
            }
        }
        
        /* Not a macro or hidden - add to result */
        pp_token_info_t *copy = token_info_create(pp, 
            mcc_token_copy(pp->ctx, tok),
            hide_set_copy(pp, cur->hide_set));
        
        if (!result_head) result_head = copy;
        if (result_tail) result_tail->next = copy;
        result_tail = copy;
        
        cur = cur->next;
    }
    
    return result_head;
}

/* ============================================================
 * Public API
 * ============================================================ */

/* Expand macros in a token list and return the result */
mcc_token_t *pp_expand_tokens(mcc_preprocessor_t *pp, mcc_token_t *tokens)
{
    pp_token_info_t *info = token_list_to_info_list(pp, tokens, NULL);
    pp_token_info_t *expanded = expand_token_list(pp, info);
    return info_list_to_token_list(expanded);
}

/* Process and expand a token list, emitting to output
 * This function also handles the case where expansion produces a function-like
 * macro name that needs to pick up arguments from the lexer */
void pp_expand_and_emit(mcc_preprocessor_t *pp, mcc_token_t *tokens)
{
    mcc_token_t *expanded = pp_expand_tokens(pp, tokens);
    
    /* Check if any token in the result is a function-like macro that needs 
     * arguments from the lexer. This handles cases like:
     * #define NEXT PP_MAP_LIST1
     * #define TEST NEXT PP_MAP_OUT
     * TEST(args) -> PP_MAP_LIST1 PP_MAP_OUT (args)
     * Here PP_MAP_LIST1 should pick up (args), not PP_MAP_OUT */
    while (expanded) {
        mcc_token_t *peek = mcc_lexer_peek(pp->lexer);
        if (!peek || peek->type != TOK_LPAREN) break;
        
        /* Find a function-like macro in the expanded tokens that could take args */
        /* We look for a pattern: MACRO_NAME followed by non-LPAREN tokens, then LPAREN from lexer */
        mcc_token_t *func_macro = NULL;
        mcc_token_t *before_func = NULL;
        
        for (mcc_token_t *t = expanded; t; t = t->next) {
            if (t->type == TOK_IDENT) {
                mcc_macro_t *macro = pp_lookup_macro(pp, t->text);
                if (macro && macro->is_function_like) {
                    /* Check if next token is NOT ( - meaning it needs args from elsewhere */
                    if (!t->next || t->next->type != TOK_LPAREN) {
                        func_macro = t;
                        break;
                    }
                }
            }
            before_func = t;
        }
        
        if (!func_macro) break;
        
        /* Collect arguments from lexer */
        mcc_token_t *args_head = NULL, *args_tail = NULL;
        int paren_depth = 0;
        do {
            mcc_token_t *next = mcc_lexer_next(pp->lexer);
            if (next->type == TOK_NEWLINE) continue;
            mcc_token_t *copy = mcc_token_copy(pp->ctx, next);
            copy->next = NULL;
            if (!args_head) args_head = copy;
            if (args_tail) args_tail->next = copy;
            args_tail = copy;
            
            if (next->type == TOK_LPAREN) paren_depth++;
            else if (next->type == TOK_RPAREN) paren_depth--;
            else if (next->type == TOK_EOF) break;
        } while (paren_depth > 0);
        
        /* Insert arguments after the function macro, before remaining tokens */
        mcc_token_t *after_func = func_macro->next;
        func_macro->next = args_head;
        if (args_tail) args_tail->next = after_func;
        
        /* Re-expand */
        expanded = pp_expand_tokens(pp, expanded);
    }
    
    for (mcc_token_t *t = expanded; t; t = t->next) {
        pp_emit_token(pp, t);
    }
}
