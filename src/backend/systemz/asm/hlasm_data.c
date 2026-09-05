#include "../systemz_internal.h"

static void systemz_emit_data_int(anvil_strbuf_t *out, size_t size, int64_t value)
{
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return;
    uint64_t bits = (uint64_t)value;
    if (size < 8)
        bits &= (UINT64_C(1) << (size * 8)) - 1;
    anvil_strbuf_appendf(out, "DC    X'%0*llX'\n", (int)(size * 2), (unsigned long long)bits);
}

static void systemz_emit_data_zero(anvil_strbuf_t *out, size_t size)
{
    if (size == 0)
        return;
    anvil_strbuf_appendf(out, "DC    %zuX'00'\n", size);
}

static void systemz_emit_decimal_initializer(anvil_strbuf_t *out, anvil_type_t *type, anvil_value_t *init)
{
    const char *digits = anvil_const_decimal_digits(init);
    if (!digits)
        digits = "0";
    if (anvil_type_decimal_encoding(type) == ANVIL_DECIMAL_PACKED) {
        anvil_strbuf_appendf(out, "DC    PL%zu'%s'\n", anvil_type_size(type), digits);
    } else {
        anvil_strbuf_appendf(out, "DC    ZL%zu'%s'\n", anvil_type_size(type), digits);
    }
}

typedef struct {
    const anvil_value_t *value;
    char label[16];
} systemz_global_string_entry_t;

typedef struct {
    systemz_global_string_entry_t *entries;
    size_t count;
    size_t capacity;
} systemz_global_string_pool_t;

static const char *systemz_global_string_intern(systemz_global_string_pool_t *pool, const anvil_value_t *value)
{
    if (!pool || !value || value->kind != ANVIL_VAL_CONST_STRING || !value->data.str)
        return NULL;
    for (size_t i = 0; i < pool->count; i++)
        if (pool->entries[i].value == value)
            return pool->entries[i].label;
    if (pool->count == pool->capacity) {
        size_t capacity = pool->capacity ? pool->capacity * 2 : 8;
        if (capacity < pool->capacity || capacity > SIZE_MAX / sizeof(*pool->entries))
            return NULL;
        systemz_global_string_entry_t *entries = realloc(pool->entries, capacity * sizeof(*entries));
        if (!entries)
            return NULL;
        pool->entries = entries;
        pool->capacity = capacity;
    }
    systemz_global_string_entry_t *entry = &pool->entries[pool->count];
    entry->value = value;
    int length = snprintf(entry->label, sizeof(entry->label), "AVG%05zu", pool->count);
    if (length < 0 || (size_t)length >= sizeof(entry->label))
        return NULL;
    pool->count++;
    return entry->label;
}

static bool systemz_emit_global_strings(anvil_strbuf_t *out, const systemz_global_string_pool_t *pool)
{
    if (!out || !pool)
        return false;
    for (size_t i = 0; i < pool->count; i++) {
        const unsigned char *bytes = (const unsigned char *)pool->entries[i].value->data.str;
        size_t length = strlen((const char *)bytes) + 1;
        size_t offset = 0;
        bool first = true;
        while (offset < length) {
            size_t chunk = length - offset;
            if (chunk > 24)
                chunk = 24;
            if (first)
                anvil_strbuf_appendf(out, "%-8s DC    X'", pool->entries[i].label);
            else
                anvil_strbuf_append(out, "         DC    X'");
            for (size_t b = 0; b < chunk; b++)
                anvil_strbuf_appendf(out, "%02X", bytes[offset + b]);
            anvil_strbuf_append(out, "'\n");
            first = false;
            offset += chunk;
        }
    }
    return !out->failed;
}

