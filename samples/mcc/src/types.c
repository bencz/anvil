/*
 * MCC - Micro C Compiler
 * Type system implementation
 */

#include "anvil/anvil.h"
#include "mcc.h"

static const char *type_kind_names[] = {
    [TYPE_VOID]        = "void",
    [TYPE_CHAR]        = "char",
    [TYPE_SHORT]       = "short",
    [TYPE_INT]         = "int",
    [TYPE_LONG]        = "long",
    [TYPE_FLOAT]       = "float",
    [TYPE_DOUBLE]      = "double",
    [TYPE_LONG_DOUBLE] = "long double",
    [TYPE_POINTER]     = "pointer",
    [TYPE_ARRAY]       = "array",
    [TYPE_FUNCTION]    = "function",
    [TYPE_STRUCT]      = "struct",
    [TYPE_UNION]       = "union",
    [TYPE_ENUM]        = "enum",
    [TYPE_TYPEDEF]     = "typedef",
};

mcc_type_context_t *mcc_type_context_create(mcc_context_t *ctx)
{
    mcc_type_context_t *tctx = mcc_alloc(ctx, sizeof(mcc_type_context_t));
    tctx->ctx = ctx;
    
    /* Get architecture info from ANVIL using shared function */
    anvil_arch_t anvil_arch = mcc_arch_to_anvil(ctx->options.arch);
    const anvil_arch_info_t *arch_info = anvil_arch_get_info(anvil_arch);
    
    /* Get pointer and word size from architecture */
    int ptr_size = arch_info ? arch_info->ptr_size : 4;
    int word_size = arch_info ? arch_info->word_size : 4;
    
    /* Determine long size based on data model:
     * - ILP32 (32-bit): long = 4 bytes (x86, S/370, S/370-XA, S/390, PPC32)
     * - LP64 (64-bit Unix): long = 8 bytes (x86_64, z/Architecture, PPC64, ARM64)
     * - LLP64 (64-bit Windows): long = 4 bytes (would need ANVIL_ABI_WIN64 check)
     * 
     * Note: IBM mainframes (S/370, S/390) use ILP32 even with 24/31-bit addressing.
     * z/Architecture uses LP64 with 64-bit addressing.
     */
    int long_size = (ptr_size == 8) ? 8 : 4;
    
    /* Create primitive types */
    tctx->type_void = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_void->kind = TYPE_VOID;
    tctx->type_void->size = 0;
    tctx->type_void->align = 1;
    
    tctx->type_char = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_char->kind = TYPE_CHAR;
    tctx->type_char->size = 1;
    tctx->type_char->align = 1;
    
    tctx->type_schar = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_schar->kind = TYPE_CHAR;
    tctx->type_schar->is_unsigned = false;
    tctx->type_schar->size = 1;
    tctx->type_schar->align = 1;
    
    tctx->type_uchar = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_uchar->kind = TYPE_CHAR;
    tctx->type_uchar->is_unsigned = true;
    tctx->type_uchar->size = 1;
    tctx->type_uchar->align = 1;
    
    tctx->type_short = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_short->kind = TYPE_SHORT;
    tctx->type_short->size = 2;
    tctx->type_short->align = 2;
    
    tctx->type_ushort = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ushort->kind = TYPE_SHORT;
    tctx->type_ushort->is_unsigned = true;
    tctx->type_ushort->size = 2;
    tctx->type_ushort->align = 2;
    
    tctx->type_int = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_int->kind = TYPE_INT;
    tctx->type_int->size = 4;
    tctx->type_int->align = 4;
    
    tctx->type_uint = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_uint->kind = TYPE_INT;
    tctx->type_uint->is_unsigned = true;
    tctx->type_uint->size = 4;
    tctx->type_uint->align = 4;
    
    /* long size depends on architecture */
    tctx->type_long = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_long->kind = TYPE_LONG;
    tctx->type_long->size = long_size;
    tctx->type_long->align = long_size;
    
    tctx->type_ulong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ulong->kind = TYPE_LONG;
    tctx->type_ulong->is_unsigned = true;
    tctx->type_ulong->size = long_size;
    tctx->type_ulong->align = long_size;
    
    /* C99 long long types - always 8 bytes */
    tctx->type_llong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_llong->kind = TYPE_LONG_LONG;
    tctx->type_llong->size = 8;
    tctx->type_llong->align = (word_size >= 8) ? 8 : word_size;
    
    tctx->type_ullong = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ullong->kind = TYPE_LONG_LONG;
    tctx->type_ullong->is_unsigned = true;
    tctx->type_ullong->size = 8;
    tctx->type_ullong->align = (word_size >= 8) ? 8 : word_size;
    
    tctx->type_float = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_float->kind = TYPE_FLOAT;
    tctx->type_float->size = 4;
    tctx->type_float->align = 4;
    
    tctx->type_double = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_double->kind = TYPE_DOUBLE;
    tctx->type_double->size = 8;
    tctx->type_double->align = (word_size >= 8) ? 8 : word_size;
    
    tctx->type_ldouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_ldouble->kind = TYPE_LONG_DOUBLE;
    tctx->type_ldouble->size = 8;
    tctx->type_ldouble->align = (word_size >= 8) ? 8 : word_size;
    
    /* C99 Complex types (size is 2x the base type) */
    tctx->type_cfloat = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cfloat->kind = TYPE_COMPLEX_FLOAT;
    tctx->type_cfloat->size = 8;   /* 2 * sizeof(float) */
    tctx->type_cfloat->align = 4;
    
    tctx->type_cdouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cdouble->kind = TYPE_COMPLEX_DOUBLE;
    tctx->type_cdouble->size = 16;  /* 2 * sizeof(double) */
    tctx->type_cdouble->align = 8;
    
    tctx->type_cldouble = mcc_alloc(ctx, sizeof(mcc_type_t));
    tctx->type_cldouble->kind = TYPE_COMPLEX_LDOUBLE;
    tctx->type_cldouble->size = 16;  /* 2 * sizeof(long double) - platform dependent */
    tctx->type_cldouble->align = 8;
    
    /* Store pointer size for use in mcc_type_pointer */
    tctx->ptr_size = ptr_size;
    
    return tctx;
}

