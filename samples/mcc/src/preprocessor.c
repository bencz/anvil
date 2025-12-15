/*
 * MCC - Micro C Compiler
 * Preprocessor implementation
 */

#include "mcc.h"
#include <time.h>

#define MACRO_TABLE_SIZE 1024

static unsigned hash_string(const char *s)
{
    unsigned h = 0;
    while (*s) {
        h = h * 31 + (unsigned char)*s++;
    }
    return h;
}

mcc_preprocessor_t *mcc_preprocessor_create(mcc_context_t *ctx)
{
    mcc_preprocessor_t *pp = mcc_alloc(ctx, sizeof(mcc_preprocessor_t));
    if (!pp) return NULL;
    
    pp->ctx = ctx;
    pp->lexer = mcc_lexer_create(ctx);
    
    pp->macro_table_size = MACRO_TABLE_SIZE;
    pp->macros = mcc_alloc(ctx, MACRO_TABLE_SIZE * sizeof(mcc_macro_t*));
    
    return pp;
}

void mcc_preprocessor_destroy(mcc_preprocessor_t *pp)
{
    (void)pp; /* Arena allocated */
}

void mcc_preprocessor_add_include_path(mcc_preprocessor_t *pp, const char *path)
{
    size_t n = pp->num_include_paths;
    const char **new_paths = mcc_realloc(pp->ctx, (void*)pp->include_paths,
                                          n * sizeof(char*), (n + 1) * sizeof(char*));
    new_paths[n] = mcc_strdup(pp->ctx, path);
    pp->include_paths = new_paths;
    pp->num_include_paths = n + 1;
}