static bool systemz_emit_global_initializer(anvil_strbuf_t *out, anvil_type_t *type, anvil_value_t *init, anvil_fp_format_t fp_format, size_t pointer_size, systemz_global_string_pool_t *strings)
{
    if (!out || !type || type->size == 0)
        return false;
    if (!init) {
        systemz_emit_data_zero(out, type->size);
        return !out->failed;
    }
    if (!init->type || !anvil_types_equal(type, init->type))
        return false;

    switch (init->kind) {
    case ANVIL_VAL_CONST_INT:
        if (!anvil_type_is_integer(type))
            return false;
        systemz_emit_data_int(out, type->size, type->is_signed ? init->data.i : (int64_t)init->data.u);
        return !out->failed;
    case ANVIL_VAL_CONST_FLOAT:
        if (type->kind == ANVIL_TYPE_F32) {
            anvil_strbuf_appendf(out, "DC    %s'%g'\n", fp_format == ANVIL_FP_IEEE754 || fp_format == ANVIL_FP_HFP_IEEE ? "EB" : "E", init->data.f);
        } else if (type->kind == ANVIL_TYPE_F64) {
            anvil_strbuf_appendf(out, "DC    %s'%g'\n", fp_format == ANVIL_FP_IEEE754 || fp_format == ANVIL_FP_HFP_IEEE ? "DB" : "D", init->data.f);
        } else
            return false;
        return !out->failed;
    case ANVIL_VAL_CONST_DECIMAL:
        if (type->kind != ANVIL_TYPE_DECIMAL)
            return false;
        systemz_emit_decimal_initializer(out, type, init);
        return !out->failed;
    case ANVIL_VAL_CONST_NULL:
        if (type->kind != ANVIL_TYPE_PTR || type->size != pointer_size)
            return false;
        systemz_emit_data_zero(out, pointer_size);
        return !out->failed;
    case ANVIL_VAL_CONST_STRING: {
        if (type->kind != ANVIL_TYPE_PTR || type->size != pointer_size)
            return false;
        const char *label = systemz_global_string_intern(strings, init);
        if (!label)
            return false;
        anvil_strbuf_appendf(out, "DC    %s(%s)\n", pointer_size == 8 ? "AD" : "A", label);
        return !out->failed;
    }
    case ANVIL_VAL_CONST_SYMBOL_ADDR:
    case ANVIL_VAL_CONST_GEP: {
        if (type->kind != ANVIL_TYPE_PTR || type->size != pointer_size || !init->data.reloc.symbol || !init->data.reloc.symbol->name)
            return false;
        char upper[96];
        systemz_uppercase(upper, init->data.reloc.symbol->name, sizeof(upper));
        anvil_strbuf_appendf(out, "DC    %s(%s", pointer_size == 8 ? "AD" : "A", upper);
        if (init->data.reloc.addend > 0)
            anvil_strbuf_appendf(out, "+%lld", (long long)init->data.reloc.addend);
        else if (init->data.reloc.addend < 0) {
            uint64_t magnitude = UINT64_C(0) - (uint64_t)init->data.reloc.addend;
            anvil_strbuf_appendf(out, "-%llu", (unsigned long long)magnitude);
        }
        anvil_strbuf_append(out, ")\n");
        return !out->failed;
    }
    case ANVIL_VAL_CONST_ARRAY:
        if (type->kind != ANVIL_TYPE_ARRAY || init->data.aggregate.num_elements != type->data.array.count)
            return false;
        for (size_t i = 0; i < type->data.array.count; i++) {
            if (!systemz_emit_global_initializer(out, type->data.array.elem, init->data.aggregate.elements[i], fp_format, pointer_size, strings))
                return false;
        }
        return true;
    case ANVIL_VAL_CONST_STRUCT: {
        if (type->kind != ANVIL_TYPE_STRUCT || !type->data.struc.complete || init->data.aggregate.num_elements != type->data.struc.num_fields)
            return false;
        size_t cursor = 0;
        for (size_t i = 0; i < type->data.struc.num_fields; i++) {
            size_t offset = type->data.struc.offsets[i];
            anvil_type_t *field = type->data.struc.fields[i];
            if (!field || offset < cursor || field->size > type->size - offset)
                return false;
            systemz_emit_data_zero(out, offset - cursor);
            if (!systemz_emit_global_initializer(out, field, init->data.aggregate.elements[i], fp_format, pointer_size, strings))
                return false;
            cursor = offset + field->size;
        }
        if (cursor > type->size)
            return false;
        systemz_emit_data_zero(out, type->size - cursor);
        return !out->failed;
    }
    case ANVIL_VAL_GLOBAL:
    case ANVIL_VAL_FUNC:
    case ANVIL_VAL_PARAM:
    case ANVIL_VAL_INSTR:
    case ANVIL_VAL_BLOCK:
        return false;
    }
    return false;
}

bool systemz_emit_globals(anvil_strbuf_t *out, anvil_module_t *mod, anvil_fp_format_t fp_format, size_t pointer_size)
{
    if (!mod || !out || (pointer_size != 4 && pointer_size != 8))
        return false;
    if (!mod->globals)
        return true;
    systemz_global_string_pool_t strings = {0};
    anvil_strbuf_append(out, "         LTORG\n");
    anvil_strbuf_append(out, "         DS    0D\n");
    for (anvil_global_t *g = mod->globals; g; g = g->next) {
        if (!g->value || !g->value->name || !g->value->type) {
            free(strings.entries);
            return false;
        }
        char upper[96];
        systemz_uppercase(upper, g->value->name, sizeof(upper));
        if (g->value->data.global.is_declaration) {
            anvil_strbuf_appendf(out, "         EXTRN %s\n", upper);
            continue;
        }
        if (g->value->data.global.linkage == ANVIL_LINK_WEAK) {
            free(strings.entries);
            return false;
        }
        if (g->value->data.global.linkage == ANVIL_LINK_EXTERNAL || g->value->data.global.linkage == ANVIL_LINK_COMMON)
            anvil_strbuf_appendf(out, "         ENTRY %s\n", upper);
        anvil_strbuf_appendf(out, "%-8s ", upper);
        if (!systemz_emit_global_initializer(out, g->value->type, g->value->data.global.init, fp_format, pointer_size, &strings)) {
            free(strings.entries);
            return false;
        }
    }
    bool ok = systemz_emit_global_strings(out, &strings);
    free(strings.entries);
    return ok && !out->failed;
}