void mcc_type_context_destroy(mcc_type_context_t *tctx)
{
    (void)tctx; /* Arena allocated */
}

/* Primitive type getters */
mcc_type_t *mcc_type_void(mcc_type_context_t *tctx) { return tctx->type_void; }
mcc_type_t *mcc_type_char(mcc_type_context_t *tctx) { return tctx->type_char; }
mcc_type_t *mcc_type_schar(mcc_type_context_t *tctx) { return tctx->type_schar; }
mcc_type_t *mcc_type_uchar(mcc_type_context_t *tctx) { return tctx->type_uchar; }
mcc_type_t *mcc_type_short(mcc_type_context_t *tctx) { return tctx->type_short; }
mcc_type_t *mcc_type_ushort(mcc_type_context_t *tctx) { return tctx->type_ushort; }
mcc_type_t *mcc_type_int(mcc_type_context_t *tctx) { return tctx->type_int; }
mcc_type_t *mcc_type_uint(mcc_type_context_t *tctx) { return tctx->type_uint; }
mcc_type_t *mcc_type_long(mcc_type_context_t *tctx) { return tctx->type_long; }
mcc_type_t *mcc_type_ulong(mcc_type_context_t *tctx) { return tctx->type_ulong; }
mcc_type_t *mcc_type_llong(mcc_type_context_t *tctx) { return tctx->type_llong; }
mcc_type_t *mcc_type_ullong(mcc_type_context_t *tctx) { return tctx->type_ullong; }
mcc_type_t *mcc_type_float(mcc_type_context_t *tctx) { return tctx->type_float; }
mcc_type_t *mcc_type_double(mcc_type_context_t *tctx) { return tctx->type_double; }
mcc_type_t *mcc_type_long_double(mcc_type_context_t *tctx) { return tctx->type_ldouble; }
mcc_type_t *mcc_type_complex_float(mcc_type_context_t *tctx) { return tctx->type_cfloat; }
mcc_type_t *mcc_type_complex_double(mcc_type_context_t *tctx) { return tctx->type_cdouble; }
mcc_type_t *mcc_type_complex_ldouble(mcc_type_context_t *tctx) { return tctx->type_cldouble; }

