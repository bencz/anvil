/*
 * MCC - Micro C Compiler
 * Preprocessor - Include File Processing
 * 
 * This file handles #include directive processing and
 * include stack management.
 */

#include "pp_internal.h"

/* ============================================================
 * Include Stack Management
 * ============================================================ */

void pp_push_include(mcc_preprocessor_t *pp)
{
    mcc_include_file_t *inc = mcc_alloc(pp->ctx, sizeof(mcc_include_file_t));
    inc->filename = pp->lexer->filename;
    inc->content = pp->lexer->source;
    inc->pos = pp->lexer->source + pp->lexer->pos;
    inc->line = pp->lexer->line;
    inc->column = pp->lexer->column;
    inc->next = pp->include_stack;
    pp->include_stack = inc;
    pp->include_depth++;
}

bool pp_pop_include(mcc_preprocessor_t *pp)
{
    if (!pp->include_stack) {
        return false;
    }
    
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
    
    return true;
}

/* ============================================================
 * File Search
 * ============================================================ */

/* Try to open a file at the given path */
static FILE *pp_try_open_file(const char *path)
{
    return fopen(path, "r");
}

/* Search for include file in various locations */
static FILE *pp_find_include_file(mcc_preprocessor_t *pp, const char *filename,
                                   bool is_system, char *resolved_path, size_t path_size)
{
    FILE *f = NULL;
    
    /* For non-system includes, try relative to current file first */
    if (!is_system && filename[0] != '/') {
        const char *cur_file = pp->lexer->filename;
        if (cur_file) {
            const char *slash = strrchr(cur_file, '/');
            if (slash) {
                size_t dir_len = slash - cur_file + 1;
                if (dir_len + strlen(filename) < path_size) {
                    memcpy(resolved_path, cur_file, dir_len);
                    strcpy(resolved_path + dir_len, filename);
                    f = pp_try_open_file(resolved_path);
                    if (f) return f;
                }
            }
        }
    }
    
    /* Try include paths */
    for (size_t i = 0; i < pp->num_include_paths && !f; i++) {
        snprintf(resolved_path, path_size, "%s/%s", pp->include_paths[i], filename);
        f = pp_try_open_file(resolved_path);
        if (f) return f;
    }
    
    /* Try filename directly */
    if (!f) {
        strncpy(resolved_path, filename, path_size - 1);
        resolved_path[path_size - 1] = '\0';
        f = pp_try_open_file(resolved_path);
    }
    
    return f;
}

/* ============================================================
 * #include Processing
 * ============================================================ */

void pp_process_include(mcc_preprocessor_t *pp)
{
    mcc_token_t *tok = mcc_lexer_next(pp->lexer);
    
    const char *filename = NULL;
    bool is_system = false;
    
    if (tok->type == TOK_STRING_LIT) {
        /* "filename" */
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
        mcc_error(pp->ctx, "Include depth limit exceeded (%d)", MCC_MAX_INCLUDE_DEPTH);
        return;
    }
    
    /* Find and open file */
    char path[1024];
    FILE *f = pp_find_include_file(pp, filename, is_system, path, sizeof(path));
    
    if (!f) {
        mcc_error(pp->ctx, "Cannot find include file: %s", filename);
        return;
    }
    
    /* Read file contents */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = mcc_alloc(pp->ctx, size + 1);
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    /* Save current lexer state */
    pp_push_include(pp);
    
    /* Initialize lexer with new file */
    mcc_lexer_init_string(pp->lexer, content, mcc_strdup(pp->ctx, path));
}

/* ============================================================
 * Public API
 * ============================================================ */

void mcc_preprocessor_add_include_path(mcc_preprocessor_t *pp, const char *path)
{
    size_t n = pp->num_include_paths;
    const char **new_paths = mcc_realloc(pp->ctx, (void*)pp->include_paths,
                                          n * sizeof(char*), (n + 1) * sizeof(char*));
    new_paths[n] = mcc_strdup(pp->ctx, path);
    pp->include_paths = new_paths;
    pp->num_include_paths = n + 1;
}
