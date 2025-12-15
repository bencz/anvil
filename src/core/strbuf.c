/*
 * ANVIL - String buffer implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define STRBUF_INIT_CAP 256

void anvil_strbuf_init(anvil_strbuf_t *sb)
{
    if (!sb) return;
    sb->data = malloc(STRBUF_INIT_CAP);
    sb->len = 0;
    sb->cap = STRBUF_INIT_CAP;
    if (sb->data) sb->data[0] = '\0';
}

void anvil_strbuf_destroy(anvil_strbuf_t *sb)
{
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void strbuf_grow(anvil_strbuf_t *sb, size_t needed)
{
    if (!sb || !sb->data) return;
    
    size_t new_cap = sb->cap;
    while (new_cap < sb->len + needed + 1) {
        new_cap *= 2;
    }
    
    if (new_cap > sb->cap) {
        char *new_data = realloc(sb->data, new_cap);
        if (new_data) {
            sb->data = new_data;
            sb->cap = new_cap;
        }
    }
}

void anvil_strbuf_append(anvil_strbuf_t *sb, const char *str)
{
    if (!sb || !str) return;
    
    size_t len = strlen(str);
    strbuf_grow(sb, len);
    
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
    
    strbuf_grow(sb, (size_t)needed);
    
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args_copy);
    sb->len += (size_t)needed;
    va_end(args_copy);
}

void anvil_strbuf_append_char(anvil_strbuf_t *sb, char c)
{
    if (!sb) return;
    
    strbuf_grow(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

char *anvil_strbuf_detach(anvil_strbuf_t *sb, size_t *len)
{
    if (!sb) return NULL;
    
    char *data = sb->data;
    if (len) *len = sb->len;
    
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    
    return data;
}