/* Derived type constructors */
mcc_type_t *mcc_type_pointer(mcc_type_context_t *tctx, mcc_type_t *pointee)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_POINTER;
    type->data.pointer.pointee = pointee;
    type->size = tctx->ptr_size;  /* Use architecture-specific pointer size */
    type->align = tctx->ptr_size;
    return type;
}

mcc_type_t *mcc_type_array(mcc_type_context_t *tctx, mcc_type_t *element, size_t length)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_ARRAY;
    type->data.array.element = element;
    type->data.array.length = length;
    type->size = element->size * length;
    type->align = element->align;
    return type;
}

mcc_type_t *mcc_type_incomplete_array(mcc_type_context_t *tctx, mcc_type_t *element)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_ARRAY;
    type->data.array.element = element;
    type->data.array.length = 0;
    type->size = 0;
    type->align = element->align;
    return type;
}

mcc_type_t *mcc_type_function(mcc_type_context_t *tctx, mcc_type_t *return_type,
                               mcc_func_param_t *params, int num_params, bool variadic)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_FUNCTION;
    type->data.function.return_type = return_type;
    type->data.function.params = params;
    type->data.function.num_params = num_params;
    type->data.function.is_variadic = variadic;
    type->size = 0;
    type->align = 1;
    return type;
}

mcc_type_t *mcc_type_struct(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_STRUCT;
    type->data.record.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.record.is_complete = false;
    return type;
}

mcc_type_t *mcc_type_union(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_UNION;
    type->data.record.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.record.is_complete = false;
    return type;
}

mcc_type_t *mcc_type_enum(mcc_type_context_t *tctx, const char *tag)
{
    mcc_type_t *type = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    type->kind = TYPE_ENUM;
    type->data.enumeration.tag = tag ? mcc_strdup(tctx->ctx, tag) : NULL;
    type->data.enumeration.is_complete = false;
    type->size = 4; /* Enums are int-sized */
    type->align = 4;
    return type;
}

/* Type completion */
void mcc_type_complete_struct(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields)
{
    type->data.record.fields = fields;
    type->data.record.num_fields = num_fields;
    type->data.record.is_complete = true;
    
    /* Calculate size and alignment */
    size_t offset = 0;
    size_t max_align = 1;
    
    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        size_t align = f->type->align;
        if (align > max_align) max_align = align;
        
        /* Align offset */
        offset = (offset + align - 1) & ~(align - 1);
        f->offset = (int)offset;
        offset += f->type->size;
    }
    
    /* Final alignment */
    type->size = (offset + max_align - 1) & ~(max_align - 1);
    type->align = max_align;
}

void mcc_type_complete_union(mcc_type_t *type, mcc_struct_field_t *fields, int num_fields)
{
    type->data.record.fields = fields;
    type->data.record.num_fields = num_fields;
    type->data.record.is_complete = true;
    
    /* Calculate size and alignment (max of all fields) */
    size_t max_size = 0;
    size_t max_align = 1;
    
    for (mcc_struct_field_t *f = fields; f; f = f->next) {
        f->offset = 0;
        if (f->type->size > max_size) max_size = f->type->size;
        if (f->type->align > max_align) max_align = f->type->align;
    }
    
    type->size = (max_size + max_align - 1) & ~(max_align - 1);
    type->align = max_align;
}

void mcc_type_complete_enum(mcc_type_t *type)
{
    type->data.enumeration.is_complete = true;
}

/* Type qualifiers */
mcc_type_t *mcc_type_qualified(mcc_type_context_t *tctx, mcc_type_t *type, mcc_type_qual_t quals)
{
    if (type->qualifiers == quals) return type;
    
    mcc_type_t *qtype = mcc_alloc(tctx->ctx, sizeof(mcc_type_t));
    *qtype = *type;
    qtype->qualifiers = quals;
    return qtype;
}

mcc_type_t *mcc_type_unqualified(mcc_type_t *type)
{
    /* Return the type without qualifiers */
    /* For simplicity, we just clear qualifiers in place */
    type->qualifiers = QUAL_NONE;
    return type;
}

