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
