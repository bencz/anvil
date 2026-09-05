/*
 * MCC - Micro C Compiler
 * Preprocessor - Include File Processing
 * 
 * This file handles #include directive processing and
 * include stack management.
 */

#include "pp_internal.h"
#include "platform/host.h"
#include "target.h"
#include <limits.h>

/* ============================================================
 * Include Stack Management
 * ============================================================ */

bool pp_push_include(mcc_preprocessor_t *pp)
{
    if (!pp || !pp->lexer || pp->include_depth == INT_MAX) {
        if (pp) mcc_fatal(pp->ctx, "include stack overflow");
        return false;
    }
    mcc_include_file_t *inc = mcc_alloc(pp->ctx, sizeof(mcc_include_file_t));
    if (!inc) return false;
    inc->filename = pp->lexer->filename;
    inc->content = pp->lexer->source;
    inc->pos = pp->lexer->source + pp->lexer->pos;
    inc->line = pp->lexer->line;
    inc->column = pp->lexer->column;
    inc->at_bol = pp->lexer->at_bol;
    inc->next = pp->include_stack;
    pp->include_stack = inc;
    pp->include_depth++;
    return true;
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
    pp->lexer->at_bol = true;  /* After include, we're at beginning of line */
    pp->lexer->peek_token = NULL;
    
    return true;
}

/* ============================================================
 * File Search
 * ============================================================ */

/* Try to open a file at the given path */
static FILE *pp_try_open_file(const char *path)
{
    /* ftell below measures bytes. Text mode translates CRLF on Windows and
     * would make a subsequent fread appear to be a short read. */
    return fopen(path, "rb");
}

/* Search for include file in various locations */
FILE *pp_find_include_file(mcc_preprocessor_t *pp, const char *filename,
                                   bool is_system, char *resolved_path, size_t path_size)
{
    FILE *f = NULL;
    
    /* For non-system includes, try relative to current file first */
    if (!is_system && filename[0] != '/') {
        const char *cur_file = pp->lexer->filename;
        if (cur_file) {
            const char *slash = mcc_host.last_path_separator(cur_file);

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

    /* #pragma once tracking: if this path has already been include-once'd,
     * skip the body entirely. pp_find_include_file resolved the full path
     * to `path`, so comparisons work regardless of the spelling in the
     * #include directive. */
    for (size_t i = 0; i < pp->num_pragma_once; i++) {
        if (strcmp(pp->pragma_once_files[i], path) == 0) {
            fclose(f);
            return;
        }
    }

    /* Read file contents */
    if (fseek(f, 0, SEEK_END) != 0) {
        mcc_error(pp->ctx, "Failed to seek include file: %s", path);
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0 || (uintmax_t)size >= (uintmax_t)SIZE_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        mcc_error(pp->ctx, "Invalid include file size: %s", path);
        fclose(f);
        return;
    }

    size_t content_size = (size_t)size;
    char *content = mcc_alloc(pp->ctx, content_size + 1);
    if (!content) {
        fclose(f);
        return;
    }
    if (fread(content, 1, content_size, f) != content_size) {
        mcc_error(pp->ctx, "Failed to read include file: %s", path);
        fclose(f);
        return;
    }
    content[content_size] = '\0';
    fclose(f);

    /* Save current lexer state */
    if (!pp_push_include(pp)) return;

    /* Initialize lexer with new file */
    char *stored_path = mcc_strdup(pp->ctx, path);
    if (!stored_path) return;
    mcc_lexer_init_string(pp->lexer, content, stored_path);
}

/* Register the current file as #pragma once. Called from pp_process_pragma. */
void pp_mark_pragma_once(mcc_preprocessor_t *pp)
{
    const char *path = pp->lexer ? pp->lexer->filename : NULL;
    if (!path) return;

    /* Already registered? */
    for (size_t i = 0; i < pp->num_pragma_once; i++) {
        if (strcmp(pp->pragma_once_files[i], path) == 0) return;
    }

    if (pp->num_pragma_once >= pp->cap_pragma_once) {
        if (pp->cap_pragma_once > SIZE_MAX / 2) {
            mcc_fatal(pp->ctx, "pragma-once table capacity overflow");
            return;
        }
        size_t ncap = pp->cap_pragma_once ? pp->cap_pragma_once * 2 : 8;
        void *grown = mcc_realloc_array(pp->ctx, pp->pragma_once_files,
            pp->cap_pragma_once, ncap, sizeof(*pp->pragma_once_files));
        if (!grown) return;
        pp->pragma_once_files = grown;
        pp->cap_pragma_once = ncap;
    }
    char *stored_path = mcc_strdup(pp->ctx, path);
    if (!stored_path) return;
    pp->pragma_once_files[pp->num_pragma_once++] = stored_path;
}

/* ============================================================
 * Public API
 * ============================================================ */

static void append_include_path(mcc_preprocessor_t *pp, const char *path)
{
    if (!pp || !path || pp->num_include_paths == SIZE_MAX) {
        if (pp) mcc_fatal(pp->ctx, "include path table capacity overflow");
        return;
    }
    size_t n = pp->num_include_paths;
    const char **new_paths = mcc_realloc_array(pp->ctx,
        (void *)pp->include_paths, n, n + 1, sizeof(*new_paths));
    if (!new_paths) return;
    char *stored_path = mcc_strdup(pp->ctx, path);
    if (!stored_path) return;
    new_paths[n] = stored_path;
    pp->include_paths = new_paths;
    pp->num_include_paths = n + 1;
}

void mcc_preprocessor_add_include_path(mcc_preprocessor_t *pp, const char *path)
{
    if (!pp || !path)
        return;

    const char *subdirectory = mcc_target_model(pp->ctx->options.arch)->include_subdirectory;
    if (subdirectory)
    {
        size_t prefix = strlen(path);
        size_t suffix = strlen(subdirectory);
        if (prefix > SIZE_MAX - suffix - 2)
        {
            mcc_fatal(pp->ctx, "target include path is too long");
            return;
        }

        size_t length = prefix + suffix + 2;
        char *specialized = mcc_alloc(pp->ctx, length);
        if (!specialized)
            return;

        snprintf(specialized, length, "%s/%s", path, subdirectory);
        append_include_path(pp, specialized);
    }

    append_include_path(pp, path);
}