/* Type queries */
bool mcc_type_is_void(mcc_type_t *type)
{
    return type->kind == TYPE_VOID;
}

bool mcc_type_is_integer(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_LONG_LONG:
        case TYPE_BOOL:
        case TYPE_ENUM:
            return true;
        default:
            return false;
    }
}

bool mcc_type_is_floating(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return true;
        default:
            return false;
    }
}

bool mcc_type_is_arithmetic(mcc_type_t *type)
{
    return mcc_type_is_integer(type) || mcc_type_is_floating(type);
}

bool mcc_type_is_scalar(mcc_type_t *type)
{
    return mcc_type_is_arithmetic(type) || type->kind == TYPE_POINTER;
}

bool mcc_type_is_pointer(mcc_type_t *type)
{
    return type->kind == TYPE_POINTER;
}

bool mcc_type_is_array(mcc_type_t *type)
{
    return type->kind == TYPE_ARRAY;
}

bool mcc_type_is_function(mcc_type_t *type)
{
    return type->kind == TYPE_FUNCTION;
}

bool mcc_type_is_struct(mcc_type_t *type)
{
    return type->kind == TYPE_STRUCT;
}

bool mcc_type_is_union(mcc_type_t *type)
{
    return type->kind == TYPE_UNION;
}

bool mcc_type_is_record(mcc_type_t *type)
{
    return type->kind == TYPE_STRUCT || type->kind == TYPE_UNION;
}

bool mcc_type_is_enum(mcc_type_t *type)
{
    return type->kind == TYPE_ENUM;
}

bool mcc_type_is_aggregate(mcc_type_t *type)
{
    return mcc_type_is_array(type) || mcc_type_is_record(type);
}

bool mcc_type_is_complete(mcc_type_t *type)
{
    switch (type->kind) {
        case TYPE_VOID:
            return false;
        case TYPE_ARRAY:
            return type->data.array.length > 0;
        case TYPE_STRUCT:
        case TYPE_UNION:
            return type->data.record.is_complete;
        case TYPE_ENUM:
            return type->data.enumeration.is_complete;
        default:
            return true;
    }
}

bool mcc_type_is_compatible(mcc_type_t *a, mcc_type_t *b)
{
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case TYPE_POINTER:
            return mcc_type_is_compatible(a->data.pointer.pointee,
                                          b->data.pointer.pointee);
        case TYPE_ARRAY:
            if (a->data.array.length != 0 && b->data.array.length != 0 &&
                a->data.array.length != b->data.array.length) {
                return false;
            }
            return mcc_type_is_compatible(a->data.array.element,
                                          b->data.array.element);
        case TYPE_FUNCTION:
            /* Simplified check */
            return mcc_type_is_compatible(a->data.function.return_type,
                                          b->data.function.return_type);
        case TYPE_STRUCT:
        case TYPE_UNION:
            return a == b; /* Must be same type */
        default:
            return a->is_unsigned == b->is_unsigned;
    }
}

bool mcc_type_is_same(mcc_type_t *a, mcc_type_t *b)
{
    return a == b || (mcc_type_is_compatible(a, b) && 
                      a->qualifiers == b->qualifiers);
}

/* Type conversions */
mcc_type_t *mcc_type_promote(mcc_type_context_t *tctx, mcc_type_t *type)
{
    /* Integer promotion: char, short -> int */
    switch (type->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
            return type->is_unsigned ? tctx->type_uint : tctx->type_int;
        case TYPE_ENUM:
            return tctx->type_int;
        default:
            return type;
    }
}