static mcc_macro_t *pp_lookup_macro(mcc_preprocessor_t *pp, const char *name)
{
    unsigned h = hash_string(name) % pp->macro_table_size;
    for (mcc_macro_t *m = pp->macros[h]; m; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

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
    unsigned h = hash_string(name) % pp->macro_table_size;
    macro->next = pp->macros[h];
    pp->macros[h] = macro;
}

void mcc_preprocessor_undef(mcc_preprocessor_t *pp, const char *name)
{
    unsigned h = hash_string(name) % pp->macro_table_size;
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

void mcc_preprocessor_define_builtins(mcc_preprocessor_t *pp)
{
    /* Standard predefined macros */
    mcc_preprocessor_define(pp, "__STDC__", "1");
    mcc_preprocessor_define(pp, "__STDC_VERSION__", "199409L");
    mcc_preprocessor_define(pp, "__STDC_HOSTED__", "1");
    
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

/* Token output helpers */
static void pp_emit_token(mcc_preprocessor_t *pp, mcc_token_t *tok)
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

/* Check if macro is currently being expanded (to prevent recursion) */
static bool pp_is_expanding(mcc_preprocessor_t *pp, const char *name)
{
    for (size_t i = 0; i < pp->num_expanding; i++) {
        if (strcmp(pp->expanding_macros[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void pp_push_expanding(mcc_preprocessor_t *pp, const char *name)
{
    size_t n = pp->num_expanding;
    const char **new_stack = mcc_realloc(pp->ctx, (void*)pp->expanding_macros,
                                          n * sizeof(char*), (n + 1) * sizeof(char*));
    new_stack[n] = name;
    pp->expanding_macros = new_stack;
    pp->num_expanding = n + 1;
}

static void pp_pop_expanding(mcc_preprocessor_t *pp)
{
    if (pp->num_expanding > 0) {
        pp->num_expanding--;
    }
}

/* Forward declarations */
static void pp_process_directive(mcc_preprocessor_t *pp);
static void pp_expand_macro(mcc_preprocessor_t *pp, mcc_macro_t *macro);

/* Read tokens from current lexer, handling directives */
static mcc_token_t *pp_next_token(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    
    /* Skip newlines in normal mode */
    while (tok->type == TOK_NEWLINE) {
        tok = mcc_lexer_next(pp->lexer);
    }
    
    return tok;
}

/* Process a single token, expanding macros */
static void pp_process_token(mcc_preprocessor_t *pp, mcc_token_t *tok)
{
    /* Check for macro expansion */
    if (tok->type == TOK_IDENT && !pp->skip_mode) {
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

/* Expand a macro */
static void pp_expand_macro(mcc_preprocessor_t *pp, mcc_macro_t *macro)
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
        
        /* Substitute arguments in body */
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
                    /* Substitute argument */
                    for (mcc_token_t *arg_tok = args[param_idx]; arg_tok; arg_tok = arg_tok->next) {
                        pp_process_token(pp, arg_tok);
                    }
                    continue;
                }
            }
            
            pp_process_token(pp, body_tok);
        }
        
    } else {
        /* Object-like macro */
        for (mcc_token_t *body_tok = macro->body; body_tok; body_tok = body_tok->next) {
            pp_process_token(pp, body_tok);
        }
    }
    
    pp_pop_expanding(pp);
}

/* Evaluate preprocessor constant expression */
static int64_t pp_eval_expr(mcc_preprocessor_t *pp);

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
        left = left && pp_eval_bitor(pp);
    }
    
    return left;
}

static int64_t pp_eval_logor(mcc_preprocessor_t *pp)
{
    int64_t left = pp_eval_logand(pp);
    
    while (mcc_lexer_peek(pp->lexer)->type == TOK_OR) {
        mcc_lexer_next(pp->lexer);
        left = left || pp_eval_logand(pp);
    }
    
    return left;
}

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

static int64_t pp_eval_expr(mcc_preprocessor_t *pp)
{
    return pp_eval_ternary(pp);
}

/* Skip to end of line */
static void pp_skip_line(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok;
    while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_NEWLINE &&
           tok->type != TOK_EOF) {
        /* Skip */
    }
}

/* Process #define directive */
static void pp_process_define(mcc_preprocessor_t *pp)
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
    unsigned h = hash_string(name) % pp->macro_table_size;
    macro->next = pp->macros[h];
    pp->macros[h] = macro;
}

/* Process #include directive */
static void pp_process_include(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    
    const char *filename = NULL;
    bool is_system = false;
    
    if (tok->type == TOK_STRING_LIT) {
        filename = tok->literal.string_val.value;
    } else if (tok->type == TOK_LT) {
        /* <filename> */
        is_system = true;
        char buf[256];
        size_t len = 0;
        
        while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_GT &&
               tok->type != TOK_NEWLINE && tok->type != TOK_EOF) {
            const char *text = mcc_token_to_string(tok);
            size_t tlen = strlen(text);
            if (len + tlen < sizeof(buf) - 1) {
                memcpy(buf + len, text, tlen);
                len += tlen;
            }
        }
        buf[len] = '\0';
        filename = mcc_strdup(pp->ctx, buf);
    } else {
        mcc_error(pp->ctx, "Expected filename after #include");
        pp_skip_line(pp);
        return;
    }
    
    pp_skip_line(pp);
    
    /* Check include depth */
    if (pp->include_depth >= MCC_MAX_INCLUDE_DEPTH) {
        mcc_error(pp->ctx, "Include depth limit exceeded");
        return;
    }
    
    /* Try to find and open file */
    char path[1024];
    FILE *f = NULL;
    
    if (!is_system && filename[0] != '/') {
        /* Try relative to current file */
        const char *cur_file = pp->lexer->filename;
        if (cur_file) {
            const char *slash = strrchr(cur_file, '/');
            if (slash) {
                size_t dir_len = slash - cur_file + 1;
                memcpy(path, cur_file, dir_len);
                strcpy(path + dir_len, filename);
                f = fopen(path, "r");
            }
        }
    }
    
    /* Try include paths */
    if (!f) {
        for (size_t i = 0; i < pp->num_include_paths && !f; i++) {
            snprintf(path, sizeof(path), "%s/%s", pp->include_paths[i], filename);
            f = fopen(path, "r");
        }
    }
    
    /* Try filename directly */
    if (!f) {
        f = fopen(filename, "r");
        if (f) strcpy(path, filename);
    }
    
    if (!f) {
        mcc_error(pp->ctx, "Cannot find include file: %s", filename);
        return;
    }
    
    /* Read file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = mcc_alloc(pp->ctx, size + 1);
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    /* Save current lexer state */
    mcc_include_file_t *inc = mcc_alloc(pp->ctx, sizeof(mcc_include_file_t));
    inc->filename = pp->lexer->filename;
    inc->content = pp->lexer->source;
    inc->pos = pp->lexer->source + pp->lexer->pos;
    inc->line = pp->lexer->line;
    inc->column = pp->lexer->column;
    inc->next = pp->include_stack;
    pp->include_stack = inc;
    pp->include_depth++;
    
    /* Initialize lexer with new file */
    mcc_lexer_init_string(pp->lexer, content, mcc_strdup(pp->ctx, path));
}

