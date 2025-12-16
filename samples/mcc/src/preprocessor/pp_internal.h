/*
 * MCC - Micro C Compiler
 * Preprocessor Internal Header
 * 
 * This file contains internal structures, constants, and function
 * declarations used by the preprocessor implementation.
 */

#ifndef MCC_PP_INTERNAL_H
#define MCC_PP_INTERNAL_H

#include "mcc.h"
#include <time.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define PP_MACRO_TABLE_SIZE 1024
#define PP_MAX_MACRO_ARGS   127
#define PP_MAX_EXPAND_DEPTH 256

/* ============================================================
 * Internal Function Declarations
 * ============================================================ */

/* Hash function for macro names */
unsigned pp_hash_string(const char *s);

/* ---- Macro Management (pp_macro.c) ---- */

/* Lookup a macro by name */
mcc_macro_t *pp_lookup_macro(mcc_preprocessor_t *pp, const char *name);

/* Check if macro is currently being expanded (recursion prevention) */
bool pp_is_expanding(mcc_preprocessor_t *pp, const char *name);

/* Push/pop macro expansion stack */
void pp_push_expanding(mcc_preprocessor_t *pp, const char *name);
void pp_pop_expanding(mcc_preprocessor_t *pp);

/* Expand a macro */
void pp_expand_macro(mcc_preprocessor_t *pp, mcc_macro_t *macro);

/* Process #define directive */
void pp_process_define(mcc_preprocessor_t *pp);

/* ---- Token Output (pp_token.c or preprocessor.c) ---- */

/* Emit a token to output */
void pp_emit_token(mcc_preprocessor_t *pp, mcc_token_t *tok);

/* Process a single token (with macro expansion) */
void pp_process_token(mcc_preprocessor_t *pp, mcc_token_t *tok);

/* Process a token list (with macro expansion for nested macros) */
void pp_process_token_list(mcc_preprocessor_t *pp, mcc_token_t *tokens);

/* ---- Expression Evaluation (pp_expr.c) ---- */

/* Evaluate preprocessor constant expression */
int64_t pp_eval_expr(mcc_preprocessor_t *pp);

/* ---- Conditional Compilation (pp_directive.c) ---- */

/* Push/pop conditional stack */
void pp_push_cond(mcc_preprocessor_t *pp, bool condition, mcc_location_t loc);
void pp_pop_cond(mcc_preprocessor_t *pp);

/* Update skip mode based on conditional stack */
void pp_update_skip_mode(mcc_preprocessor_t *pp);

/* ---- Include Processing (pp_include.c) ---- */

/* Process #include directive */
void pp_process_include(mcc_preprocessor_t *pp);

/* Save current lexer state to include stack */
void pp_push_include(mcc_preprocessor_t *pp);

/* Restore lexer state from include stack */
bool pp_pop_include(mcc_preprocessor_t *pp);

/* ---- Directive Processing (pp_directive.c) ---- */

/* Process a preprocessor directive */
void pp_process_directive(mcc_preprocessor_t *pp);

/* Skip to end of line */
void pp_skip_line(mcc_preprocessor_t *pp);

/* ---- Builtin Macros ---- */

/* Define standard predefined macros based on C standard */
void pp_define_standard_macros(mcc_preprocessor_t *pp);

/* ============================================================
 * C Standard Feature Checks for Preprocessor
 * ============================================================ */

/* Check if a preprocessor feature is enabled */
static inline bool pp_has_feature(mcc_preprocessor_t *pp, mcc_feature_id_t feat)
{
    return mcc_ctx_has_feature(pp->ctx, feat);
}

/* Common feature checks */
static inline bool pp_has_variadic_macros(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_VARIADIC);
}

static inline bool pp_has_line_comments(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_LINE_COMMENT);
}

static inline bool pp_has_pragma_operator(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_PRAGMA_OP);
}

static inline bool pp_has_empty_args(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_EMPTY_ARGS);
}

static inline bool pp_has_va_opt(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_VA_OPT);
}

static inline bool pp_has_elifdef(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_ELIFDEF);
}

static inline bool pp_has_stringify(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_STRINGIFY);
}

static inline bool pp_has_concat(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_CONCAT);
}

static inline bool pp_has_include_next(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_INCLUDE_NEXT);
}

static inline bool pp_has_warning_directive(mcc_preprocessor_t *pp)
{
    return pp_has_feature(pp, MCC_FEAT_PP_WARNING);
}

#endif /* MCC_PP_INTERNAL_H */
