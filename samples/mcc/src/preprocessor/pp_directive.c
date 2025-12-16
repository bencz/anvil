/*
 * MCC - Micro C Compiler
 * Preprocessor - Directive Processing
 * 
 * This file handles processing of preprocessor directives
 * (#if, #ifdef, #ifndef, #elif, #else, #endif, #error, #warning, etc.)
 */

#include "pp_internal.h"

/* ============================================================
 * Utility Functions
 * ============================================================ */

void pp_skip_line(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok;
    while ((tok = mcc_lexer_next(pp->lexer))->type != TOK_NEWLINE &&
           tok->type != TOK_EOF) {
        /* Skip */
    }
}

/* ============================================================
 * Conditional Stack Management
 * ============================================================ */

void pp_update_skip_mode(mcc_preprocessor_t *pp)
{
    pp->skip_mode = false;
    for (mcc_cond_stack_t *c = pp->cond_stack; c; c = c->next) {
        if (!c->condition) {
            pp->skip_mode = true;
            break;
        }
    }
}

void pp_push_cond(mcc_preprocessor_t *pp, bool condition, mcc_location_t loc)
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

void pp_pop_cond(mcc_preprocessor_t *pp)
{
    if (!pp->cond_stack) {
        mcc_error(pp->ctx, "Unmatched #endif");
        return;
    }
    
    pp->cond_stack = pp->cond_stack->next;
    pp_update_skip_mode(pp);
}

/* ============================================================
 * Directive Handlers
 * ============================================================ */

/* Process #ifdef directive */
static void pp_process_ifdef(mcc_preprocessor_t *pp, mcc_location_t loc)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #ifdef");
        pp_skip_line(pp);
        return;
    }
    bool defined = mcc_preprocessor_is_defined(pp, tok->text);
    pp_push_cond(pp, pp->skip_mode ? false : defined, loc);
    pp_skip_line(pp);
}

/* Process #ifndef directive */
static void pp_process_ifndef(mcc_preprocessor_t *pp, mcc_location_t loc)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #ifndef");
        pp_skip_line(pp);
        return;
    }
    bool defined = mcc_preprocessor_is_defined(pp, tok->text);
    pp_push_cond(pp, pp->skip_mode ? false : !defined, loc);
    pp_skip_line(pp);
}

/* Process #if directive */
static void pp_process_if(mcc_preprocessor_t *pp, mcc_location_t loc)
{
    int64_t val = 0;
    if (!pp->skip_mode) {
        val = pp_eval_expr(pp);
    }
    pp_push_cond(pp, pp->skip_mode ? false : (val != 0), loc);
    pp_skip_line(pp);
}

/* Process #elif directive */
static void pp_process_elif(mcc_preprocessor_t *pp)
{
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
    
    pp_update_skip_mode(pp);
    pp_skip_line(pp);
}

/* Process #elifdef directive (C23) */
static void pp_process_elifdef(mcc_preprocessor_t *pp)
{
    if (!pp_has_elifdef(pp)) {
        mcc_error(pp->ctx, "#elifdef requires C23 or later (-std=c23)");
        pp_skip_line(pp);
        return;
    }
    
    if (!pp->cond_stack) {
        mcc_error(pp->ctx, "#elifdef without #if");
        pp_skip_line(pp);
        return;
    }
    if (pp->cond_stack->has_else) {
        mcc_error(pp->ctx, "#elifdef after #else");
        pp_skip_line(pp);
        return;
    }
    
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #elifdef");
        pp_skip_line(pp);
        return;
    }
    
    if (pp->cond_stack->any_true) {
        pp->cond_stack->condition = false;
    } else {
        bool defined = mcc_preprocessor_is_defined(pp, tok->text);
        pp->cond_stack->condition = defined;
        if (pp->cond_stack->condition) {
            pp->cond_stack->any_true = true;
        }
    }
    
    pp_update_skip_mode(pp);
    pp_skip_line(pp);
}

/* Process #elifndef directive (C23) */
static void pp_process_elifndef(mcc_preprocessor_t *pp)
{
    if (!pp_has_elifdef(pp)) {
        mcc_error(pp->ctx, "#elifndef requires C23 or later (-std=c23)");
        pp_skip_line(pp);
        return;
    }
    
    if (!pp->cond_stack) {
        mcc_error(pp->ctx, "#elifndef without #if");
        pp_skip_line(pp);
        return;
    }
    if (pp->cond_stack->has_else) {
        mcc_error(pp->ctx, "#elifndef after #else");
        pp_skip_line(pp);
        return;
    }
    
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #elifndef");
        pp_skip_line(pp);
        return;
    }
    
    if (pp->cond_stack->any_true) {
        pp->cond_stack->condition = false;
    } else {
        bool defined = mcc_preprocessor_is_defined(pp, tok->text);
        pp->cond_stack->condition = !defined;
        if (pp->cond_stack->condition) {
            pp->cond_stack->any_true = true;
        }
    }
    
    pp_update_skip_mode(pp);
    pp_skip_line(pp);
}

