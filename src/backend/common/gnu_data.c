#include "gnu_data.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void anvil_gnu_string_pool_init(anvil_gnu_string_pool_t *pool, const char *label_prefix)
{
    if (!pool)
        return;
    memset(pool, 0, sizeof(*pool));
    pool->label_prefix = label_prefix;
}

void anvil_gnu_string_pool_destroy(anvil_gnu_string_pool_t *pool)
{
    if (!pool)
        return;
    free(pool->entries);
    memset(pool, 0, sizeof(*pool));
}

static const char *anvil_gnu_string_pool_intern(anvil_gnu_string_pool_t *pool, const anvil_value_t *value)
{
    if (!pool || !pool->label_prefix || !value || value->kind != ANVIL_VAL_CONST_STRING || !value->data.str)
        return NULL;
    for (size_t i = 0; i < pool->count; i++)
        if (pool->entries[i].value == value)
            return pool->entries[i].label;
    if (pool->count == pool->capacity) {
        size_t capacity = pool->capacity ? pool->capacity * 2 : 8;
        if (capacity < pool->capacity || capacity > SIZE_MAX / sizeof(*pool->entries))
            return NULL;
        anvil_gnu_string_entry_t *entries = realloc(pool->entries, capacity * sizeof(*entries));
        if (!entries)
            return NULL;
        pool->entries = entries;
        pool->capacity = capacity;
    }
    anvil_gnu_string_entry_t *entry = &pool->entries[pool->count];
    entry->value = value;
    int length = snprintf(entry->label, sizeof(entry->label), "%s%zu", pool->label_prefix, pool->count);
    if (length < 0 || (size_t)length >= sizeof(entry->label))
        return NULL;
    pool->count++;
    return entry->label;
}

bool anvil_gnu_string_pool_emit(anvil_strbuf_t *out, const anvil_gnu_string_pool_t *pool, const char *section)
{
    if (!out || !pool)
        return false;
    if (pool->count == 0)
        return true;
    if (section && section[0])
        anvil_strbuf_appendf(out, "%s\n", section);
    for (size_t i = 0; i < pool->count; i++) {
        anvil_strbuf_appendf(out, "%s:\n\t.asciz \"", pool->entries[i].label);
        const unsigned char *p = (const unsigned char *)pool->entries[i].value->data.str;
        for (; *p; p++) {
            switch (*p) {
            case '\n':
                anvil_strbuf_append(out, "\\n");
                break;
            case '\r':
                anvil_strbuf_append(out, "\\r");
                break;
            case '\t':
                anvil_strbuf_append(out, "\\t");
                break;
            case '\\':
                anvil_strbuf_append(out, "\\\\");
                break;
            case '"':
                anvil_strbuf_append(out, "\\\"");
                break;
            default:
                if (*p < 0x20 || *p >= 0x7f)
                    anvil_strbuf_appendf(out, "\\%03o", *p);
                else
                    anvil_strbuf_append_char(out, (char)*p);
                break;
            }
        }
        anvil_strbuf_append(out, "\"\n");
    }
    return !out->failed;
}

static bool anvil_gnu_emit_zero(anvil_strbuf_t *out, size_t size)
{
    if (!out || size == 0)
        return out != NULL;
    anvil_strbuf_appendf(out, "\t.zero %zu\n", size);
    return !out->failed;
}

static bool anvil_gnu_emit_integer(anvil_strbuf_t *out, size_t size, uint64_t bits)
{
    const char *directive;
    switch (size) {
    case 1:
        directive = ".byte";
        break;
    case 2:
        directive = ".short";
        break;
    case 4:
        directive = ".long";
        break;
    case 8:
        directive = ".quad";
        break;
    default:
        return false;
    }
    if (size < 8)
        bits &= (UINT64_C(1) << (size * 8)) - 1;
    anvil_strbuf_appendf(out, "\t%s 0x%" PRIx64 "\n", directive, bits);
    return !out->failed;
}

