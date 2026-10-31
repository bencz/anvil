/* GNU assembler data emission shared by x86, PowerPC and AArch64.
 * Layout comes from the target's types; symbol spelling is supplied by callback.
 * This internal interface owns no calling convention or host-platform policy.
 */
#ifndef ANVIL_BACKEND_GNU_DATA_H
#define ANVIL_BACKEND_GNU_DATA_H

#include "anvil/anvil_internal.h"
#include "obj_format.h"

typedef struct {
    const anvil_value_t *value;
    char label[48];
} anvil_gnu_string_entry_t;

typedef struct {
    anvil_gnu_string_entry_t *entries;
    size_t count;
    size_t capacity;
    const char *label_prefix;
} anvil_gnu_string_pool_t;

typedef bool (*anvil_gnu_format_symbol_fn)(char *buffer, size_t capacity, const anvil_value_t *symbol, const char *default_prefix, void *user);

void anvil_gnu_string_pool_init(anvil_gnu_string_pool_t *pool, const char *label_prefix);
void anvil_gnu_string_pool_destroy(anvil_gnu_string_pool_t *pool);
bool anvil_gnu_string_pool_emit(anvil_strbuf_t *out, const anvil_gnu_string_pool_t *pool, const char *section);
bool anvil_gnu_emit_constant(anvil_strbuf_t *out, const anvil_type_t *type, const anvil_value_t *value, size_t pointer_size, const char *symbol_prefix, anvil_gnu_string_pool_t *strings,
                             anvil_gnu_format_symbol_fn format_symbol, void *format_user);

/* What a backend supplies to emit a module's global variables. */
typedef struct {
    const anvil_obj_format_t *format;
    const char *symbol_prefix;
    size_t pointer_size;
    /* The target's alignment of a type, in bytes. */
    size_t (*type_align)(const anvil_type_t *type);
    anvil_gnu_format_symbol_fn format_symbol; /* NULL: prefix + name */
    void *format_user;
} anvil_gnu_globals_config_t;

/* Every non-function global of `mod`: binding by linkage, uncapped
 * alignment, ELF `.type`/`.size`, zero-initialized objects in the
 * zero-fill section where the format has a plain one, then the pooled string
 * literals in the format's string section. */
bool anvil_gnu_emit_globals(anvil_strbuf_t *out, const anvil_module_t *mod, const anvil_gnu_globals_config_t *config);

#endif