/* Process #else directive */
static void pp_process_else(mcc_preprocessor_t *pp)
{
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
    
    pp_update_skip_mode(pp);
    pp_skip_line(pp);
}

/* Process #endif directive */
static void pp_process_endif(mcc_preprocessor_t *pp)
{
    pp_pop_cond(pp);
    pp_skip_line(pp);
}

/* Process #undef directive */
static void pp_process_undef(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type != TOK_IDENT) {
        mcc_error(pp->ctx, "Expected identifier after #undef");
    } else {
        mcc_preprocessor_undef(pp, tok->text);
    }
    pp_skip_line(pp);
}

/* Process #error directive */
static void pp_process_error(mcc_preprocessor_t *pp, mcc_location_t loc)
{
    char buf[256];
    size_t len = 0;
    mcc_token_t *tok;
    
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
}

/* Process #warning directive (GNU extension) */
static void pp_process_warning(mcc_preprocessor_t *pp, mcc_location_t loc)
{
    if (!pp_has_warning_directive(pp)) {
        mcc_warning(pp->ctx, "#warning is a GNU extension");
    }
    
    char buf[256];
    size_t len = 0;
    mcc_token_t *tok;
    
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
}

/* Process #line directive */
static void pp_process_line(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    if (tok->type == TOK_INT_LIT) {
        pp->lexer->line = (int)tok->literal.int_val.value;
        tok = mcc_lexer_peek(pp->lexer);
        if (tok->type == TOK_STRING_LIT) {
            mcc_lexer_next(pp->lexer);
            pp->lexer->filename = tok->literal.string_val.value;
        }
    }
    pp_skip_line(pp);
}

/* Process #pragma directive */
static void pp_process_pragma(mcc_preprocessor_t *pp)
{
    /* Check for specific pragmas */
    mcc_token_t *tok = mcc_lexer_peek(pp->lexer);
    
    if (tok->type == TOK_IDENT) {
        if (strcmp(tok->text, "once") == 0) {
            /* #pragma once - mark file as include-once */
            /* TODO: Implement include guard tracking */
            mcc_lexer_next(pp->lexer);
        }
        /* Other pragmas can be added here */
    }
    
    /* Ignore unknown pragmas */
    pp_skip_line(pp);
}

/* ============================================================
 * Main Directive Processor
 * ============================================================ */

void pp_process_directive(mcc_preprocessor_t *pp)
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
        pp_process_ifdef(pp, loc);
        return;
    }
    
    if (strcmp(directive, "ifndef") == 0) {
        pp_process_ifndef(pp, loc);
        return;
    }
    
    if (strcmp(directive, "if") == 0) {
        pp_process_if(pp, loc);
        return;
    }
    
    if (strcmp(directive, "elif") == 0) {
        pp_process_elif(pp);
        return;
    }
    
    if (strcmp(directive, "elifdef") == 0) {
        pp_process_elifdef(pp);
        return;
    }
    
    if (strcmp(directive, "elifndef") == 0) {
        pp_process_elifndef(pp);
        return;
    }
    
    if (strcmp(directive, "else") == 0) {
        pp_process_else(pp);
        return;
    }
    
    if (strcmp(directive, "endif") == 0) {
        pp_process_endif(pp);
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
        pp_process_undef(pp);
        return;
    }
    
    if (strcmp(directive, "include") == 0) {
        pp_process_include(pp);
        return;
    }
    
    if (strcmp(directive, "include_next") == 0) {
        if (!pp_has_include_next(pp)) {
            mcc_warning(pp->ctx, "#include_next is a GNU extension");
        }
        /* TODO: Implement include_next properly */
        pp_process_include(pp);
        return;
    }
    
    if (strcmp(directive, "error") == 0) {
        pp_process_error(pp, loc);
        return;
    }
    
    if (strcmp(directive, "warning") == 0) {
        pp_process_warning(pp, loc);
        return;
    }
    
    if (strcmp(directive, "line") == 0) {
        pp_process_line(pp);
        return;
    }
    
    if (strcmp(directive, "pragma") == 0) {
        pp_process_pragma(pp);
        return;
    }
    
    mcc_warning(pp->ctx, "Unknown preprocessor directive: #%s", directive);
    pp_skip_line(pp);
}
