/*
 * ANVIL - String buffer implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

#define STRBUF_INIT_CAP 256

void anvil_strbuf_init(anvil_strbuf_t *sb)
{
    if (!sb) return;
    sb->data = malloc(STRBUF_INIT_CAP);
    sb->len = 0;
    sb->cap = sb->data ? STRBUF_INIT_CAP : 0;
    sb->failed = sb->data == NULL;
    if (sb->data) sb->data[0] = '\0';
}

void anvil_strbuf_destroy(anvil_strbuf_t *sb)
{
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = false;
}

static bool strbuf_grow(anvil_strbuf_t *sb, size_t needed)
{
    if (!sb || sb->failed) return false;
    if (sb->len == SIZE_MAX || needed > SIZE_MAX - sb->len - 1) {
        sb->failed = true;
        return false;
    }

    size_t required = sb->len + needed + 1;
    if (required <= sb->cap) return true;

    size_t new_cap = sb->cap ? sb->cap : STRBUF_INIT_CAP;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }

    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) {
        sb->failed = true;
        return false;
    }

    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

void anvil_strbuf_append(anvil_strbuf_t *sb, const char *str)
{
    if (!sb || !str) return;
    
    size_t len = strlen(str);
    if (!strbuf_grow(sb, len)) return;
    
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void anvil_strbuf_appendf(anvil_strbuf_t *sb, const char *fmt, ...)
{
    if (!sb || !fmt) return;
    
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    /* Calculate needed size */
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (needed < 0) {
        va_end(args_copy);
        return;
    }
    
    if (!strbuf_grow(sb, (size_t)needed)) {
        va_end(args_copy);
        return;
    }
    
    int written = vsnprintf(sb->data + sb->len, sb->cap - sb->len,
                            fmt, args_copy);
    va_end(args_copy);
    if (written != needed) {
        sb->failed = true;
        return;
    }
    sb->len += (size_t)written;
}

void anvil_strbuf_append_char(anvil_strbuf_t *sb, char c)
{
    if (!sb) return;
    
    if (!strbuf_grow(sb, 1)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

char *anvil_strbuf_detach(anvil_strbuf_t *sb, size_t *len)
{
    if (!sb) return NULL;

    if (sb->failed) {
        free(sb->data);
        sb->data = NULL;
        sb->len = 0;
        sb->cap = 0;
        sb->failed = false;
        if (len) *len = 0;
        return NULL;
    }
    
    char *data = sb->data;
    if (len) *len = sb->len;
    
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = false;
    
    return data;
}
