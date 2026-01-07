#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

bool anvil_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

bool anvil_str_starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return false;
    size_t str_len = strlen(str);
    size_t prefix_len = strlen(prefix);
    if (prefix_len > str_len) return false;
    return strncmp(str, prefix, prefix_len) == 0;
}

bool anvil_str_ends_with(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return false;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

size_t anvil_str_hash(const char* str) {
    if (!str) return 0;
    size_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint32_t anvil_str_hash32(const char* str) {
    if (!str) return 0;
    uint32_t hash = 2166136261U;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619U;
    }
    return hash;
}

void anvil_str_builder_init(AnvilStrBuilder* sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void anvil_str_builder_free(AnvilStrBuilder* sb) {
    if (sb->data) {
        free(sb->data);
        sb->data = NULL;
    }
    sb->len = 0;
    sb->cap = 0;
}

void anvil_str_builder_clear(AnvilStrBuilder* sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void anvil_str_builder_grow(AnvilStrBuilder* sb, size_t needed) {
    size_t new_cap = sb->cap ? sb->cap : 64;
    while (new_cap < sb->len + needed + 1) new_cap *= 2;
    if (new_cap > sb->cap) {
        char* new_data = (char*)realloc(sb->data, new_cap);
        if (new_data) {
            sb->data = new_data;
            sb->cap = new_cap;
        }
    }
}

void anvil_str_builder_append(AnvilStrBuilder* sb, const char* str) {
    if (!str) return;
    size_t len = strlen(str);
    anvil_str_builder_grow(sb, len);
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void anvil_str_builder_append_char(AnvilStrBuilder* sb, char c) {
    anvil_str_builder_grow(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void anvil_str_builder_appendf(AnvilStrBuilder* sb, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed > 0) {
        anvil_str_builder_grow(sb, (size_t)needed);
        vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
        sb->len += (size_t)needed;
    }
    va_end(args);
}

void anvil_str_builder_append_int(AnvilStrBuilder* sb, int64_t val) {
    anvil_str_builder_appendf(sb, "%lld", (long long)val);
}

void anvil_str_builder_append_uint(AnvilStrBuilder* sb, uint64_t val) {
    anvil_str_builder_appendf(sb, "%llu", (unsigned long long)val);
}

void anvil_str_builder_append_hex(AnvilStrBuilder* sb, uint64_t val) {
    anvil_str_builder_appendf(sb, "0x%llx", (unsigned long long)val);
}

const char* anvil_str_builder_cstr(AnvilStrBuilder* sb) {
    if (!sb->data) {
        anvil_str_builder_grow(sb, 1);
        sb->data[0] = '\0';
    }
    return sb->data;
}

char* anvil_str_builder_take(AnvilStrBuilder* sb) {
    char* result = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return result;
}