static bool anvil_gnu_emit_reloc(anvil_strbuf_t *out, const anvil_value_t *value, size_t pointer_size, const char *symbol_prefix, anvil_gnu_format_symbol_fn format_symbol, void *format_user)
{
    if (!value || !value->type || value->type->kind != ANVIL_TYPE_PTR || (pointer_size != 4 && pointer_size != 8) || !value->data.reloc.symbol || !value->data.reloc.symbol->name) {
        return false;
    }
    const char *directive = pointer_size == 4 ? ".long" : ".quad";
    int64_t addend = value->data.reloc.addend;
    char formatted[320];
    if (format_symbol) {
        if (!format_symbol(formatted, sizeof(formatted), value->data.reloc.symbol, symbol_prefix, format_user))
            return false;
    } else {
        int length = snprintf(formatted, sizeof(formatted), "%s%s", symbol_prefix ? symbol_prefix : "", value->data.reloc.symbol->name);
        if (length < 0 || (size_t)length >= sizeof(formatted))
            return false;
    }
    anvil_strbuf_appendf(out, "\t%s %s", directive, formatted);
    if (addend > 0) {
        anvil_strbuf_appendf(out, "+%" PRId64, addend);
    } else if (addend < 0) {
        /* Printing -(INT64_MIN) would overflow.  Convert its magnitude in
           unsigned arithmetic instead. */
        uint64_t magnitude = UINT64_C(0) - (uint64_t)addend;
        anvil_strbuf_appendf(out, "-%" PRIu64, magnitude);
    }
    anvil_strbuf_append(out, "\n");
    return !out->failed;
}

bool anvil_gnu_emit_constant(anvil_strbuf_t *out, const anvil_type_t *type, const anvil_value_t *value, size_t pointer_size, const char *symbol_prefix, anvil_gnu_string_pool_t *strings,
                             anvil_gnu_format_symbol_fn format_symbol, void *format_user)
{
    if (!out || !type || type->size == 0)
        return false;
    if (!value)
        return anvil_gnu_emit_zero(out, type->size);
    if (!value->type || !anvil_types_equal(type, value->type))
        return false;

    switch (value->kind) {
    case ANVIL_VAL_CONST_INT:
        return anvil_gnu_emit_integer(out, type->size, type->is_signed ? (uint64_t)value->data.i : value->data.u);
    case ANVIL_VAL_CONST_FLOAT: {
        if (type->kind == ANVIL_TYPE_F32 && type->size == 4) {
            float f = (float)value->data.f;
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            return anvil_gnu_emit_integer(out, 4, bits);
        }
        if (type->kind == ANVIL_TYPE_F64 && type->size == 8) {
            uint64_t bits;
            memcpy(&bits, &value->data.f, sizeof(bits));
            return anvil_gnu_emit_integer(out, 8, bits);
        }
        return false;
    }
    case ANVIL_VAL_CONST_NULL:
        return type->kind == ANVIL_TYPE_PTR && type->size == pointer_size && anvil_gnu_emit_zero(out, pointer_size);
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP:
        return type->size == pointer_size && anvil_gnu_emit_reloc(out, value, pointer_size, symbol_prefix, format_symbol, format_user);
    case ANVIL_VAL_CONST_STRING: {
        if (type->kind != ANVIL_TYPE_PTR || type->size != pointer_size)
            return false;
        const char *label = anvil_gnu_string_pool_intern(strings, value);
        if (!label)
            return false;
        anvil_strbuf_appendf(out, "\t%s %s\n", pointer_size == 4 ? ".long" : ".quad", label);
        return !out->failed;
    }
    case ANVIL_VAL_CONST_ARRAY:
        if (type->kind != ANVIL_TYPE_ARRAY || value->data.aggregate.num_elements != type->data.array.count) {
            return false;
        }
        for (size_t i = 0; i < type->data.array.count; i++) {
            if (!anvil_gnu_emit_constant(out, type->data.array.elem, value->data.aggregate.elements[i], pointer_size, symbol_prefix, strings, format_symbol, format_user))
                return false;
        }
        return true;
    case ANVIL_VAL_CONST_STRUCT: {
        if (type->kind == ANVIL_TYPE_STRUCT && type->data.struc.complete && type->data.struc.is_union) {
            /* The chosen member at offset 0, then zeros. */
            size_t member = value->data.aggregate.union_member;
            if (value->data.aggregate.num_elements != 1 || member >= type->data.struc.num_fields)
                return false;
            const anvil_type_t *member_type = type->data.struc.fields[member];
            return member_type->size <= type->size &&
                   anvil_gnu_emit_constant(out, member_type, value->data.aggregate.elements[0], pointer_size, symbol_prefix, strings, format_symbol, format_user) &&
                   anvil_gnu_emit_zero(out, type->size - member_type->size);
        }
        if (type->kind != ANVIL_TYPE_STRUCT || !type->data.struc.complete || value->data.aggregate.num_elements != type->data.struc.num_fields)
            return false;
        size_t cursor = 0;
        for (size_t i = 0; i < type->data.struc.num_fields; i++) {
            size_t offset = type->data.struc.offsets[i];
            const anvil_type_t *field_type = type->data.struc.fields[i];
            if (!field_type || offset < cursor || field_type->size > type->size - offset)
                return false;
            if (!anvil_gnu_emit_zero(out, offset - cursor) ||
                !anvil_gnu_emit_constant(out, field_type, value->data.aggregate.elements[i], pointer_size, symbol_prefix, strings, format_symbol, format_user))
                return false;
            cursor = offset + field_type->size;
        }
        return cursor <= type->size && anvil_gnu_emit_zero(out, type->size - cursor);
    }
    case ANVIL_VAL_CONST_DECIMAL:
    case ANVIL_VAL_GLOBAL:
    case ANVIL_VAL_FUNC:
    case ANVIL_VAL_PARAM:
    case ANVIL_VAL_INSTR:
    case ANVIL_VAL_BLOCK:
        return false;
    }
    return false;
}

