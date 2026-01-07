#ifndef ANVIL_STR_H
#define ANVIL_STR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool anvil_str_eq(const char* a, const char* b);
bool anvil_str_starts_with(const char* str, const char* prefix);
bool anvil_str_ends_with(const char* str, const char* suffix);
size_t anvil_str_hash(const char* str);
uint32_t anvil_str_hash32(const char* str);

typedef struct AnvilStrBuilder {
    char* data;
    size_t len;
    size_t cap;
} AnvilStrBuilder;

void anvil_str_builder_init(AnvilStrBuilder* sb);
void anvil_str_builder_free(AnvilStrBuilder* sb);
void anvil_str_builder_clear(AnvilStrBuilder* sb);
void anvil_str_builder_append(AnvilStrBuilder* sb, const char* str);
void anvil_str_builder_append_char(AnvilStrBuilder* sb, char c);
void anvil_str_builder_appendf(AnvilStrBuilder* sb, const char* fmt, ...);
void anvil_str_builder_append_int(AnvilStrBuilder* sb, int64_t val);
void anvil_str_builder_append_uint(AnvilStrBuilder* sb, uint64_t val);
void anvil_str_builder_append_hex(AnvilStrBuilder* sb, uint64_t val);
const char* anvil_str_builder_cstr(AnvilStrBuilder* sb);
char* anvil_str_builder_take(AnvilStrBuilder* sb);

#ifdef __cplusplus
}
#endif

#endif