mcc_type_t *mcc_type_common(mcc_type_context_t *tctx, mcc_type_t *a, mcc_type_t *b)
{
    /* Usual arithmetic conversions */
    a = mcc_type_promote(tctx, a);
    b = mcc_type_promote(tctx, b);
    
    /* If either is long double */
    if (a->kind == TYPE_LONG_DOUBLE || b->kind == TYPE_LONG_DOUBLE) {
        return tctx->type_ldouble;
    }
    
    /* If either is double */
    if (a->kind == TYPE_DOUBLE || b->kind == TYPE_DOUBLE) {
        return tctx->type_double;
    }
    
    /* If either is float */
    if (a->kind == TYPE_FLOAT || b->kind == TYPE_FLOAT) {
        return tctx->type_float;
    }
    
    /* Both are integers */
    if (a->kind == TYPE_LONG || b->kind == TYPE_LONG) {
        if (a->is_unsigned || b->is_unsigned) {
            return tctx->type_ulong;
        }
        return tctx->type_long;
    }
    
    if (a->is_unsigned || b->is_unsigned) {
        return tctx->type_uint;
    }
    
    return tctx->type_int;
}

mcc_type_t *mcc_type_decay(mcc_type_context_t *tctx, mcc_type_t *type)
{
    /* Array decays to pointer */
    if (type->kind == TYPE_ARRAY) {
        return mcc_type_pointer(tctx, type->data.array.element);
    }
    
    /* Function decays to pointer to function */
    if (type->kind == TYPE_FUNCTION) {
        return mcc_type_pointer(tctx, type);
    }
    
    return type;
}

/* Type utilities */
const char *mcc_type_kind_name(mcc_type_kind_t kind)
{
    if (kind < TYPE_COUNT) {
        return type_kind_names[kind];
    }
    return "unknown";
}

char *mcc_type_to_string(mcc_type_t *type)
{
    static char buf[256];
    char *p = buf;
    
    if (type->qualifiers & QUAL_CONST) {
        p += sprintf(p, "const ");
    }
    if (type->qualifiers & QUAL_VOLATILE) {
        p += sprintf(p, "volatile ");
    }
    
    switch (type->kind) {
        case TYPE_VOID:
            p += sprintf(p, "void");
            break;
        case TYPE_CHAR:
            if (type->is_unsigned) p += sprintf(p, "unsigned ");
            p += sprintf(p, "char");
            break;
        case TYPE_SHORT:
            if (type->is_unsigned) p += sprintf(p, "unsigned ");
            p += sprintf(p, "short");
            break;
        case TYPE_INT:
            if (type->is_unsigned) p += sprintf(p, "unsigned ");
            p += sprintf(p, "int");
            break;
        case TYPE_LONG:
            if (type->is_unsigned) p += sprintf(p, "unsigned ");
            p += sprintf(p, "long");
            break;
        case TYPE_FLOAT:
            p += sprintf(p, "float");
            break;
        case TYPE_DOUBLE:
            p += sprintf(p, "double");
            break;
        case TYPE_LONG_DOUBLE:
            p += sprintf(p, "long double");
            break;
        case TYPE_POINTER:
            p += sprintf(p, "%s *", mcc_type_to_string(type->data.pointer.pointee));
            break;
        case TYPE_ARRAY:
            p += sprintf(p, "%s[%zu]", mcc_type_to_string(type->data.array.element),
                         type->data.array.length);
            break;
        case TYPE_STRUCT:
            p += sprintf(p, "struct %s", type->data.record.tag ? type->data.record.tag : "(anonymous)");
            break;
        case TYPE_UNION:
            p += sprintf(p, "union %s", type->data.record.tag ? type->data.record.tag : "(anonymous)");
            break;
        case TYPE_ENUM:
            p += sprintf(p, "enum %s", type->data.enumeration.tag ? type->data.enumeration.tag : "(anonymous)");
            break;
        case TYPE_FUNCTION:
            p += sprintf(p, "%s ()", mcc_type_to_string(type->data.function.return_type));
            break;
        default:
            p += sprintf(p, "?");
            break;
    }
    
    return buf;
}

size_t mcc_type_sizeof(mcc_type_t *type)
{
    return type->size;
}

size_t mcc_type_alignof(mcc_type_t *type)
{
    return type->align;
}

mcc_struct_field_t *mcc_type_find_field(mcc_type_t *type, const char *name)
{
    if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
        return NULL;
    }
    
    for (mcc_struct_field_t *f = type->data.record.fields; f; f = f->next) {
        /* Skip anonymous fields (bitfield padding) */
        if (!f->name) continue;
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    
    return NULL;
}