/* Process conditional directive */
static void pp_push_cond(mcc_preprocessor_t *pp, bool condition, mcc_location_t loc)
{
    mcc_cond_stack_t *cond = mcc_alloc(pp->ctx, sizeof(mcc_cond_stack_t));
    cond->condition = condition;
    cond->any_true = condition;
    cond->location = loc;
    cond->next = pp->cond_stack;
    pp->cond_stack = cond;
    
    /* Update skip mode */
    pp->skip_mode = !condition;
    for (mcc_cond_stack_t *c = cond->next; c; c = c->next) {
        if (!c->condition) {
            pp->skip_mode = true;
            break;
        }
    }
}

static void pp_pop_cond(mcc_preprocessor_t *pp)
{
    if (!pp->cond_stack) {
        mcc_error(pp->ctx, "Unmatched #endif");
        return;
    }
    
    pp->cond_stack = pp->cond_stack->next;
    
    /* Update skip mode */
    pp->skip_mode = false;
    for (mcc_cond_stack_t *c = pp->cond_stack; c; c = c->next) {
        if (!c->condition) {
            pp->skip_mode = true;
            break;
        }
    }
}

/* Process preprocessor directive */
static void pp_process_directive(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    
    /* Empty directive */
    if (tok->type == TOK_NEWLINE) {
        return;
    }
    
    /* Get directive name - can be identifier or keyword (else, if) */
    const char *directive = NULL;
    if (tok->type == TOK_IDENT) {
        directive = tok->text;
    } else if (tok->type == TOK_ELSE) {
        directive = "else";
    } else if (tok->type == TOK_IF) {
        directive = "if";
    } else {
        mcc_error(pp->ctx, "Expected directive name after #");
        pp_skip_line(pp);
        return;
    }
    mcc_location_t loc = tok->location;
    
    /* Conditional directives are always processed */
    if (strcmp(directive, "ifdef") == 0) {
        tok = mcc_lexer_next(pp->lexer);
        if (tok->type != TOK_IDENT) {
            mcc_error(pp->ctx, "Expected identifier after #ifdef");
            pp_skip_line(pp);
            return;
        }
        bool defined = mcc_preprocessor_is_defined(pp, tok->text);
        pp_push_cond(pp, pp->skip_mode ? false : defined, loc);
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "ifndef") == 0) {
        tok = mcc_lexer_next(pp->lexer);
        if (tok->type != TOK_IDENT) {
            mcc_error(pp->ctx, "Expected identifier after #ifndef");
            pp_skip_line(pp);
            return;
        }
        bool defined = mcc_preprocessor_is_defined(pp, tok->text);
        pp_push_cond(pp, pp->skip_mode ? false : !defined, loc);
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "if") == 0) {
        int64_t val = 0;
        if (!pp->skip_mode) {
            val = pp_eval_expr(pp);
        }
        pp_push_cond(pp, pp->skip_mode ? false : (val != 0), loc);
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "elif") == 0) {
        if (!pp->cond_stack) {
            mcc_error(pp->ctx, "#elif without #if");
            pp_skip_line(pp);
            return;
        }
        if (pp->cond_stack->has_else) {
            mcc_error(pp->ctx, "#elif after #else");
            pp_skip_line(pp);
            return;
        }
        
        if (pp->cond_stack->any_true) {
            pp->cond_stack->condition = false;
        } else {
            int64_t val = pp_eval_expr(pp);
            pp->cond_stack->condition = (val != 0);
            if (pp->cond_stack->condition) {
                pp->cond_stack->any_true = true;
            }
        }
        
        /* Update skip mode */
        pp->skip_mode = !pp->cond_stack->condition;
        for (mcc_cond_stack_t *c = pp->cond_stack->next; c; c = c->next) {
            if (!c->condition) {
                pp->skip_mode = true;
                break;
            }
        }
        
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "else") == 0) {
        if (!pp->cond_stack) {
            mcc_error(pp->ctx, "#else without #if");
            pp_skip_line(pp);
            return;
        }
        if (pp->cond_stack->has_else) {
            mcc_error(pp->ctx, "Duplicate #else");
            pp_skip_line(pp);
            return;
        }
        
        pp->cond_stack->has_else = true;
        pp->cond_stack->condition = !pp->cond_stack->any_true;
        
        /* Update skip mode */
        pp->skip_mode = !pp->cond_stack->condition;
        for (mcc_cond_stack_t *c = pp->cond_stack->next; c; c = c->next) {
            if (!c->condition) {
                pp->skip_mode = true;
                break;
            }
        }
        
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "endif") == 0) {
        pp_pop_cond(pp);
        pp_skip_line(pp);
        return;
    }
    
    /* Other directives are skipped in skip mode */
    if (pp->skip_mode) {
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "define") == 0) {
        pp_process_define(pp);
        return;
    }
    
    if (strcmp(directive, "undef") == 0) {
        tok = mcc_lexer_next(pp->lexer);
        if (tok->type != TOK_IDENT) {
            mcc_error(pp->ctx, "Expected identifier after #undef");
        } else {
            mcc_preprocessor_undef(pp, tok->text);
        }
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "include") == 0) {
        pp_process_include(pp);
        return;
    }
    
    if (strcmp(directive, "error") == 0) {
        char buf[256];
        size_t len = 0;
        while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_NEWLINE &&
               tok->type != TOK_EOF) {
            if (tok->has_space && len > 0) buf[len++] = ' ';
            const char *text = mcc_token_to_string(tok);
            size_t tlen = strlen(text);
            if (len + tlen < sizeof(buf) - 1) {
                memcpy(buf + len, text, tlen);
                len += tlen;
            }
        }
        buf[len] = '\0';
        mcc_error_at(pp->ctx, loc, "#error %s", buf);
        return;
    }
    
    if (strcmp(directive, "warning") == 0) {
        char buf[256];
        size_t len = 0;
        while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_NEWLINE &&
               tok->type != TOK_EOF) {
            if (tok->has_space && len > 0) buf[len++] = ' ';
            const char *text = mcc_token_to_string(tok);
            size_t tlen = strlen(text);
            if (len + tlen < sizeof(buf) - 1) {
                memcpy(buf + len, text, tlen);
                len += tlen;
            }
        }
        buf[len] = '\0';
        mcc_warning_at(pp->ctx, loc, "#warning %s", buf);
        return;
    }
    
    if (strcmp(directive, "line") == 0) {
        tok = mcc_lexer_next(pp->lexer);
        if (tok->type == TOK_INT_LIT) {
            pp->lexer->line = (int)tok->literal.int_val.value;
            tok = mcc_lexer_peek(pp->lexer);
            if (tok->type == TOK_STRING_LIT) {
                mcc_lexer_next(pp->lexer);
                pp->lexer->filename = tok->literal.string_val.value;
            }
        }
        pp_skip_line(pp);
        return;
    }
    
    if (strcmp(directive, "pragma") == 0) {
        /* Ignore pragmas for now */
        pp_skip_line(pp);
        return;
    }
    
    mcc_warning(pp->ctx, "Unknown preprocessor directive: #%s", directive);
    pp_skip_line(pp);
}

/* Main preprocessing loop */
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
            if (pp->include_stack) {
                mcc_include_file_t *inc = pp->include_stack;
                pp->include_stack = inc->next;
                pp->include_depth--;
                /* Restore lexer state */
                pp->lexer->source = inc->content;
                pp->lexer->source_len = strlen(inc->content);
                pp->lexer->pos = inc->pos - inc->content;
                pp->lexer->filename = inc->filename;
                pp->lexer->line = inc->line;
                pp->lexer->column = inc->column;
                pp->lexer->current = inc->pos[0];
                pp->lexer->peek_token = NULL;
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