/* Whether a constant's image holds addresses the loader must relocate. */
static bool gnu_constant_needs_relocation(const anvil_value_t *value)
{
    if (!value)
        return false;
    switch (value->kind) {
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP:
        return true;
    case ANVIL_VAL_CONST_STRING:
        return value->type && value->type->kind == ANVIL_TYPE_PTR;
    case ANVIL_VAL_CONST_ARRAY:
    case ANVIL_VAL_CONST_STRUCT:
        for (size_t i = 0; i < value->data.aggregate.num_elements; i++)
            if (gnu_constant_needs_relocation(value->data.aggregate.elements[i]))
                return true;
        return false;
    default:
        return false;
    }
}

bool anvil_gnu_emit_globals(anvil_strbuf_t *out, const anvil_module_t *mod, const anvil_gnu_globals_config_t *config)
{
    if (!out || !mod || !config || !config->format)
        return false;
    const anvil_obj_format_t *format = config->format;
    const char *prefix = config->symbol_prefix ? config->symbol_prefix : "";
    anvil_gnu_string_pool_t strings;
    anvil_gnu_string_pool_init(&strings, ".Lanvil_global_string_");

    const char *section = NULL;
    bool ok = true;
    for (const anvil_global_t *g = mod->globals; ok && g; g = g->next) {
        const anvil_value_t *global = g->value;
        if (!global || (global->type && global->type->kind == ANVIL_TYPE_FUNC))
            continue;
        if (!global->name || !global->type) {
            ok = false;
            break;
        }
        char symbol[512];
        snprintf(symbol, sizeof(symbol), "%s%s", prefix, global->name);
        if (global->data.global.is_declaration) {
            anvil_strbuf_appendf(out, "\t.extern %s\n", symbol);
            continue;
        }
        size_t align = config->type_align ? config->type_align(global->type) : anvil_type_align(global->type);
        if (!align)
            align = 1;
        /* COMMON: an uninitialized tentative definition the linker merges. */
        if (global->data.global.linkage == ANVIL_LINK_COMMON && !global->data.global.init && !global->data.global.is_constant) {
            unsigned log2 = 0;
            while (((size_t)1 << log2) < align)
                log2++;
            anvil_strbuf_appendf(out, "\t.comm %s, %zu, %zu\n", symbol, global->type->size, format->comm_align_log2 ? (size_t)log2 : align);
            continue;
        }
        const char *wanted = global->data.global.is_constant
                                 ? (gnu_constant_needs_relocation(global->data.global.init) ? format->relro_section : format->rodata_section)
                                 : (!global->data.global.init && format->plain_bss ? format->bss_section : format->data_section);
        if (section != wanted) {
            anvil_strbuf_append(out, wanted);
            section = wanted;
        }
        anvil_obj_emit_binding(out, format, global->data.global.linkage, symbol);
        anvil_obj_emit_align(out, align);
        if (format->symbol_type_size)
            anvil_strbuf_appendf(out, "\t.type %s, %%object\n", symbol);
        anvil_strbuf_appendf(out, "%s:\n", symbol);
        ok = anvil_gnu_emit_constant(out, global->type, global->data.global.init, config->pointer_size, prefix, &strings, config->format_symbol,
                                     config->format_user);
        if (ok && format->symbol_type_size)
            anvil_strbuf_appendf(out, "\t.size %s, %zu\n", symbol, global->type->size);
    }
    if (ok && section)
        anvil_strbuf_append(out, "\n");
    if (ok) {
        char string_section[160];
        snprintf(string_section, sizeof(string_section), "%s", format->cstring_section);
        size_t length = strlen(string_section);
        if (length && string_section[length - 1] == '\n')
            string_section[length - 1] = '\0';
        ok = anvil_gnu_string_pool_emit(out, &strings, string_section);
    }
    anvil_gnu_string_pool_destroy(&strings);
    return ok && !out->failed;
}
